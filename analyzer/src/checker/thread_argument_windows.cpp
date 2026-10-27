// Copyright and license: see data_race.hpp.
#include <ikos/analyzer/checker/thread_argument_windows.hpp>
#include <ikos/analyzer/checker/data_race.hpp>
#include <ikos/analyzer/checker/thread_argument_window_consumer.hpp>
#include <ikos/analyzer/analysis/call_context.hpp>
#include <ikos/analyzer/analysis/execution_engine/native_thread_handles.hpp>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <ikos/analyzer/analysis/source_call_binding.hpp>
#include <algorithm>
#include <cstdlib>
#include <set>

namespace ikos {
namespace analyzer {
namespace {
using Z = core::ZNumber;
using Interval = core::machine_int::Interval;
using Unary = core::machine_int::UnaryOperator;
using Binary = core::machine_int::BinaryOperator;

bool reaches(ar::BasicBlock* start, ar::BasicBlock* target) {
  std::vector< ar::BasicBlock* > pending{start};
  std::set< ar::BasicBlock* > seen;
  while (!pending.empty()) {
    auto* block = pending.back(); pending.pop_back();
    if (block == target || seen.size() >= 4096) return true;
    if (!seen.insert(block).second) continue;
    pending.insert(pending.end(), block->successor_begin(), block->successor_end());
  }
  return false;
}
ar::Value* uncast(ar::Value* value) {
  for (unsigned n = 0; n < 32; ++n) {
    auto* var = dyn_cast< ar::InternalVariable >(value);
    auto* cast = var ? dyn_cast_or_null< ar::UnaryOperation >(unique_def(var)) : nullptr;
    if (!cast || cast->op() != ar::UnaryOperation::Bitcast ||
        !cast->operand()->type()->is_pointer()) return value;
    value = cast->operand();
  }
  return nullptr;
}
bool singleton(const Interval& interval, const Z& expected) {
  auto value = interval.singleton();
  return value && value->to_z_number() == expected;
}

// Source mode deliberately carries no numeric anchor or scratch variables.
// Pointer equations come from the imported/source operations; intervals only
// discharge value-preservation and modular-span premises.
bool pointer_binding(ar::Value* value, llvm::Value* source) {
  for (unsigned depth = 0; depth < 32; ++depth) {
    if (!value || !value->type()->is_pointer() || !source ||
        !source->getType()->isPointerTy() ||
        source->getType()->getPointerAddressSpace() != 0) return false;
    if (auto* cast = llvm::dyn_cast< llvm::BitCastOperator >(source)) {
      source = cast->getOperand(0); continue;
    }
    auto* variable = dyn_cast< ar::InternalVariable >(value);
    if (!variable) return false;
    if (variable->has_frontend() &&
        variable->frontend< llvm::Value >() == source) return true;
    auto* definition = unique_def(variable);
    if (auto* copy = dyn_cast_or_null< ar::Assignment >(definition)) value = copy->operand();
    else if (auto* cast = dyn_cast_or_null< ar::UnaryOperation >(definition)) {
      if (cast->op() != ar::UnaryOperation::Bitcast) return false;
      value = cast->operand();
    } else return false;
  }
  return false;
}
bool fits_nonnegative(const Z& upper, unsigned bits, core::Signedness sign) {
  return bits && bits <= 64 && upper <= core::MachineInt::max(bits, sign).to_z_number();
}
// Return every intermediate constant displacement, in origin-to-use order.
// Requiring unique definitions excludes loads, phi merges and helper returns.
bool ar_residual(ar::Value* pointer, ar::Function* function, std::vector< Z >& path) {
  std::vector< Z > reverse;
  for (unsigned depth = 0; depth < 32; ++depth) {
    if (pointer == function->param(0)) {
      Z total(0); path.push_back(total);
      for (auto it = reverse.rbegin(); it != reverse.rend(); ++it) {
        total += *it; path.push_back(total);
      }
      return true;
    }
    auto* variable = dyn_cast< ar::InternalVariable >(pointer);
    if (!variable || variable->code() != function->body() ||
        !variable->type()->is_pointer()) return false;
    auto* definition = unique_def(variable);
    if (auto* copy = dyn_cast_or_null< ar::Assignment >(definition)) pointer = copy->operand();
    else if (auto* cast = dyn_cast_or_null< ar::UnaryOperation >(definition)) {
      if (cast->op() != ar::UnaryOperation::Bitcast ||
          !cast->operand()->type()->is_pointer()) return false;
      pointer = cast->operand();
    } else if (auto* shift = dyn_cast_or_null< ar::PointerShift >(definition)) {
      if (shift->num_terms() > 32) return false;
      Z displacement(0);
      for (auto it = shift->term_begin(); it != shift->term_end(); ++it) {
        auto term = *it;
        auto* constant = dyn_cast< ar::IntegerConstant >(term.second);
        if (!constant) return false;
        displacement += term.first.to_z_number() * constant->value().to_z_number();
      }
      reverse.push_back(displacement); pointer = shift->pointer();
    } else return false;
  }
  return false;
}
bool source_residual(llvm::Value* pointer, llvm::Function* function,
                     unsigned bits, std::vector< Z >& path) {
  std::vector< Z > reverse;
  const auto& layout = function->getParent()->getDataLayout();
  for (unsigned depth = 0; depth < 32; ++depth) {
    if (!pointer->getType()->isPointerTy() ||
        pointer->getType()->getPointerAddressSpace() != 0) return false;
    if (pointer == function->getArg(0)) {
      Z total(0); path.push_back(total);
      for (auto it = reverse.rbegin(); it != reverse.rend(); ++it) {
        total += *it; path.push_back(total);
      }
      return true;
    }
    if (auto* cast = llvm::dyn_cast< llvm::BitCastInst >(pointer)) {
      if (cast->getFunction() != function) return false;
      pointer = cast->getOperand(0); continue;
    }
    auto* gep = llvm::dyn_cast< llvm::GetElementPtrInst >(pointer);
    if (!gep || gep->getFunction() != function || gep->getNumIndices() > 32) return false;
    Z displacement(0);
    for (auto it = llvm::gep_type_begin(gep), end = llvm::gep_type_end(gep);
         it != end; ++it) {
      auto* index = llvm::dyn_cast< llvm::ConstantInt >(it.getOperand());
      // Native GEP indices are signed, even when their C origin is unsigned.
      if (!index || index->getBitWidth() > bits) return false;
      if (auto* structure = it.getStructTypeOrNull()) {
        if (index->getValue().getActiveBits() > 32 ||
            index->getZExtValue() >= structure->getNumElements()) return false;
        displacement += Z(layout.getStructLayout(structure)
                              ->getElementOffset(index->getZExtValue()));
      } else {
        if (!it.getIndexedType()->isSized()) return false;
        auto size = layout.getTypeAllocSize(it.getIndexedType());
        if (size.isScalable()) return false;
        displacement += Z(index->getSExtValue()) * Z(size.getFixedValue());
      }
    }
    reverse.push_back(displacement); pointer = gep->getPointerOperand();
  }
  return false;
}
} // namespace

class ThreadArgumentWindows::Impl {
public:
  Context& ctx;
  ar::Function* worker = nullptr;
  ar::CallBase* create = nullptr;
  ar::Store* precreate_store = nullptr;
  bool numeric_mode = false, numeric_publication_seen = false;
  ar::UnaryOperation* numeric_argument = nullptr;
  ar::Store* numeric_publication = nullptr;
  ar::GlobalVariable* numeric_global = nullptr;
  llvm::GlobalVariable* numeric_source_global = nullptr;
  ar::Value* base = nullptr;
  ar::InternalVariable* counter = nullptr;
  ar::BinaryOperation* step = nullptr;
  Z stride{0};
  Variable* anchor = nullptr;
  MemoryLocation* object = nullptr;
  bool create_seen = false, step_seen = false, valid = true;
  unsigned pointer_bits;
  const bool source_mode;
  std::vector< ar::UnaryOperation* > index_adapters;
  llvm::Value* source_index = nullptr;
  llvm::PHINode* source_counter = nullptr;
  bool index_seen = false;
  Z index_lower{0}, index_upper{0};
  std::size_t creator_queries = 0, window_queries = 0;
  mutable std::size_t pair_queries = 0;

  explicit Impl(Context& context) : ctx(context),
      pointer_bits(context.bundle->data_layout().pointers.bit_width),
      source_mode(context.opts.machine_int_domain == MachineIntDomainOption::Interval) {
    // The explicit Polka mode retains its native projection. Default interval
    // mode authenticates the actual source affine family without ghost state.
    if ((!source_mode && ctx.opts.machine_int_domain != MachineIntDomainOption::VarPackApronPolkaPolyhedra) ||
        ctx.opts.entry_points.size() != 1 || !ctx.opts.no_init_globals.empty() ||
        ctx.bundle->global_or_null("ar.global_ctors") ||
        ctx.bundle->global_or_null("ar.global_dtors")) return;
    auto* main = ctx.opts.entry_points.front();
    auto* source_main = main->has_frontend()
        ? llvm::dyn_cast< llvm::Function >(main->frontend< llvm::Value >()) : nullptr;
    if (main->name() != "main" || main->num_parameters() != 0 || !source_main ||
        !source_main->use_empty() || !source_main->getParent()->getModuleInlineAsm().empty() ||
        (pointer_bits != 32 && pointer_bits != 64)) return;
    ar::CallBase* candidate = nullptr;
    for (auto fi = ctx.bundle->function_begin(); fi != ctx.bundle->function_end(); ++fi) {
      if (!(*fi)->is_definition()) continue;
      for (auto* block : *(*fi)->body()) for (auto* stmt : *block) {
        auto* call = dyn_cast< ar::CallBase >(stmt);
        auto* api = call ? native_handle_callee(call) : nullptr;
        if (api && api->name() == "pthread_create") {
          if (candidate) return;
          candidate = call;
        }
      }
    }
    if (!candidate || candidate->code() != main->body() ||
        !isa< ar::Call >(candidate) || !candidate->has_frontend() ||
        candidate->num_arguments() != 4 ||
        !isa< ar::NullConstant >(candidate->argument(1))) return;
    auto* api = native_handle_callee(candidate);
    auto* source = llvm::dyn_cast< llvm::CallInst >(candidate->frontend< llvm::Value >());
    auto* source_api = source ? source->getCalledFunction() : nullptr;
    auto* entry = dyn_cast< ar::FunctionPointerConstant >(candidate->argument(2));
    auto* result = api ? dyn_cast< ar::IntegerType >(api->type()->return_type()) : nullptr;
    if (!api || !api->is_declaration() || api->type()->is_var_arg() ||
        api->num_parameters() != 4 || !result || result->bit_width() != 32 ||
        !source_api || !source_api->isDeclaration() || source_api->isVarArg() ||
        source_api->getName() != "pthread_create" || source_api->arg_size() != 4 ||
        source->arg_size() != 4 || source->hasOperandBundles() || source->isInlineAsm() ||
        source->getCallingConv() != llvm::CallingConv::C ||
        source_api->getCallingConv() != llvm::CallingConv::C ||
        source->getFunctionType() != source_api->getFunctionType() ||
        source->getFunction() != source_main || !api->has_frontend() ||
        api->frontend< llvm::Value >() != source_api ||
        source->getModule()->getDataLayout().getPointerSizeInBits() != pointer_bits ||
        !source->getType()->isIntegerTy(32) || !entry) return;
    for (unsigned i = 0; i < 4; ++i) {
      if (api->type()->param_type(i) != candidate->argument(i)->type() ||
          !candidate->argument(i)->type()->is_pointer()) return;
      auto* type = llvm::dyn_cast< llvm::PointerType >(source->getArgOperand(i)->getType());
      if (!type || type->getAddressSpace() != 0) return;
      for (auto attr : {llvm::Attribute::ByVal, llvm::Attribute::StructRet,
                       llvm::Attribute::InAlloca, llvm::Attribute::Preallocated})
        if (source->paramHasAttr(i, attr) || source_api->hasParamAttribute(i, attr)) return;
    }
    auto* child = entry->function();
    auto* source_worker = child->has_frontend()
        ? llvm::dyn_cast< llvm::Function >(child->frontend< llvm::Value >()) : nullptr;
    if (!child->is_definition() || child == main || child->num_parameters() != 1 ||
        child->type()->is_var_arg() || !child->param(0)->type()->is_pointer() ||
        !child->type()->return_type()->is_pointer() || !source_worker ||
        source_worker->isVarArg() || source_worker->arg_size() != 1 ||
        source_worker->getCallingConv() != llvm::CallingConv::C ||
        source->getArgOperand(2)->stripPointerCasts() != source_worker) return;
    auto* byte_pointer = llvm::Type::getInt8PtrTy(source->getContext());
    auto* signature = llvm::cast< llvm::PointerType >(source->getArgOperand(2)->getType());
    if (source_worker->getReturnType() != byte_pointer ||
        source_worker->getFunctionType()->getParamType(0) != byte_pointer ||
        source->getArgOperand(3)->getType() != byte_pointer ||
        signature->getPointerElementType() != source_worker->getFunctionType()) return;
    // Close every activation, including ordinary calls and escaped callbacks.
    std::vector< llvm::Value* > uses{source_worker};
    std::set< llvm::Value* > seen;
    while (!uses.empty()) {
      auto* value = uses.back(); uses.pop_back();
      if (seen.size() >= 4096) return;
      if (!seen.insert(value).second) continue;
      for (auto* user : value->users()) {
        if (llvm::isa< llvm::BitCastOperator >(user)) uses.push_back(user);
        else if (user != source || source->getArgOperand(2) != value) return;
      }
    }
    auto* pointer = dyn_cast_or_null< ar::InternalVariable >(uncast(candidate->argument(3)));
    auto* shift = pointer ? dyn_cast_or_null< ar::PointerShift >(unique_def(pointer)) : nullptr;
    if (shift) {
      if (shift->num_terms() != 1 || shift->parent() != candidate->parent()) return;
    } else {
      numeric_argument = pointer
          ? dyn_cast_or_null< ar::UnaryOperation >(unique_def(pointer)) : nullptr;
      auto* integer = numeric_argument
          ? dyn_cast< ar::IntegerType >(numeric_argument->operand()->type()) : nullptr;
      if (!source_mode || std::getenv("IKOS_RACE_DISABLE_NUMERIC_ARGUMENT_PRODUCER") ||
          !numeric_argument || !integer || integer->bit_width() != pointer_bits ||
          numeric_argument->parent() != candidate->parent() ||
          (numeric_argument->op() != ar::UnaryOperation::SIToPtr &&
           numeric_argument->op() != ar::UnaryOperation::UIToPtr)) return;
      numeric_mode = true;
    }
    const Z family_stride = shift ? shift->term(0).first.to_z_number() : Z(1);
    ar::Value* index_value = shift ? shift->term(0).second : numeric_argument->operand();
    // Discover the induction variable through native width adapters only.
    for (unsigned depth = 0; depth < 32; ++depth) {
      auto* variable = dyn_cast< ar::InternalVariable >(index_value);
      auto* conversion = variable
          ? dyn_cast_or_null< ar::UnaryOperation >(unique_def(variable)) : nullptr;
      if (!conversion || (conversion->op() != ar::UnaryOperation::SExt &&
                          conversion->op() != ar::UnaryOperation::ZExt &&
                          conversion->op() != ar::UnaryOperation::Bitcast &&
                          (!source_mode || (conversion->op() != ar::UnaryOperation::UTrunc &&
                                            conversion->op() != ar::UnaryOperation::STrunc))) ||
          !conversion->operand()->type()->is_integer()) break;
      if (source_mode) index_adapters.push_back(conversion);
      index_value = conversion->operand();
    }
    auto* index = dyn_cast< ar::InternalVariable >(index_value);
    auto* integer = index ? dyn_cast< ar::IntegerType >(index->type()) : nullptr;
    if (!integer || integer->bit_width() > 64 || family_stride <= 0) return;
    auto* body = candidate->parent();
    if (body->num_predecessors() != 1 || body->num_successors() != 1) return;
    auto* header = *body->predecessor_begin();
    if (*body->successor_begin() != header || header == body ||
        header->num_predecessors() != 2 || header->num_successors() != 2 ||
        !header->is_predecessor(body) || !header->is_successor(body)) return;
    ar::BasicBlock* pre = nullptr;
    ar::BasicBlock* exit = nullptr;
    for (auto it = header->predecessor_begin(); it != header->predecessor_end(); ++it)
      if (*it != body) pre = *it;
    for (auto it = header->successor_begin(); it != header->successor_end(); ++it)
      if (*it != body) exit = *it;
    if (!pre || !exit || pre == header || pre == exit ||
        pre->num_successors() != 1 || *pre->successor_begin() != header ||
        reaches(exit, pre) || reaches(exit, header) || reaches(exit, body)) return;
    ar::Assignment* init = nullptr;
    ar::Assignment* update = nullptr;
    for (auto* block : *main->body()) for (auto* stmt : *block) {
      if (stmt->result_or_null() != index) continue;
      auto* assignment = dyn_cast< ar::Assignment >(stmt);
      if (!assignment) return;
      if (block == pre && !init) init = assignment;
      else if (block == body && !update) update = assignment;
      else return;
    }
    auto* zero = init ? dyn_cast< ar::IntegerConstant >(init->operand()) : nullptr;
    auto* next = update ? dyn_cast< ar::InternalVariable >(update->operand()) : nullptr;
    auto* advance = next ? dyn_cast_or_null< ar::BinaryOperation >(unique_def(next)) : nullptr;
    auto* one = advance ? dyn_cast< ar::IntegerConstant >(advance->right()) : nullptr;
    if (!zero || !zero->value().is_zero() || pre->back() != init ||
        !advance || advance->parent() != body || advance->left() != index ||
        !one || one->value().to_z_number() != 1 ||
        (advance->op() != ar::BinaryOperation::SAdd &&
         advance->op() != ar::BinaryOperation::UAdd) ||
        body->back() != update || advance->result()->type() != index->type()) return;
    bool after_create = false, after_step = false;
    for (auto* stmt : *body) {
      if (stmt == candidate) { after_create = true; continue; }
      if (stmt == advance) { if (!after_create) return; after_step = true; continue; }
      if (stmt == update) { if (!after_step) return; continue; }
      if (stmt == numeric_argument) continue;
      if (auto* cast = dyn_cast< ar::UnaryOperation >(stmt)) {
        if (cast->op() != ar::UnaryOperation::Bitcast &&
            cast->op() != ar::UnaryOperation::SExt &&
            cast->op() != ar::UnaryOperation::ZExt &&
            (!source_mode || (cast->op() != ar::UnaryOperation::UTrunc &&
                             cast->op() != ar::UnaryOperation::STrunc))) return;
      } else if (auto* store = dyn_cast< ar::Store >(stmt)) {
        if (!source_mode || numeric_mode || after_create || precreate_store) return;
        precreate_store = store;
      } else if (!isa< ar::Comparison >(stmt) && !isa< ar::PointerShift >(stmt)) return;
    }
    for (auto* stmt : *header)
      if (!isa< ar::UnaryOperation >(stmt)) return;
    // A base defined inside the family cannot be a stable coordinate.
    if (!numeric_mode) {
      auto* base_var = dyn_cast_or_null< ar::InternalVariable >(uncast(shift->pointer()));
      auto* base_def = base_var ? unique_def(base_var) : nullptr;
      if (!base_def || base_def->parent() == body || base_def->parent() == header ||
          !isa< ar::CallBase >(base_def)) return;
      base = shift->pointer(); stride = family_stride;
    }
    if (source_mode && !prepare_source(main, source_main, child, source_worker,
                                       candidate, source, shift, index, advance, pre)) return;
    create = candidate; worker = child; counter = index; step = advance;
    if (source_mode) return; // No anchor allocation or numeric-state mutation.
    auto* type = ar::IntegerType::get(ctx.bundle->context(), pointer_bits, core::Unsigned);
    anchor = ctx.var_factory->get_named_shadow(type, "thread.argument.anchor." + worker->name());
  }


  // Authenticate the narrower source certificate independently of Polka.
  bool prepare_source(ar::Function* main, llvm::Function* source_main,
                      ar::Function* child, llvm::Function* source_worker,
                      ar::CallBase* candidate, llvm::CallInst* source,
                      ar::PointerShift* shift, ar::InternalVariable* index,
                      ar::BinaryOperation* advance, ar::BasicBlock* ar_pre) {
    const auto& layout = source->getModule()->getDataLayout();
    const auto abi = pthread_object_layout(pointer_bits, source->getModule());
    auto* api = source->getCalledFunction();
    auto* output = llvm::dyn_cast< llvm::PointerType >(source->getArgOperand(0)->getType());
    auto* ar_output = dyn_cast< ar::PointerType >(candidate->argument(0)->type());
    auto* handle = ar_output ? dyn_cast< ar::IntegerType >(ar_output->pointee()) : nullptr;
    if (!abi || abi.word * 8 != pointer_bits ||
        layout.getIndexSizeInBits(0) != pointer_bits || !output ||
        !output->getPointerElementType()->isIntegerTy(pointer_bits) ||
        !handle || handle->bit_width() != pointer_bits ||
        !ordinary_source_call_abi_attributes(source, api) ||
        !ordinary_source_function_abi_attributes(source_main) ||
        !ordinary_source_function_abi_attributes(source_worker) ||
        source_main->arg_size() || source_main->isVarArg() ||
        source_main->getCallingConv() != llvm::CallingConv::C ||
        !llvm::isa< llvm::ConstantPointerNull >(source->getArgOperand(1))) return false;
    // Independent AR use closure (LLVM closure is checked by the constructor).
    auto closed = [&](ar::Code* code) {
      unsigned budget = 8192;
      for (auto* block : *code) for (auto* statement : *block) {
        if (!budget--) return false;
        if (code == child->body() && statement->result_or_null() == child->param(0))
          return false;
        for (auto it = statement->op_begin(); it != statement->op_end(); ++it) {
          auto* function = dyn_cast< ar::FunctionPointerConstant >(*it);
          if (!function) continue;
          if (function->function() == main) return false;
          if (function->function() == child &&
              (statement != candidate || *it != candidate->argument(2))) return false;
        }
      }
      return true;
    };
    for (auto fi = ctx.bundle->function_begin(); fi != ctx.bundle->function_end(); ++fi)
      if ((*fi)->is_definition() && !closed((*fi)->body())) return false;
    for (auto gi = ctx.bundle->global_begin(); gi != ctx.bundle->global_end(); ++gi)
      if ((*gi)->is_definition() && !closed((*gi)->initializer())) return false;
    unsigned source_creates = 0;
    for (auto& f : *source->getModule()) for (auto& block : f) for (auto& i : block)
      if (auto* call = llvm::dyn_cast< llvm::CallBase >(&i))
        if (call->getCalledOperand()->stripPointerCasts() == api) {
          if (call != source || ++source_creates != 1) return false;
        }
    auto* gep = llvm::dyn_cast< llvm::GetElementPtrInst >(
        source->getArgOperand(3)->stripPointerCasts());
    auto* numeric_source = numeric_mode ? llvm::dyn_cast< llvm::IntToPtrInst >(
        source->getArgOperand(3)->stripPointerCasts()) : nullptr;
    auto* phi = index->has_frontend()
        ? llvm::dyn_cast< llvm::PHINode >(index->frontend< llvm::Value >()) : nullptr;
    auto* source_step = advance->has_frontend()
        ? llvm::dyn_cast< llvm::BinaryOperator >(advance->frontend< llvm::Value >()) : nullptr;
    if (!pointer_binding(candidate->argument(3), source->getArgOperand(3)) ||
        !phi || phi->getFunction() != source_main || phi->getNumIncomingValues() != 2 ||
        !source_step || source_step->getOpcode() != llvm::Instruction::Add ||
        source_step->getOperand(0) != phi) return false;
    if (numeric_mode) {
      if (!numeric_source || numeric_source->getFunction() != source_main ||
          !numeric_source->getOperand(0)->getType()->isIntegerTy(pointer_bits) ||
          numeric_source->getType()->getPointerAddressSpace() != 0 ||
          !numeric_argument->has_frontend() ||
          numeric_argument->frontend< llvm::Value >() != numeric_source ||
          !source_integer_binding(numeric_argument->operand(), numeric_source->getOperand(0)))
        return false;
    } else if (!gep || gep->getFunction() != source_main || gep->getAddressSpace() != 0 ||
               gep->getNumIndices() != 1 || !gep->getSourceElementType()->isSized() ||
               !pointer_binding(shift->pointer(), gep->getPointerOperand()) ||
               !source_integer_binding(shift->term(0).second, gep->getOperand(1))) return false;
    // LLVM retains a separate increment latch that AR may merge into body.
    // Certify that exact one-way chain; dominance alone would permit a bypass
    // or another visit to create before the selected +1 step.
    auto* source_body = source->getParent();
    auto* latch = source_step->getParent();
    auto* header = phi->getParent();
    auto* header_branch = llvm::dyn_cast< llvm::BranchInst >(header->getTerminator());
    auto* body_branch = llvm::dyn_cast< llvm::BranchInst >(source_body->getTerminator());
    auto* latch_branch = llvm::dyn_cast< llvm::BranchInst >(latch->getTerminator());
    if (header == source_body || header == latch || !header_branch ||
        !header_branch->isConditional() ||
        (header_branch->getSuccessor(0) != source_body &&
         header_branch->getSuccessor(1) != source_body) ||
        source_body->getSinglePredecessor() != header || !body_branch ||
        !body_branch->isUnconditional() || !latch_branch ||
        !latch_branch->isUnconditional() || latch_branch->getSuccessor(0) != header)
      return false;
    if (source_body == latch) {
      if (!source->comesBefore(source_step)) return false;
    } else {
      if (body_branch->getSuccessor(0) != latch ||
          latch->getSinglePredecessor() != source_body) return false;
      for (auto& instruction : *latch)
        if (&instruction != source_step && &instruction != latch_branch) return false;
    }
    auto* one = llvm::dyn_cast< llvm::ConstantInt >(source_step->getOperand(1));
    bool initial = false, backedge = false;
    for (unsigned i = 0; i < 2; ++i) {
      auto* constant = llvm::dyn_cast< llvm::ConstantInt >(phi->getIncomingValue(i));
      if (constant && constant->isZero()) {
        auto* pre = phi->getIncomingBlock(i);
        auto* branch = llvm::dyn_cast< llvm::BranchInst >(pre->getTerminator());
        if (pre == header || pre == source_body || pre == latch || !branch ||
            !branch->isUnconditional() || branch->getSuccessor(0) != header) return false;
        initial = true;
      } else if (phi->getIncomingValue(i) == source_step &&
               phi->getIncomingBlock(i) == source_step->getParent()) backedge = true;
      else return false;
    }
    if (!one || !one->isOne() || !initial || !backedge) return false;
    if (numeric_mode) {
      source_index = numeric_source->getOperand(0); source_counter = phi;
      return prepare_numeric_base(main, source_main, child, source_worker, source, ar_pre);
    }
    auto size = layout.getTypeAllocSize(gep->getSourceElementType());
    if (size.isScalable() || Z(size.getFixedValue()) != shift->term(0).first.to_z_number()) return false;
    llvm::DominatorTree dominance(*source_main);
    auto* source_base = llvm::dyn_cast< llvm::Instruction >(
        gep->getPointerOperand()->stripPointerCasts());
    if (!source_base || !dominance.dominates(source_base, gep) ||
        !dominance.dominates(gep, source)) return false;
    if (precreate_store) {
      auto same_index = [](llvm::Value* a, llvm::Value* b) {
        for (unsigned depth = 0; depth < 32; ++depth) {
          if (a == b) return true;
          auto* x = llvm::dyn_cast< llvm::CastInst >(a);
          auto* y = llvm::dyn_cast< llvm::CastInst >(b);
          if (!x || !y || x->getFunction() != y->getFunction() ||
              !x->getType()->isIntegerTy() || !x->getOperand(0)->getType()->isIntegerTy() ||
              x->getType() != y->getType() ||
              x->getOperand(0)->getType() != y->getOperand(0)->getType() ||
              x->getOpcode() != y->getOpcode() ||
              (x->getOpcode() != llvm::Instruction::SExt &&
               x->getOpcode() != llvm::Instruction::ZExt &&
               x->getOpcode() != llvm::Instruction::Trunc &&
               x->getOpcode() != llvm::Instruction::BitCast)) return false;
          a = x->getOperand(0); b = y->getOperand(0);
        }
        return false;
      };
      auto* store = precreate_store->has_frontend()
          ? llvm::dyn_cast< llvm::StoreInst >(precreate_store->frontend< llvm::Value >()) : nullptr;
      auto* address = store ? llvm::dyn_cast< llvm::GetElementPtrInst >(
          store->getPointerOperand()->stripPointerCasts()) : nullptr;
      if (!store || store->getFunction() != source_main || store->isAtomic() ||
          store->getParent() != source_body || !store->comesBefore(source) ||
          !address || address->getFunction() != source_main ||
          address->getAddressSpace() != 0 || address->getNumIndices() != 1 ||
          !dominance.dominates(source_base, address) ||
          !dominance.dominates(address, store) ||
          address->getPointerOperand()->stripPointerCasts() !=
              gep->getPointerOperand()->stripPointerCasts() ||
          !same_index(address->getOperand(1), gep->getOperand(1)) ||
          address->getSourceElementType() != gep->getSourceElementType() ||
          !pointer_binding(precreate_store->pointer(), store->getPointerOperand()) ||
          !same_creation_pointer(precreate_store->pointer(), candidate->argument(3))) return false;
    }
    source_index = gep->getOperand(1); source_counter = phi;
    return true;
  }

  // Exact imported address identity; similar intervals cannot establish it.
  bool same_creation_pointer(ar::Value* a, ar::Value* b) const {
    a = uncast(a); b = uncast(b);
    if (a == b) return a != nullptr;
    auto* av = dyn_cast_or_null< ar::InternalVariable >(a);
    auto* bv = dyn_cast_or_null< ar::InternalVariable >(b);
    auto* x = av ? dyn_cast_or_null< ar::PointerShift >(unique_def(av)) : nullptr;
    auto* y = bv ? dyn_cast_or_null< ar::PointerShift >(unique_def(bv)) : nullptr;
    if (!x || !y || x->num_terms() != 1 || y->num_terms() != 1 ||
        uncast(x->pointer()) != uncast(y->pointer()) ||
        x->term(0).first != y->term(0).first) return false;
    a = x->term(0).second; b = y->term(0).second;
    // Separate GEPs may duplicate the same cast of the induction variable.
    for (unsigned depth = 0; depth < 32; ++depth) {
      if (a == b) return true;
      auto* av = dyn_cast< ar::InternalVariable >(a);
      auto* bv = dyn_cast< ar::InternalVariable >(b);
      auto* ac = av ? dyn_cast_or_null< ar::UnaryOperation >(unique_def(av)) : nullptr;
      auto* bc = bv ? dyn_cast_or_null< ar::UnaryOperation >(unique_def(bv)) : nullptr;
      if (!ac || !bc || !a->type()->is_integer() || !ac->operand()->type()->is_integer() ||
          a->type() != b->type() || ac->operand()->type() != bc->operand()->type() ||
          ac->op() != bc->op() ||
          (ac->op() != ar::UnaryOperation::SExt && ac->op() != ar::UnaryOperation::ZExt &&
           ac->op() != ar::UnaryOperation::UTrunc && ac->op() != ar::UnaryOperation::STrunc &&
           ac->op() != ar::UnaryOperation::Bitcast))
        return false;
      a = ac->operand(); b = bc->operand();
    }
    return false;
  }


  // Native integer tokens are coordinates, never pointer capabilities.
  bool numeric_ar_index(ar::Value* value, ar::Function* child, bool range) const {
    for (unsigned depth = 0; depth < 32; ++depth) {
      auto* type = dyn_cast< ar::IntegerType >(value->type());
      auto* variable = dyn_cast< ar::InternalVariable >(value);
      auto* operation = variable
          ? dyn_cast_or_null< ar::UnaryOperation >(unique_def(variable)) : nullptr;
      if (!type || !variable || variable->code() != child->body() || !operation ||
          type->bit_width() > 64 ||
          (range && !fits_nonnegative(index_upper, type->bit_width(), type->sign())))
        return false;
      if (operation->op() == ar::UnaryOperation::PtrToSI ||
          operation->op() == ar::UnaryOperation::PtrToUI)
        return type->bit_width() <= pointer_bits &&
            uncast(operation->operand()) == child->param(0);
      auto* input = dyn_cast< ar::IntegerType >(operation->operand()->type());
      if (!input || input->bit_width() > 64 ||
          (range && !fits_nonnegative(index_upper, input->bit_width(), input->sign())))
        return false;
      switch (operation->op()) {
        case ar::UnaryOperation::SExt:
          if (range && !fits_nonnegative(index_upper, input->bit_width(), core::Signed))
            return false;
          break;
        case ar::UnaryOperation::ZExt:
        case ar::UnaryOperation::UTrunc:
        case ar::UnaryOperation::STrunc: break;
        case ar::UnaryOperation::Bitcast:
          if (input->bit_width() != type->bit_width()) return false;
          break;
        default: return false;
      }
      value = operation->operand();
    }
    return false;
  }

  bool numeric_source_index(llvm::Value* value, llvm::Function* child, bool range) const {
    if (!value->getType()->isIntegerTy() ||
        (range && !fits_nonnegative(index_upper,
                                   value->getType()->getIntegerBitWidth(), core::Signed)))
      return false; // GEP indices use signed native interpretation.
    for (unsigned depth = 0; depth < 32; ++depth) {
      auto* operation = llvm::dyn_cast< llvm::CastInst >(value);
      if (!operation || operation->getFunction() != child ||
          !value->getType()->isIntegerTy() ||
          value->getType()->getIntegerBitWidth() > 64 ||
          (range && !fits_nonnegative(index_upper,
                                     value->getType()->getIntegerBitWidth(), core::Unsigned)))
        return false;
      if (operation->getOpcode() == llvm::Instruction::PtrToInt) {
        if (value->getType()->getIntegerBitWidth() > pointer_bits) return false;
        auto* token = operation->getOperand(0);
        for (unsigned copies = 0; copies < 32; ++copies) {
          if (!token->getType()->isPointerTy() ||
              token->getType()->getPointerAddressSpace() != 0) return false;
          if (token == child->getArg(0)) return true;
          auto* copy = llvm::dyn_cast< llvm::BitCastInst >(token);
          if (!copy || copy->getFunction() != child) return false;
          token = copy->getOperand(0);
        }
        return false;
      }
      auto* input = operation->getOperand(0)->getType();
      if (!input->isIntegerTy() || input->getIntegerBitWidth() > 64 ||
          (range && !fits_nonnegative(index_upper, input->getIntegerBitWidth(), core::Unsigned)))
        return false;
      switch (operation->getOpcode()) {
        case llvm::Instruction::SExt:
          if (range && !fits_nonnegative(index_upper, input->getIntegerBitWidth(), core::Signed))
            return false;
          break;
        case llvm::Instruction::ZExt:
        case llvm::Instruction::Trunc: break;
        case llvm::Instruction::BitCast:
          if (input != value->getType()) return false;
          break;
        default: return false;
      }
      value = operation->getOperand(0);
    }
    return false;
  }

  ar::PointerShift* numeric_shift(ar::Value* pointer, ar::Function* child) const {
    auto* variable = dyn_cast_or_null< ar::InternalVariable >(uncast(pointer));
    auto* shift = variable
        ? dyn_cast_or_null< ar::PointerShift >(unique_def(variable)) : nullptr;
    if (!shift || shift->code() != child->body() || shift->num_terms() != 1 ||
        shift->term(0).first.to_z_number() <= 0 ||
        !numeric_ar_index(shift->term(0).second, child, false)) return nullptr;
    return shift;
  }

  bool numeric_address(ar::Value* pointer, llvm::Value* source_pointer,
                       ar::Function* child, bool range,
                       ar::GlobalVariable*& global, llvm::GlobalVariable*& source_global,
                       Z& spacing) const {
    auto* shift = numeric_shift(pointer, child);
    auto* source_child = child->has_frontend()
        ? llvm::dyn_cast< llvm::Function >(child->frontend< llvm::Value >()) : nullptr;
    auto* gep = source_pointer
        ? llvm::dyn_cast< llvm::GetElementPtrInst >(source_pointer->stripPointerCasts()) : nullptr;
    auto* base_variable = shift
        ? dyn_cast_or_null< ar::InternalVariable >(uncast(shift->pointer())) : nullptr;
    auto* load = base_variable
        ? dyn_cast_or_null< ar::Load >(unique_def(base_variable)) : nullptr;
    auto* source_load = gep
        ? llvm::dyn_cast< llvm::LoadInst >(gep->getPointerOperand()->stripPointerCasts()) : nullptr;
    global = load ? dyn_cast< ar::GlobalVariable >(load->operand()) : nullptr;
    source_global = source_load
        ? llvm::dyn_cast< llvm::GlobalVariable >(source_load->getPointerOperand()) : nullptr;
    if (!source_child || !gep || gep->getFunction() != source_child ||
        gep->getAddressSpace() != 0 || gep->getNumIndices() != 1 ||
        !gep->getSourceElementType()->isSized() || !load || load->code() != child->body() ||
        load->is_atomic() || load->is_volatile() || !source_load || !source_load->isSimple() ||
        source_load->getFunction() != source_child || !global || !source_global ||
        !global->has_frontend() || global->frontend< llvm::Value >() != source_global ||
        !load->has_frontend() || load->frontend< llvm::Value >() != source_load ||
        !load->result()->type()->is_pointer() || !source_load->getType()->isPointerTy() ||
        source_load->getType()->getPointerAddressSpace() != 0 ||
        !pointer_binding(pointer, source_pointer) ||
        !pointer_binding(shift->pointer(), gep->getPointerOperand()) ||
        !source_integer_binding(shift->term(0).second, gep->getOperand(1)) ||
        !numeric_ar_index(shift->term(0).second, child, range) ||
        !numeric_source_index(gep->getOperand(1), source_child, range)) return false;
    const auto size = gep->getModule()->getDataLayout().getTypeAllocSize(gep->getSourceElementType());
    if (size.isScalable() || Z(size.getFixedValue()) != shift->term(0).first.to_z_number())
      return false;
    spacing = shift->term(0).first.to_z_number();
    return true;
  }

  bool prepare_numeric_base(ar::Function* main, llvm::Function* source_main,
                            ar::Function* child, llvm::Function* source_child,
                            llvm::CallInst* source_create, ar::BasicBlock* pre) {
    // Discover a candidate data address from actual worker memory operations.
    for (auto* block : *child->body()) for (auto* statement : *block) {
      auto* load = dyn_cast< ar::Load >(statement);
      auto* store = dyn_cast< ar::Store >(statement);
      auto* source = statement->has_frontend()
          ? llvm::dyn_cast< llvm::Instruction >(statement->frontend< llvm::Value >()) : nullptr;
      auto* source_load = llvm::dyn_cast_or_null< llvm::LoadInst >(source);
      auto* source_store = llvm::dyn_cast_or_null< llvm::StoreInst >(source);
      if ((!load && !store) || bool(load) != bool(source_load) || bool(store) != bool(source_store))
        continue;
      ar::GlobalVariable* global = nullptr;
      llvm::GlobalVariable* source_global = nullptr;
      Z spacing(0);
      if (!numeric_address(load ? load->operand() : store->pointer(),
                           source_load ? source_load->getPointerOperand() : source_store->getPointerOperand(),
                           child, false, global, source_global, spacing)) continue;
      if (numeric_global && (numeric_global != global || numeric_source_global != source_global ||
                             stride != spacing)) return false;
      numeric_global = global; numeric_source_global = source_global; stride = spacing;
    }
    if (!numeric_global || !numeric_source_global ||
        !numeric_global->is_definition() || numeric_global->is_thread_local() ||
        numeric_source_global->isConstant() || numeric_source_global->isThreadLocal() ||
        numeric_source_global->isExternallyInitialized() || numeric_source_global->getAddressSpace() != 0 ||
        !numeric_source_global->hasInitializer() ||
        !llvm::isa< llvm::ConstantPointerNull >(numeric_source_global->getInitializer()) ||
        !numeric_source_global->getValueType()->isPointerTy() ||
        numeric_source_global->getValueType()->getPointerAddressSpace() != 0) return false;
    // The pointer slot has one native full-width store, before the entire family.
    // Loaded heap addresses may escape; the slot address itself must not.
    llvm::StoreInst* publication = nullptr;
    unsigned budget = 32768;
    for (auto* user : numeric_source_global->users()) {
      if (!budget--) return false;
      if (llvm::isa< llvm::DbgInfoIntrinsic >(user)) continue;
      if (auto* load = llvm::dyn_cast< llvm::LoadInst >(user)) {
        if (!load->isSimple() || load->getPointerOperand() != numeric_source_global ||
            load->getType() != numeric_source_global->getValueType()) return false;
      } else if (auto* store = llvm::dyn_cast< llvm::StoreInst >(user)) {
        if (publication || !store->isSimple() || store->getFunction() != source_main ||
            store->getPointerOperand() != numeric_source_global ||
            store->getValueOperand()->getType() != numeric_source_global->getValueType())
          return false;
        publication = store;
      } else return false;
    }
    if (!publication) return false;
    llvm::BasicBlock* source_pre = nullptr;
    for (unsigned n = 0; n < source_counter->getNumIncomingValues(); ++n)
      if (auto* zero = llvm::dyn_cast< llvm::ConstantInt >(source_counter->getIncomingValue(n)))
        if (zero->isZero()) source_pre = source_counter->getIncomingBlock(n);
    llvm::DominatorTree dominance(*source_main);
    if (!source_pre || publication->getParent() != source_pre ||
        !dominance.dominates(publication, source_counter) ||
        !dominance.dominates(publication, source_create)) return false;
    for (auto* block : *main->body()) for (auto* statement : *block) {
      if (!budget--) return false;
      if (!statement->has_frontend() ||
          statement->frontend< llvm::Value >() != publication) continue;
      auto* store = dyn_cast< ar::Store >(statement);
      if (!store || numeric_publication || store->parent() != pre ||
          store->pointer() != numeric_global || !store->value()->type()->is_pointer() ||
          store->is_atomic() || store->is_volatile() ||
          store->value()->type() != numeric_global->type()->pointee() ||
          !pointer_binding(store->value(), publication->getValueOperand())) return false;
      numeric_publication = store;
    }
    if (!numeric_publication) return false;
    base = numeric_publication->value();
    auto* base_variable = dyn_cast_or_null< ar::InternalVariable >(uncast(base));
    auto* base_definition = base_variable ? unique_def(base_variable) : nullptr;
    if (!base_definition || !isa< ar::CallBase >(base_definition) ||
        base_definition->code() != main->body()) return false;
    // Close imported slot uses separately, including optimizer-introduced uses.
    for (auto fi = ctx.bundle->function_begin(); fi != ctx.bundle->function_end(); ++fi) {
      if (!(*fi)->is_definition()) continue;
      for (auto* block : *(*fi)->body()) for (auto* statement : *block) {
        if (!budget--) return false;
        bool used = false;
        for (auto op = statement->op_begin(); op != statement->op_end(); ++op)
          used |= *op == numeric_global;
        if (!used) continue;
        if (statement == numeric_publication) continue;
        auto* load = dyn_cast< ar::Load >(statement);
        if (!load || load->operand() != numeric_global || !load->result()->type()->is_pointer() ||
            load->is_atomic() || load->is_volatile() ||
            load->result()->type() != numeric_global->type()->pointee() ||
            !load->has_frontend()) return false;
        auto* source = llvm::dyn_cast< llvm::LoadInst >(load->frontend< llvm::Value >());
        if (!source || source->getPointerOperand() != numeric_source_global) return false;
      }
    }
    // Other imported global initializers must not manufacture a slot alias.
    for (auto gi = ctx.bundle->global_begin(); gi != ctx.bundle->global_end(); ++gi) {
      if (!(*gi)->is_definition() || *gi == numeric_global) continue;
      for (auto* block : *(*gi)->initializer()) for (auto* statement : *block) {
        if (!budget--) return false;
        for (auto op = statement->op_begin(); op != statement->op_end(); ++op)
          if (*op == numeric_global) return false;
      }
    }
    unsigned initialized = 0;
    for (auto* block : *numeric_global->initializer()) for (auto* statement : *block) {
      if (!budget--) return false;
      auto* store = dyn_cast< ar::Store >(statement);
      if (!store || store->pointer() != numeric_global ||
          !isa< ar::NullConstant >(store->value()) ||
          store->value()->type() != numeric_global->type()->pointee()) return false;
      ++initialized;
    }
    return initialized == 1;
  }

  bool adapters_preserve(const Z& upper) const {
    for (auto* conversion : index_adapters) {
      auto* input = dyn_cast< ar::IntegerType >(conversion->operand()->type());
      auto* output = dyn_cast< ar::IntegerType >(conversion->result()->type());
      if (!input || !output || !fits_nonnegative(upper, input->bit_width(), input->sign()) ||
          !fits_nonnegative(upper, output->bit_width(), output->sign())) return false;
      if (conversion->op() == ar::UnaryOperation::SExt &&
          !fits_nonnegative(upper, input->bit_width(), core::Signed)) return false;
      if (conversion->op() == ar::UnaryOperation::Bitcast &&
          input->bit_width() != output->bit_width()) return false;
    }
    if (!source_index->getType()->isIntegerTy() ||
        !fits_nonnegative(upper, source_index->getType()->getIntegerBitWidth(), core::Signed))
      return false; // GEP sign-extends/truncates its index, not the byte product.
    auto* value = source_index;
    for (unsigned depth = 0; depth < 32; ++depth) {
      if (!value->getType()->isIntegerTy() ||
          !fits_nonnegative(upper, value->getType()->getIntegerBitWidth(), core::Unsigned))
        return false;
      if (value == source_counter) return true;
      auto* conversion = llvm::dyn_cast< llvm::CastInst >(value);
      if (!conversion || !conversion->getOperand(0)->getType()->isIntegerTy()) return false;
      const unsigned input = conversion->getOperand(0)->getType()->getIntegerBitWidth();
      switch (conversion->getOpcode()) {
        case llvm::Instruction::SExt:
          if (!fits_nonnegative(upper, input, core::Signed)) return false;
          break;
        case llvm::Instruction::ZExt: case llvm::Instruction::Trunc: break;
        case llvm::Instruction::BitCast:
          if (input != value->getType()->getIntegerBitWidth()) return false;
          break;
        default: return false;
      }
      value = conversion->getOperand(0);
    }
    return false;
  }

  bool span(const std::vector< Z >& path, std::uint64_t width = 0) const {
    if (!index_seen || path.empty()) return false;
    const Z modulus = Z(1) << pointer_bits;
    for (const auto& residual : path)
      if (stride * index_lower + residual < 0 ||
          stride * index_upper + residual >= modulus) return false;
    return !width || stride * index_upper + path.back() + Z(width) <= modulus;
  }

  bool source_endpoint(const AccessRecord& access) const {
    if (!access.stmt || !access.stmt->has_frontend() || !access.argument_window ||
        !access.width || access.argument_window->lower != access.argument_window->upper)
      return false;
    auto* source = llvm::dyn_cast< llvm::Instruction >(access.stmt->frontend< llvm::Value >());
    auto* load = llvm::dyn_cast_or_null< llvm::LoadInst >(source);
    auto* store = llvm::dyn_cast_or_null< llvm::StoreInst >(source);
    auto* ar_load = dyn_cast< ar::Load >(access.stmt);
    auto* ar_store = dyn_cast< ar::Store >(access.stmt);
    if ((!load && !store) || bool(load) != bool(ar_load) || bool(store) != bool(ar_store) ||
        (access.kind == AccessKind::Write) != bool(store) ||
        (source->getFunction() != worker->frontend< llvm::Value >() &&
         access.stmt != precreate_store)) return false;
    auto* type = load ? load->getType() : store->getValueOperand()->getType();
    auto* ar_type = ar_load ? ar_load->result()->type() : ar_store->value()->type();
    const auto bytes = source->getModule()->getDataLayout().getTypeStoreSize(type);
    if (bytes.isScalable() || bytes.getFixedValue() != access.width ||
        ctx.bundle->data_layout().store_size_in_bytes(ar_type) != Z(access.width)) return false;
    auto* pointer = load ? load->getPointerOperand() : store->getPointerOperand();
    auto* ar_pointer = ar_load ? ar_load->operand() : ar_store->pointer();
    if (numeric_mode) {
      ar::GlobalVariable* global = nullptr;
      llvm::GlobalVariable* source_global = nullptr;
      Z spacing(0);
      return access.thread_id == worker->name() && numeric_publication_seen &&
          access.argument_window->lower == Z(0) &&
          numeric_address(ar_pointer, pointer, worker, true, global, source_global, spacing) &&
          global == numeric_global && source_global == numeric_source_global &&
          spacing == stride && span({Z(0)}, access.width);
    }
    if (access.stmt == precreate_store) {
      // Same-iteration initialization precedes the actual create; other
      // iterations use the separately checked nonoverlapping byte cells.
      return access.thread_id == "main" && store &&
          same_creation_pointer(ar_pointer, create->argument(3)) &&
          access.argument_window->lower == Z(0) && span({Z(0)}, access.width);
    }
    std::vector< Z > ar_path, source_path;
    if (!pointer_binding(ar_pointer, pointer) || !ar_residual(ar_pointer, worker, ar_path) ||
        !source_residual(pointer, llvm::cast< llvm::Function >(worker->frontend< llvm::Value >()),
                         pointer_bits, source_path) ||
        ar_path.back() != source_path.back() ||
        ar_path.back() != access.argument_window->lower) return false;
    return span(ar_path, access.width) && span(source_path, access.width);
  }

  Variable* scratch(unsigned bits, const char* name) {
    return ctx.var_factory->get_named_shadow(
        ar::IntegerType::get(ctx.bundle->context(), bits, core::Signed), name);
  }

  void observe(ar::Statement* stmt, const value::AbstractDomain& inv, CallContext* cc) {
    if (!worker || (stmt != create && stmt != step && stmt != numeric_publication)) return;
    if (stmt == numeric_publication) {
      if (cc && cc->has_parent()) { valid = false; return; }
      const auto& literal = ctx.lit_factory->get_scalar(numeric_publication->value());
      const auto& state = inv.first().normal();
      if (!literal.is_pointer_var() || state.uninit_is_uninitialized(literal.var())) {
        valid = false; return;
      }
      auto points = state.pointer_to_points_to(literal.var());
      if (!points.is_set() || points.size() != 1 ||
          !isa< DynAllocMemoryLocation >(*points.begin()) ||
          !singleton(state.pointer_offset_to_interval(literal.var()), Z(0)) ||
          (object && object != *points.begin())) { valid = false; return; }
      object = *points.begin(); numeric_publication_seen = true;
      return;
    }
    // The certified creation belongs to the sole main activation, not a helper.
    if (cc && cc->has_parent()) { valid = false; return; }
    const auto& index = ctx.lit_factory->get_scalar(counter);
    auto range = inv.first().normal().int_to_interval(index.var());
    const auto* type = cast< ar::IntegerType >(counter->type());
    if (range.is_bottom() || range.lb().to_z_number() < 0 ||
        range.ub().to_z_number() >= core::MachineInt::max(type->bit_width(), type->sign()).to_z_number())
      valid = false;
    if (source_mode && !range.is_bottom() &&
        !adapters_preserve(range.ub().to_z_number())) valid = false;
    if (stmt == step) { step_seen = true; return; }
    create_seen = true;
    ++creator_queries;
    auto max = (Z(1) << pointer_bits) - 1;
    if (range.is_bottom() || stride * range.ub().to_z_number() > max) { valid = false; return; }
    if (numeric_mode) {
      const Z lower = range.lb().to_z_number(), upper = range.ub().to_z_number();
      if (!index_seen) { index_lower = lower; index_upper = upper; }
      else { index_lower = std::min(index_lower, lower); index_upper = std::max(index_upper, upper); }
      index_seen = true;
      return; // Numeric token is not the heap base.
    }
    const auto& pointer = ctx.lit_factory->get_scalar(create->argument(3));
    const auto& base_ptr = ctx.lit_factory->get_scalar(base);
    const auto& state = inv.first().normal();
    if (!pointer.is_pointer_var() || !base_ptr.is_pointer_var()) { valid = false; return; }
    auto pts = state.pointer_to_points_to(pointer.var());
    auto base_pts = state.pointer_to_points_to(base_ptr.var());
    if (!pts.is_set() || pts.size() != 1 || !base_pts.equals(pts) ||
        !singleton(state.pointer_offset_to_interval(base_ptr.var()), Z(0)) ||
        !isa< DynAllocMemoryLocation >(*pts.begin()) ||
        (object && object != *pts.begin())) { valid = false; return; }
    object = *pts.begin();
    if (source_mode) {
      if (range.is_bottom()) { valid = false; return; }
      const Z lower = range.lb().to_z_number(), upper = range.ub().to_z_number();
      if (!index_seen) { index_lower = lower; index_upper = upper; }
      else { index_lower = std::min(index_lower, lower); index_upper = std::max(index_upper, upper); }
      index_seen = true;
      return;
    }
    auto query = state;
    unsigned width = 2 * std::max< unsigned >(pointer_bits, type->bit_width()) + 2;
    auto* actual = scratch(width, "thread.window.creator.actual");
    auto* number = scratch(width, "thread.window.creator.index");
    auto* difference = scratch(width, "thread.window.creator.delta");
    query.int_apply(Unary::Cast, actual, pointer.var()->offset_var());
    query.int_apply(Unary::Cast, number, index.var());
    query.int_apply(Binary::Mul, number, number, core::MachineInt(stride, width, core::Signed));
    query.int_apply(Binary::Sub, difference, actual, number);
    if (!singleton(query.int_to_interval(difference), Z(0))) valid = false;
  }

  boost::optional< ThreadArgumentWindow > window(ar::Value* pointer,
      const value::AbstractDomain& inv, const std::string& root) {
    if (!worker) return boost::none;
    const bool initializer = source_mode && precreate_store && root == "main" &&
        same_creation_pointer(pointer, precreate_store->pointer());
    if (!initializer && root != worker->name()) return boost::none;
    ++window_queries;
    const auto& literal = ctx.lit_factory->get_scalar(pointer);
    if (!literal.is_pointer_var() || inv.first().normal().uninit_is_uninitialized(literal.var()))
      return boost::none;
    if (source_mode) {
      if (initializer) return ThreadArgumentWindow{worker, Z(0), Z(0)};
      if (numeric_mode) {
        auto* shift = numeric_shift(pointer, worker);
        auto* base_variable = shift
            ? dyn_cast_or_null< ar::InternalVariable >(uncast(shift->pointer())) : nullptr;
        auto* load = base_variable
            ? dyn_cast_or_null< ar::Load >(unique_def(base_variable)) : nullptr;
        if (!load || load->operand() != numeric_global ||
            shift->term(0).first.to_z_number() != stride) return boost::none;
        return ThreadArgumentWindow{worker, Z(0), Z(0)};
      }
      std::vector< Z > path;
      if (!ar_residual(pointer, worker, path)) return boost::none;
      return ThreadArgumentWindow{worker, path.back(), path.back()};
    }
    auto query = inv.first().normal();
    auto* offset = scratch(pointer_bits + 2, "thread.window.access.offset");
    auto* entry = scratch(pointer_bits + 2, "thread.window.access.entry");
    auto* delta = scratch(pointer_bits + 2, "thread.window.access.delta");
    auto range = native_thread_argument_window(
        query, literal.var()->offset_var(), anchor, offset, entry, delta);
    if (range.is_bottom()) return boost::none;
    return ThreadArgumentWindow{worker, range.lb().to_z_number(), range.ub().to_z_number()};
  }

  bool disjoint(const AccessRecord& a, const AccessRecord& b) const {
    if (!worker || std::getenv("IKOS_RACE_DISABLE_THREAD_ARGUMENT_CONSUMER") ||
        (numeric_mode && (!numeric_publication_seen ||
                         std::getenv("IKOS_RACE_DISABLE_NUMERIC_ARGUMENT_CONSUMER"))) ||
        !a.argument_window || !b.argument_window ||
        !a.pts.is_set() || !b.pts.is_set() ||
        a.pts.size() != 1 || b.pts.size() != 1) return false;
    ++pair_queries; // attempted window pairs, including invalid snapshots
    if (source_mode && (!source_endpoint(a) || !source_endpoint(b))) return false;
    const ThreadArgumentWindowConsumerState state{
        worker, worker->name(), stride, object, valid, create_seen, step_seen};
    const auto& x = *a.argument_window;
    const auto& y = *b.argument_window;
    const ThreadArgumentWindowAccessView left{
        x.family, *a.pts.begin(), a.thread_id, x.lower, x.upper, a.width};
    const ThreadArgumentWindowAccessView right{
        y.family, *b.pts.begin(), b.thread_id, y.lower, y.upper, b.width};
    if (source_mode && (a.stmt == precreate_store || b.stmt == precreate_store)) {
      if (std::getenv("IKOS_RACE_DISABLE_INDEXED_PRECREATE")) return false;
      const auto& parent = a.stmt == precreate_store ? left : right;
      const auto& child = a.stmt == precreate_store ? right : left;
      return state.indexed_precreate(parent, child, ctx.concurrent_env,
                                     ctx.race_analysis_complete);
    }
    return state.disjoint(left, right, ctx.concurrent_env, ctx.race_analysis_complete);
  }
};

std::shared_ptr< ThreadArgumentWindows > ThreadArgumentWindows::prepare(Context& ctx) {
  // Producer ablation keeps consumers/coverage conservative and is independent
  // from IKOS_RACE_DISABLE_THREAD_ARGUMENT_CONSUMER.
  if (std::getenv("IKOS_RACE_DISABLE_THREAD_ARGUMENT_PRODUCER") ||
      !ctx.opts.use_checks || ctx.memory_independent_ranges ||
      (ctx.opts.machine_int_domain != MachineIntDomainOption::Interval &&
       (ctx.opts.machine_int_domain != MachineIntDomainOption::VarPackApronPolkaPolyhedra ||
        !ctx.opts.use_partitioning_domain || !ctx.opts.auto_boolean_partitioning)) ||
      ctx.opts.entry_points.size() != 1 || !ctx.opts.no_init_globals.empty() ||
      ctx.bundle->global_or_null("ar.global_ctors") ||
      ctx.bundle->global_or_null("ar.global_dtors") ||
      std::find(ctx.opts.analyses.begin(), ctx.opts.analyses.end(),
                CheckerName::DataRace) == ctx.opts.analyses.end()) return nullptr;
  // Do not construct Impl before this scan. Allocation alone can perturb native
  // variable addresses/packing even when the constructor later rejects a family.
  ar::CallBase* candidate = nullptr;
  for (auto fi = ctx.bundle->function_begin(); fi != ctx.bundle->function_end(); ++fi) {
    if (!(*fi)->is_definition()) continue;
    for (auto* block : *(*fi)->body()) for (auto* stmt : *block) {
      auto* call = dyn_cast< ar::CallBase >(stmt);
      auto* api = call ? native_handle_callee(call) : nullptr;
      if (api && api->name() == "pthread_create") {
        if (candidate) return nullptr;
        candidate = call;
      }
    }
  }
  if (!candidate) return nullptr;
  return std::shared_ptr< ThreadArgumentWindows >(new ThreadArgumentWindows(ctx));
}
ThreadArgumentWindows::ThreadArgumentWindows(Context& ctx) : _impl(new Impl(ctx)) {}
ThreadArgumentWindows::~ThreadArgumentWindows() = default;
Variable* ThreadArgumentWindows::anchor(ar::Function* function) const {
  return _impl->worker == function ? _impl->anchor : nullptr;
}
void ThreadArgumentWindows::observe(ar::Statement* s, const value::AbstractDomain& i, CallContext* c) {
  _impl->observe(s, i, c);
}
boost::optional< ThreadArgumentWindow > ThreadArgumentWindows::window(
    ar::Value* p, const value::AbstractDomain& i, const std::string& root) {
  return _impl->window(p, i, root);
}
bool ThreadArgumentWindows::disjoint(const AccessRecord& a, const AccessRecord& b) const {
  return _impl->disjoint(a, b);
}
JsonDict ThreadArgumentWindows::proof(const AccessRecord& a, const AccessRecord& b) const {
  JsonDict proof;
  proof.put("thread_argument_contract", _impl->source_mode
      ? (_impl->numeric_mode ? "creation-family-numeric-index-window-v1" :
          (a.stmt == _impl->precreate_store || b.stmt == _impl->precreate_store)
          ? "creation-family-indexed-precreate-v1" : "creation-family-source-affine-window-v1")
      : "creation-family-native-window-v1");
  proof.put("family", _impl->worker->name());
  proof.put("spacing_bytes", _impl->stride.str());
  proof.put("a_lower", a.argument_window->lower.str());
  proof.put("a_upper", a.argument_window->upper.str());
  proof.put("b_lower", b.argument_window->lower.str());
  proof.put("b_upper", b.argument_window->upper.str());
  proof.put("a_width", a.width);
  proof.put("b_width", b.width);
  return proof;
}
JsonDict ThreadArgumentWindows::statistics() const {
  return JsonDict{{"candidate_family", _impl->worker != nullptr},
                  {"source_affine_mode", _impl->source_mode},
                  {"numeric_index_mode", _impl->numeric_mode},
                  {"numeric_publication_seen", _impl->numeric_publication_seen},
                  {"native_creator_queries", _impl->creator_queries},
                  {"native_window_queries", _impl->window_queries},
                  {"pair_queries", _impl->pair_queries}};
}
} // namespace analyzer
} // namespace ikos
