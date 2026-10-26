// Copyright and license: see data_race.hpp.
#include <ikos/analyzer/checker/thread_argument_windows.hpp>
#include <ikos/analyzer/checker/data_race.hpp>
#include <ikos/analyzer/checker/thread_argument_window_consumer.hpp>
#include <ikos/analyzer/analysis/call_context.hpp>
#include <ikos/analyzer/analysis/execution_engine/native_thread_handles.hpp>
#include <llvm/IR/Operator.h>
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
} // namespace

class ThreadArgumentWindows::Impl {
public:
  Context& ctx;
  ar::Function* worker = nullptr;
  ar::CallBase* create = nullptr;
  ar::Value* base = nullptr;
  ar::InternalVariable* counter = nullptr;
  ar::BinaryOperation* step = nullptr;
  Z stride{0};
  Variable* anchor = nullptr;
  MemoryLocation* object = nullptr;
  bool create_seen = false, step_seen = false, valid = true;
  unsigned pointer_bits;
  std::size_t creator_queries = 0, window_queries = 0;
  mutable std::size_t pair_queries = 0;

  explicit Impl(Context& context) : ctx(context),
      pointer_bits(context.bundle->data_layout().pointers.bit_width) {
    // ponytail: start with the native relational configuration already tested;
    // additional domains need their own projection/cost checks.
    if (ctx.opts.machine_int_domain != MachineIntDomainOption::VarPackApronPolkaPolyhedra ||
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
    if (!shift || shift->num_terms() != 1 || shift->parent() != candidate->parent()) return;
    auto term = shift->term(0);
    ar::Value* index_value = term.second;
    // Discover the induction variable through native width adapters only.
    for (unsigned depth = 0; depth < 32; ++depth) {
      auto* variable = dyn_cast< ar::InternalVariable >(index_value);
      auto* conversion = variable
          ? dyn_cast_or_null< ar::UnaryOperation >(unique_def(variable)) : nullptr;
      if (!conversion || (conversion->op() != ar::UnaryOperation::SExt &&
                          conversion->op() != ar::UnaryOperation::ZExt &&
                          conversion->op() != ar::UnaryOperation::Bitcast) ||
          !conversion->operand()->type()->is_integer()) break;
      index_value = conversion->operand();
    }
    auto* index = dyn_cast< ar::InternalVariable >(index_value);
    auto* integer = index ? dyn_cast< ar::IntegerType >(index->type()) : nullptr;
    if (!integer || integer->bit_width() > 64 || term.first.to_z_number() <= 0) return;
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
      if (auto* cast = dyn_cast< ar::UnaryOperation >(stmt)) {
        if (cast->op() != ar::UnaryOperation::Bitcast &&
            cast->op() != ar::UnaryOperation::SExt &&
            cast->op() != ar::UnaryOperation::ZExt) return;
      } else if (!isa< ar::Comparison >(stmt) && !isa< ar::PointerShift >(stmt)) return;
    }
    for (auto* stmt : *header)
      if (!isa< ar::UnaryOperation >(stmt)) return;
    // A base defined inside the family cannot be a stable coordinate.
    auto* base_var = dyn_cast_or_null< ar::InternalVariable >(uncast(shift->pointer()));
    auto* base_def = base_var ? unique_def(base_var) : nullptr;
    if (!base_def || base_def->parent() == body || base_def->parent() == header ||
        !isa< ar::CallBase >(base_def)) return;
    create = candidate; worker = child; counter = index; step = advance;
    base = shift->pointer(); stride = term.first.to_z_number();
    auto* type = ar::IntegerType::get(ctx.bundle->context(), pointer_bits, core::Unsigned);
    anchor = ctx.var_factory->get_named_shadow(type, "thread.argument.anchor." + worker->name());
  }

  Variable* scratch(unsigned bits, const char* name) {
    return ctx.var_factory->get_named_shadow(
        ar::IntegerType::get(ctx.bundle->context(), bits, core::Signed), name);
  }

  void observe(ar::Statement* stmt, const value::AbstractDomain& inv, CallContext* cc) {
    if (!worker || (stmt != create && stmt != step)) return;
    // The certified creation belongs to the sole main activation, not a helper.
    if (cc && cc->has_parent()) { valid = false; return; }
    const auto& index = ctx.lit_factory->get_scalar(counter);
    auto range = inv.first().normal().int_to_interval(index.var());
    const auto* type = cast< ar::IntegerType >(counter->type());
    if (range.is_bottom() || range.lb().to_z_number() < 0 ||
        range.ub().to_z_number() >= core::MachineInt::max(type->bit_width(), type->sign()).to_z_number())
      valid = false;
    if (stmt == step) { step_seen = true; return; }
    create_seen = true;
    ++creator_queries;
    auto max = (Z(1) << pointer_bits) - 1;
    if (range.is_bottom() || stride * range.ub().to_z_number() > max) { valid = false; return; }
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
    if (!worker || root != worker->name()) return boost::none;
    ++window_queries;
    const auto& literal = ctx.lit_factory->get_scalar(pointer);
    if (!literal.is_pointer_var() || inv.first().normal().uninit_is_uninitialized(literal.var()))
      return boost::none;
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
        !a.argument_window || !b.argument_window ||
        !a.pts.is_set() || !b.pts.is_set() ||
        a.pts.size() != 1 || b.pts.size() != 1) return false;
    ++pair_queries; // attempted native-window pairs, including invalid snapshots
    const ThreadArgumentWindowConsumerState state{
        worker, worker->name(), stride, object, valid, create_seen, step_seen};
    const auto& x = *a.argument_window;
    const auto& y = *b.argument_window;
    const ThreadArgumentWindowAccessView left{
        x.family, *a.pts.begin(), a.thread_id, x.lower, x.upper, a.width};
    const ThreadArgumentWindowAccessView right{
        y.family, *b.pts.begin(), b.thread_id, y.lower, y.upper, b.width};
    return state.disjoint(left, right, ctx.concurrent_env, ctx.race_analysis_complete);
  }
};

std::shared_ptr< ThreadArgumentWindows > ThreadArgumentWindows::prepare(Context& ctx) {
  // Producer ablation keeps consumers/coverage conservative and is independent
  // from IKOS_RACE_DISABLE_THREAD_ARGUMENT_CONSUMER.
  if (std::getenv("IKOS_RACE_DISABLE_THREAD_ARGUMENT_PRODUCER") ||
      !ctx.opts.use_checks || ctx.memory_independent_ranges ||
      ctx.opts.machine_int_domain != MachineIntDomainOption::VarPackApronPolkaPolyhedra ||
      !ctx.opts.use_partitioning_domain || !ctx.opts.auto_boolean_partitioning ||
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
  proof.put("thread_argument_contract", "creation-family-native-window-v1");
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
                  {"native_creator_queries", _impl->creator_queries},
                  {"native_window_queries", _impl->window_queries},
                  {"pair_queries", _impl->pair_queries}};
}
} // namespace analyzer
} // namespace ikos
