// Copyright and license: see context.hpp.
#include <ikos/analyzer/analysis/closed_integer_fold.hpp>
#include <ikos/analyzer/analysis/pthread_initialization_frame.hpp>
#include <ikos/analyzer/analysis/source_call_binding.hpp>
#include <ikos/analyzer/analysis/execution_engine/pthread_initialization.hpp>
#include <ikos/analyzer/analysis/execution_engine/allocation_continuation.hpp>
#include <ikos/analyzer/util/log.hpp>
#include <llvm/Analysis/ConstantFolding.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/IR/CFG.h>
#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>

namespace ikos {
namespace analyzer {
namespace {
using core::ZNumber;
struct Refuse {};
using Address = std::pair< unsigned, ZNumber >;
struct Root {
  llvm::Value* source;
  ar::CallBase* allocation;
  ar::GlobalVariable* global;
  ZNumber bytes;
};
struct Access { Address address; ZNumber bytes; bool relay_ok = false; };
struct Frame {
  std::map< llvm::Value*, Address > pointers;
  std::set< Address > held;
};
ZNumber integer(const llvm::APInt& value, bool sign) {
  llvm::SmallString< 64 > text;
  value.toString(text, 10, sign);
  return ZNumber::from_string(text.str().str());
}
}
class PthreadInitializationFrame::Impl {
  Context& ctx;
  llvm::Module* module = nullptr;
  llvm::Function* main = nullptr;
  std::map< llvm::Value*, ar::Statement* > originals;
  std::map< llvm::Function*, ar::Function* > functions;
  std::map< llvm::GlobalVariable*, ar::GlobalVariable* > globals;
  std::map< llvm::GlobalVariable*, Address > relays;
  std::map< llvm::Value*, unsigned > identities;
  std::vector< Root > roots{{nullptr, nullptr, nullptr, ZNumber(0)}};
  std::vector< Access > ordinary;
  std::set< ar::Statement* > consumed;
  std::map< Address, unsigned > attributes, mutexes;
  std::set< unsigned > born;
  std::set< llvm::Function* > active, visited;
  std::vector< llvm::Function* > workers;
  std::map< ar::CallBase*, PthreadInitializationEffect > effects;
  unsigned budget = 8192;
  PthreadObjectLayout abi;
  bool spawned = false, valid = false, heap_mutex = false, arrays = false, recursive_mode = false;
  llvm::Instruction* prefix_stop = nullptr;
  std::map< llvm::Value*, llvm::ConstantInt* > integers;
  std::map< ar::CallBase*, PthreadMutexArrayAccess > array_accesses;
  std::string refusal;

  void need(bool condition, const char* reason) {
    if (!condition) { refusal = reason; throw Refuse{}; }
  }
  void step() { need(budget != 0, "constant byte-frame budget"); --budget; }
  ar::Statement* original(llvm::Value* value) const {
    auto i = originals.find(value);
    return i == originals.end() ? nullptr : i->second;
  }
  bool value(ar::Value* a, llvm::Value* source) const {
    for (unsigned depth = 0; depth < 3 && a; ++depth) {
      if (auto* constant = llvm::dyn_cast< llvm::ConstantInt >(source)) {
        auto* actual = dyn_cast< ar::IntegerConstant >(a);
        return actual && actual->value().bit_width() == constant->getBitWidth() &&
            actual->value().cast(actual->value().bit_width(), core::Unsigned).to_z_number() == integer(constant->getValue(), false);
      }
      if (llvm::isa< llvm::ConstantPointerNull >(source)) return isa< ar::NullConstant >(a);
      auto* stripped = source->getType()->isPointerTy() ? source->stripPointerCasts() : source;
      if (auto* function = llvm::dyn_cast< llvm::Function >(stripped)) {
        if (auto* pointer = dyn_cast< ar::FunctionPointerConstant >(a))
          return pointer->function()->has_frontend() && pointer->function()->frontend< llvm::Value >() == function;
      }
      if (auto* variable = dyn_cast< ar::Variable >(a))
        if (variable->has_frontend() &&
            (variable->frontend< llvm::Value >() == source || variable->frontend< llvm::Value >() == stripped)) return true;
      auto* statement = original(source);
      if (statement && statement->result_or_null() == a) return true;
      if (auto* argument = llvm::dyn_cast< llvm::Argument >(source)) {
        auto function = functions.find(argument->getParent());
        if (function != functions.end() && function->second->param(argument->getArgNo()) == a) return true;
      }
      auto* variable = dyn_cast< ar::InternalVariable >(a);
      auto* definition = variable ? unique_def(variable) : nullptr;
      if (auto* copy = dyn_cast_or_null< ar::Assignment >(definition)) a = copy->operand();
      else if (auto* cast = dyn_cast_or_null< ar::UnaryOperation >(definition)) {
        if (cast->op() != ar::UnaryOperation::Bitcast) return false;
        a = cast->operand();
      } else return false;
    }
    return false;
  }
  bool width(ar::Type* a, llvm::Type* source) const {
    return ctx.bundle->data_layout().store_size_in_bytes(a) ==
        module->getDataLayout().getTypeStoreSize(source).getFixedValue();
  }
  unsigned global(llvm::GlobalVariable* g) {
    auto i = identities.find(g);
    if (i != identities.end()) return i->second;
    need(globals.count(g) && g->hasInitializer() && !g->isThreadLocal() &&
         !g->isExternallyInitialized() && g->getAddressSpace() == 0 &&
         g->getInitializer()->isNullValue(), "global storage origin");
    auto bytes = module->getDataLayout().getTypeAllocSize(g->getValueType()).getFixedValue();
    need(ctx.bundle->data_layout().alloc_size_in_bytes(globals.at(g)->type()->pointee()) == bytes &&
         ZNumber(bytes) <= (ZNumber(1) << (abi.word * 8 - 1)) - 1,
         "global source/AR native extent");
    unsigned id = roots.size();
    roots.push_back({g, nullptr, globals.at(g), ZNumber(bytes)});
    identities.emplace(g, id);
    return id;
  }
  // Only layout expressions with literal/NULL leaves; actual pointers never fold.
  llvm::Constant* layout(llvm::Value* value, unsigned depth = 0) {
    step(); need(depth < 24, "layout expression depth");
    if (auto* c = llvm::dyn_cast< llvm::ConstantInt >(value)) return c;
    if (arrays && integers.count(value)) return integers.at(value);
    if (auto* c = llvm::dyn_cast< llvm::ConstantPointerNull >(value)) return c;
    auto* i = llvm::dyn_cast< llvm::Instruction >(value);
    auto* e = llvm::dyn_cast< llvm::ConstantExpr >(value);
    need(i || e, "nonconstant layout input");
    unsigned op = i ? i->getOpcode() : e->getOpcode();
    need(op == llvm::Instruction::GetElementPtr || op == llvm::Instruction::PtrToInt ||
         op == llvm::Instruction::BitCast || op == llvm::Instruction::Sub,
         "unsupported layout expression");
    std::vector< llvm::Constant* > operands;
    auto* user = llvm::cast< llvm::User >(value);
    for (auto& operand : user->operands()) operands.push_back(layout(operand.get(), depth + 1));
    llvm::Constant* result = i ? llvm::ConstantFoldInstOperands(i, operands, module->getDataLayout())
        : llvm::ConstantFoldConstant(e->getWithOperands(operands), module->getDataLayout());
    need(result && !llvm::isa< llvm::UndefValue >(result) &&
         !llvm::isa< llvm::PoisonValue >(result), "indeterminate layout constant");
    return result;
  }
  Address pointer(llvm::Value* value, Frame& frame) {
    if (llvm::isa< llvm::ConstantPointerNull >(value)) return {0, ZNumber(0)};
    auto known = frame.pointers.find(value);
    if (known != frame.pointers.end()) return known->second;
    if (auto* g = llvm::dyn_cast< llvm::GlobalVariable >(value)) return {global(g), ZNumber(0)};
    if (auto* cast = llvm::dyn_cast< llvm::BitCastOperator >(value))
      return pointer(cast->getOperand(0), frame);
    auto* gep = llvm::dyn_cast< llvm::GEPOperator >(value);
    need(gep && gep->getPointerAddressSpace() == 0, "unknown pointer source");
    auto base = pointer(gep->getPointerOperand(), frame);
    // NULL-based layout expressions are never entered into the address map.
    need(base.first != 0, "layout token is not a memory address");
    const auto bits = abi.word * 8;
    const auto& data_layout = module->getDataLayout();
    const ZNumber minimum = -(ZNumber(1) << (bits - 1));
    const ZNumber maximum = (ZNumber(1) << (bits - 1)) - 1;
    ZNumber delta(0);
    // GEP indices are signed at the native index width. Prove mathematical
    // products and sums before comparing with LLVM's modular displacement.
    for (auto it = llvm::gep_type_begin(gep), end = llvm::gep_type_end(gep);
         it != end; ++it) {
      step();
      ZNumber term(0);
      if (auto* structure = it.getStructTypeOrNull()) {
        auto* index = llvm::dyn_cast< llvm::ConstantInt >(it.getOperand());
        need(index && index->getValue().ult(structure->getNumElements()),
             "constant structure field");
        term = ZNumber(data_layout.getStructLayout(structure)
                           ->getElementOffset(index->getZExtValue()));
      } else {
        auto* index = llvm::dyn_cast< llvm::ConstantInt >(layout(it.getOperand()));
        need(index, "constant sequential index");
        const auto mathematical = integer(index->getValue(), true);
        const auto native = index->getValue().sextOrTrunc(bits);
        need(mathematical == integer(native, true), "lossy native GEP index");
        term = mathematical *
            ZNumber(data_layout.getTypeAllocSize(it.getIndexedType()).getFixedValue());
      }
      need(term >= minimum && term <= maximum, "GEP byte product overflow");
      delta += term;
      need(delta >= minimum && delta <= maximum, "GEP byte sum overflow");
    }
    llvm::APInt native_delta(bits, 0);
    bool exact = gep->accumulateConstantOffset(data_layout, native_delta,
        [&](llvm::Value& index, llvm::APInt& out) {
          auto* c = llvm::dyn_cast< llvm::ConstantInt >(layout(&index));
          if (!c) return false;
          out = c->getValue().sextOrTrunc(bits);
          return true;
        });
    need(exact && delta == integer(native_delta, true),
         "native mathematical GEP correspondence");
    base.second += delta;
    need(base.second >= 0 && base.second <= roots.at(base.first).bytes, "pointer byte bounds");
    return base;
  }
  void range(const Address& a, const ZNumber& bytes) {
    need(a.first && bytes > 0 && a.second >= 0 &&
         a.second + bytes <= roots.at(a.first).bytes, "memory byte bounds");
  }
  // Interpret the actual imported address, not its opaque leaf type. The
  // source interpreter has already proved mathematical bounds; this evaluates
  // AR's native modular pointer arithmetic and demands the identical live root
  // and byte offset. It exports no pointer or numerical fact.
  ZNumber imported_index(ar::Value* value) {
    const auto bits = abi.word * 8;
    if (auto* constant = dyn_cast< ar::IntegerConstant >(value))
      return constant->value().cast(bits, core::Unsigned).to_z_number();
    auto* variable = dyn_cast< ar::InternalVariable >(value);
    auto* native = variable && variable->has_frontend()
        ? variable->frontend< llvm::Value >() : nullptr;
    auto* type = dyn_cast< ar::IntegerType >(value->type());
    need(native && type && source_integer_binding(value, native),
         "actual imported constant-prefix/layout index");
    auto* constant = scalar(native);
    if (!constant)
      constant = llvm::dyn_cast< llvm::ConstantInt >(layout(native));
    need(constant && constant->getBitWidth() == type->bit_width(),
         "same-width interpreted imported index");
    return core::MachineInt(integer(constant->getValue(), type->is_signed()),
                            type->bit_width(), type->sign())
        .cast(bits, core::Unsigned).to_z_number();
  }
  Address imported_pointer(ar::Value* value, Frame& frame, unsigned depth = 0) {
    step();
    need(value && value->type()->is_pointer() && depth < 32,
         "bounded imported pointer chain");
    if (isa< ar::NullConstant >(value)) return {0, ZNumber(0)};
    if (auto* g = dyn_cast< ar::GlobalVariable >(value)) {
      auto* source = g->has_frontend()
          ? llvm::dyn_cast< llvm::GlobalVariable >(g->frontend< llvm::Value >())
          : nullptr;
      need(source && globals.count(source) && globals.at(source) == g,
           "actual imported global root");
      return {global(source), ZNumber(0)};
    }
    auto* variable = dyn_cast< ar::Variable >(value);
    auto* source = variable && variable->has_frontend()
        ? variable->frontend< llvm::Value >() : nullptr;
    if (auto* local = dyn_cast< ar::LocalVariable >(value)) {
      auto* native = llvm::dyn_cast_or_null< llvm::AllocaInst >(source);
      auto* allocation = native
          ? dyn_cast_or_null< ar::Allocate >(original(native)) : nullptr;
      need(native && allocation && allocation->result() == local &&
           frame.pointers.count(native), "actual imported local root");
      return frame.pointers.at(native);
    }
    auto* internal = dyn_cast< ar::InternalVariable >(value);
    need(internal, "imported pointer variable");
    if (auto* formal = llvm::dyn_cast_or_null< llvm::Argument >(source)) {
      auto found = functions.find(formal->getParent());
      if (found != functions.end() &&
          found->second->param(formal->getArgNo()) == internal) {
        need(formal->getType()->isPointerTy() &&
             formal->getType()->getPointerAddressSpace() == 0 &&
             frame.pointers.count(formal), "bound imported helper pointer");
        return frame.pointers.at(formal);
      }
      // Importer-generated pointer casts may inherit the formal's frontend.
      // Only the actual AR parameter is a root; aliases follow their definition.
    }
    auto* definition = unique_def(internal);
    if (auto* copy = dyn_cast_or_null< ar::Assignment >(definition))
      return imported_pointer(copy->operand(), frame, depth + 1);
    if (auto* cast = dyn_cast_or_null< ar::UnaryOperation >(definition)) {
      need(cast->op() == ar::UnaryOperation::Bitcast &&
           cast->operand()->type()->is_pointer(), "pointer-only imported cast");
      return imported_pointer(cast->operand(), frame, depth + 1);
    }
    if (auto* shift = dyn_cast_or_null< ar::PointerShift >(definition)) {
      auto base = imported_pointer(shift->pointer(), frame, depth + 1);
      need(base.first, "imported layout token is not memory");
      const auto bits = abi.word * 8;
      const ZNumber modulus = ZNumber(1) << bits;
      ZNumber offset = base.second;
      for (auto it = shift->term_begin(); it != shift->term_end(); ++it) {
        step();
        need((*it).first.bit_width() == bits, "native imported stride width");
        offset += (*it).first.cast(bits, core::Unsigned).to_z_number() *
                  imported_index((*it).second);
      }
      base.second = offset % modulus;
      if (base.second < 0) base.second += modulus;
      need(base.second <= roots.at(base.first).bytes,
           "imported pointer within live root");
      return base;
    }
    // Allocation results and immutable relay loads have been separately bound
    // during this exact source/helper activation. No other instruction can be
    // promoted into a root from frontend metadata alone.
    if (source && definition && frame.pointers.count(source) && definition == original(source) &&
        (isa< ar::CallBase >(definition) ||
         isa< ar::Load >(definition))) {
      const auto address = frame.pointers.at(source);
      if (auto* call = dyn_cast< ar::CallBase >(definition))
        need(address.first && roots.at(address.first).allocation == call,
             "actual imported allocation result");
      return address;
    }
    need(false, "unsupported imported pointer origin");
    return {};
  }
  bool native_leaf(llvm::Value* value, const char* name, unsigned bytes) const {
    auto* p = llvm::dyn_cast< llvm::PointerType >(value->getType());
    auto* t = p && !p->isOpaque()
        ? llvm::dyn_cast< llvm::StructType >(p->getPointerElementType()) : nullptr;
    const auto& native = module->getDataLayout();
    return p && p->getAddressSpace() == 0 && t && !t->isOpaque() &&
        t->hasName() && t->getName() == name &&
        native.getTypeAllocSize(t) == bytes &&
        native.getTypeStoreSize(t) == bytes;
  }
  ar::Value* pointer_copies(ar::Value* value) {
    for (unsigned depth = 0; depth < 32; ++depth) {
      step();
      need(value && value->type()->is_pointer(), "pointer-only leaf alias");
      auto* variable = dyn_cast< ar::InternalVariable >(value);
      auto* definition = variable ? unique_def(variable) : nullptr;
      if (auto* copy = dyn_cast_or_null< ar::Assignment >(definition))
        value = copy->operand();
      else if (auto* cast = dyn_cast_or_null< ar::UnaryOperation >(definition)) {
        need(cast->op() == ar::UnaryOperation::Bitcast &&
             cast->operand()->type()->is_pointer(), "pointer-only leaf cast");
        value = cast->operand();
      } else return value;
    }
    need(false, "leaf alias depth");
    return nullptr;
  }
  bool typed(llvm::Value* value, ar::Value* imported,
             const char* name, unsigned bytes) {
    auto* actual = dyn_cast< ar::PointerType >(imported->type());
    const auto& ar_layout = ctx.bundle->data_layout();
    // Initializers retain their real typed object. Mapped lock/unlock consumers
    // use native_leaf plus a bound address to that initialized leaf instead.
    return native_leaf(value, name, bytes) && actual &&
        actual->pointee()->is_struct() &&
        ar_layout.alloc_size_in_bytes(actual->pointee()) == bytes &&
        ar_layout.store_size_in_bytes(actual->pointee()) == bytes;
  }
  void aligned(const Address& a, unsigned required) {
    need(a.second % required == 0, "runtime leaf alignment");
    if (auto* g = llvm::dyn_cast< llvm::GlobalVariable >(roots.at(a.first).source)) {
      auto alignment = g->getAlign() ? g->getAlign()->value()
          : module->getDataLayout().getABITypeAlign(g->getValueType()).value();
      need(alignment >= required, "global runtime alignment");
    } else need(roots.at(a.first).allocation != nullptr, "runtime storage lifetime");
  }
  ar::CallBase* binding(llvm::CallBase* c) {
    auto* a = source_call_binding(c, original(c));
    need(a != nullptr, "source/AR call binding");
    for (unsigned n = 0; n < c->arg_size(); ++n)
      need(value(a->argument(n), c->getArgOperand(n)) && width(a->argument(n)->type(), c->getArgOperand(n)->getType()),
           "source/AR actual argument");
    need(c->getType()->isVoidTy() ? !a->has_result() :
         a->has_result() && value(a->result(), c) && width(a->result()->type(), c->getType()),
         "source/AR call result");
    consumed.insert(a);
    return a;
  }
  void effect(ar::CallBase* call, const Address& dest, unsigned bytes, unsigned read) {
    const auto& root = roots.at(dest.first);
    auto inserted = effects.emplace(call, PthreadInitializationEffect{bytes, read, {}});
    auto& description = inserted.first->second;
    need(description.write_bytes == bytes && description.read_bytes == read, "effect shape mismatch");
    description.destinations.push_back({root.allocation, root.global, dest.second});
  }
  void call(llvm::CallBase* c, Frame& frame, bool startup) {
    auto* a = binding(c);
    auto* f = llvm::dyn_cast< llvm::Function >(c->getCalledOperand()->stripPointerCasts());
    auto* target = native_handle_callee(a);
    const auto name = f->getName();
    if (!f->isDeclaration()) {
      need(functions.count(f) && target == functions.at(f), "helper identity");
      Frame child;
      for (unsigned n = 0; n < c->arg_size(); ++n)
        if (f->getArg(n)->getType()->isPointerTy()) {
          auto address = pointer(c->getArgOperand(n), frame);
          need(imported_pointer(a->argument(n), frame) == address,
               "helper actual pointer byte correspondence");
          child.pointers.emplace(f->getArg(n), address);
        }
      child.held = frame.held;
      walk(f, child, startup);
      frame.held = std::move(child.held);
      return;
    }
    if (c->arg_empty() && c->getType()->isIntegerTy() &&
        target->intrinsic_id() == ar::Intrinsic::IkosNonDet &&
        c->getMetadata("ikos.freeze_uninit") &&
        (name == "__ikos_nondet_int" || name == "__ikos_nondet_int64")) return;
    if (name == "malloc") {
      auto* size = c->arg_size() == 1 ? llvm::dyn_cast< llvm::ConstantInt >(c->getArgOperand(0)) : nullptr;
      need(startup && !spawned && c->getFunction() == main && size && size->getBitWidth() == abi.word * 8 &&
           size->getZExtValue() > 0 && size->getZExtValue() <= 1048576 &&
           target->intrinsic_id() == ar::Intrinsic::LibcMalloc && c->getType()->isPointerTy(),
           "one-shot constant allocation");
      unsigned id = roots.size();
      roots.push_back({c, a, nullptr, integer(size->getValue(), false)});
      frame.pointers.emplace(c, Address{id, ZNumber(0)});
      return;
    }
    bool attr_init = name == "pthread_mutexattr_init";
    bool attr_set = name == "pthread_mutexattr_settype";
    bool mutex_init = name == "pthread_mutex_init";
    if (attr_init || attr_set || mutex_init) {
      need(startup && !spawned && c->getType()->isIntegerTy(32) &&
           target->has_frontend() && target->frontend< llvm::Value >() == f &&
           target->name() == name.str() && c->arg_size() == (attr_init ? 1u : 2u),
           "initialization declaration/prefix");
      if (arrays) {
        auto whole_attribute = [](llvm::Value* v) {
          for (unsigned depth = 0; depth < 16; ++depth) {
            if (llvm::isa< llvm::GlobalVariable >(v)) return true;
            auto* cast = llvm::dyn_cast< llvm::BitCastOperator >(v);
            if (!cast) return false;
            v = cast->getOperand(0);
          }
          return false;
        };
        need(mutex_init ? (llvm::isa< llvm::ConstantPointerNull >(c->getArgOperand(1)) ||
                           whole_attribute(c->getArgOperand(1)))
                        : whole_attribute(c->getArgOperand(0)),
             "whole global attribute without byte arithmetic");
      }
      if (arrays && mutex_init) {
        auto* gep = llvm::dyn_cast< llvm::GEPOperator >(c->getArgOperand(0));
        auto* g = gep ? llvm::dyn_cast< llvm::GlobalVariable >(gep->getPointerOperand()) : nullptr;
        auto* type = g ? llvm::dyn_cast< llvm::ArrayType >(g->getValueType()) : nullptr;
        auto* zero = gep && gep->getNumIndices() == 2
            ? llvm::dyn_cast< llvm::ConstantInt >(gep->getOperand(1)) : nullptr;
        auto* index = zero ? scalar(gep->getOperand(2)) : nullptr;
        need(type && gep->getSourceElementType() == type && zero && zero->isZero() && index &&
             integer(index->getValue(), true) >= 0 &&
             integer(index->getValue(), true) < ZNumber(type->getNumElements()),
             "actual initialization index without truncation/wrap");
      }
      auto dest = pointer(c->getArgOperand(0), frame);
      need(imported_pointer(a->argument(0), frame) == dest,
           "initialization source/AR byte address");
      range(dest, ZNumber(mutex_init ? abi.mutex : 4));
      aligned(dest, mutex_init ? abi.word : 4);
      need(typed(c->getArgOperand(0), a->argument(0), mutex_init ? "union.pthread_mutex_t" : "union.pthread_mutexattr_t",
                 mutex_init ? abi.mutex : 4), "runtime opaque type/layout");
      if (attr_init) {
        need(roots.at(dest.first).global && dest.second == 0 &&
             roots.at(dest.first).bytes == 4 && !attributes.count(dest), "attribute lifetime/one init");
        need(!arrays || !llvm::cast< llvm::GlobalVariable >(roots.at(dest.first).source)->isConstant(),
             "writable attribute storage");
        attributes.emplace(dest, 0);
      } else if (attr_set) {
        auto* kind = llvm::dyn_cast< llvm::ConstantInt >(c->getArgOperand(1));
        need(attributes.count(dest) && kind && kind->getBitWidth() == 32 &&
             (kind->isZero() || kind->equalsInt(2)), "default attribute flags/type");
        attributes.at(dest) = kind->getZExtValue();
      } else {
        unsigned kind = 0;
        bool nonnull = !llvm::isa< llvm::ConstantPointerNull >(c->getArgOperand(1));
        if (nonnull) {
          auto attr = pointer(c->getArgOperand(1), frame);
          need(attributes.count(attr) && imported_pointer(a->argument(1), frame) == attr &&
               typed(c->getArgOperand(1), a->argument(1), "union.pthread_mutexattr_t", 4),
               "initialized attribute input");
          kind = attributes.at(attr);
        }
        need(((roots.at(dest.first).allocation && born.count(dest.first)) ||
              (arrays && roots.at(dest.first).global && dest.second % abi.mutex == 0)) &&
             mutexes.emplace(dest, kind).second, "live storage and distinct mutex initialization");
        heap_mutex = true;
      }
      effect(a, dest, mutex_init ? abi.mutex : 4,
             mutex_init && !llvm::isa< llvm::ConstantPointerNull >(c->getArgOperand(1)) ? 4 : 0);
      return;
    }
    if (name == "pthread_mutex_lock" || name == "pthread_mutex_unlock") {
      need(c->arg_size() == 1 && c->getType()->isIntegerTy(32) &&
           target->intrinsic_id() == (name == "pthread_mutex_lock" ?
             ar::Intrinsic::PthreadMutexLock : ar::Intrinsic::PthreadMutexUnlock), "mutex API identity");
      auto dest = pointer(c->getArgOperand(0), frame);
      need(mutexes.count(dest) &&
           native_leaf(c->getArgOperand(0), "union.pthread_mutex_t", abi.mutex) &&
           imported_pointer(a->argument(0), frame) == dest,
           "source/AR address of initialized mutex consumer");
      if (name == "pthread_mutex_lock")
        need(frame.held.insert(dest).second, "repeated acquisition");
      else if (!frame.held.erase(dest))
        need(mutexes.at(dest) == 2, "wrong NORMAL unlock");
      return;
    }
    if (name == "pthread_create") {
      need(startup && c->getFunction() == main, "create activation");
      need(native_handle_api(c, true), "native create ABI");
      need(target->intrinsic_id() == ar::Intrinsic::PthreadCreate ||
           (target->has_frontend() && target->frontend< llvm::Value >() == f &&
            target->name() == "pthread_create"), "AR create identity");
      need(unread_private_thread_output(a), "private create output/callback");
      auto output = pointer(c->getArgOperand(0), frame);
      need(imported_pointer(a->argument(0), frame) == output,
           "create source/AR output address");
      range(output, ZNumber(abi.word));
      auto* storage = llvm::dyn_cast< llvm::AllocaInst >(roots.at(output.first).source);
      need(storage && output.second == 0 && roots.at(output.first).bytes == abi.word &&
           storage->getAlign().value() >= abi.word, "private native output bounds");
      auto* worker = llvm::cast< llvm::Function >(c->getArgOperand(2)->stripPointerCasts());
      workers.push_back(worker); spawned = true; return;
    }
    need(false, "uncovered library effect");
  }
  std::vector< llvm::Instruction* > linear_body(llvm::Function* f) {
    std::vector< llvm::Instruction* > result;
    std::set< llvm::BasicBlock* > seen;
    auto* block = &f->getEntryBlock();
    llvm::BasicBlock* previous = nullptr;
    while (block) {
      step();
      need(seen.insert(block).second &&
           (!previous || (llvm::pred_size(block) == 1 &&
                          *llvm::pred_begin(block) == previous)),
           "unique finite initialization continuation");
      auto* branch = llvm::dyn_cast< llvm::BranchInst >(block->getTerminator());
      llvm::BasicBlock* next = nullptr;
      llvm::ICmpInst* comparison = nullptr;
      if (branch) {
        need(f == main, "single-block direct helper");
        next = branch->getSuccessor(0);
        if (branch->isConditional()) {
          comparison = llvm::dyn_cast< llvm::ICmpInst >(branch->getCondition());
          need(comparison && comparison->isEquality() && comparison->hasOneUse() &&
               comparison->getParent() == block, "allocation-only branch");
          auto* left = comparison->getOperand(0);
          auto* right = comparison->getOperand(1);
          auto* source = llvm::isa< llvm::ConstantPointerNull >(left) ? right :
                         llvm::isa< llvm::ConstantPointerNull >(right) ? left : nullptr;
          auto* allocation = source ? llvm::dyn_cast< llvm::CallBase >(
              source->stripPointerCasts()) : nullptr;
          auto* actual = allocation ? dyn_cast_or_null< ar::CallBase >(
              original(allocation)) : nullptr;
          ar::Comparison* guard = nullptr;
          need(allocation && allocation->getParent() == block && actual &&
               heap_initialization_successor(actual->parent(), guard) && guard &&
               guard->has_frontend() && guard->frontend< llvm::Value >() == comparison,
               "complete pure-abort allocation check");
          auto* value = dyn_cast< ar::InternalVariable >(
              isa< ar::NullConstant >(guard->left()) ? guard->right() : guard->left());
          need(value && unique_def(value) == actual, "actual checked allocation");
          ar::BasicBlock* ar_failure = nullptr;
          for (auto it = actual->parent()->successor_begin();
               it != actual->parent()->successor_end(); ++it)
            if (*it != guard->parent()) {
              need(!ar_failure, "unique imported failure successor");
              ar_failure = *it;
            }
          need(ar_failure && !ar_failure->empty(), "matched imported failure block");
          auto* equal = dyn_cast< ar::Comparison >(*ar_failure->begin());
          need(equal && equal->has_frontend() &&
               equal->frontend< llvm::Value >() == comparison,
               "source/AR complementary comparison");
          const unsigned failed = comparison->getPredicate() == llvm::CmpInst::ICMP_EQ ? 0 : 1;
          auto* failure = branch->getSuccessor(failed);
          next = branch->getSuccessor(failed ^ 1);
          need(failure != next && seen.insert(failure).second && failure->size() == 2,
               "terminal failure block");
          auto* terminal = llvm::dyn_cast< llvm::CallInst >(&failure->front());
          auto* imported = terminal ? binding(terminal) : nullptr;
          auto* target = imported ? native_handle_callee(imported) : nullptr;
          need(target && std::distance(ar_failure->begin(), ar_failure->end()) == 3 &&
               *std::next(ar_failure->begin()) == imported &&
               target->intrinsic_id() == ar::Intrinsic::LibcAbort &&
               llvm::isa< llvm::UnreachableInst >(failure->back()),
               "source/AR terminal failure");
        }
      } else need(llvm::isa< llvm::ReturnInst >(block->getTerminator()),
                  "complete returning initialization body");
      for (auto& i : *block)
        if (&i != branch && &i != comparison) result.push_back(&i);
      previous = block; block = next;
    }
    need(seen.size() == f->size(), "unaccounted initialization blocks");
    return result;
  }
  void walk(llvm::Function* f, Frame& frame, bool startup) {
    need(functions.count(f) && (f == main || f->size() == 1) && active.size() < 16 && active.insert(f).second &&
         f->getCallingConv() == llvm::CallingConv::C && !f->isVarArg() &&
         ordinary_source_function_abi_attributes(f), "finite direct helper tree");
    visited.insert(f);
    for (auto* source : linear_body(f)) {
      auto& i = *source;
      step();
      if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) call(c, frame, startup);
      else if (auto* alloca = llvm::dyn_cast< llvm::AllocaInst >(&i)) {
        auto* count = llvm::dyn_cast< llvm::ConstantInt >(alloca->getArraySize());
        auto* actual = dyn_cast_or_null< ar::Allocate >(original(alloca));
        auto* actual_count = actual
            ? dyn_cast< ar::IntegerConstant >(actual->array_size()) : nullptr;
        auto* actual_type = actual
            ? dyn_cast< ar::IntegerType >(actual->allocated_type()) : nullptr;
        bool native_output = false;
        if (f == main && alloca->getAllocatedType()->isIntegerTy(abi.word * 8)) {
          for (auto& block : *main) for (auto& instruction : block) {
            step();
            auto* call = llvm::dyn_cast< llvm::CallBase >(&instruction);
            if (!call || !native_handle_api(call, true) ||
                call->getArgOperand(0)->stripPointerCasts() != alloca) continue;
            auto* imported = dyn_cast_or_null< ar::CallBase >(original(call));
            if (imported && unread_private_thread_output(imported))
              native_output = true;
          }
        }
        need(f == main &&
             (alloca->getAllocatedType()->isIntegerTy(32) || native_output) &&
             count && count->isOne() && alloca->getAddressSpace() == 0 &&
             actual && actual_count && actual_count->value().to_z_number() == 1 &&
             actual_type &&
             actual_type->bit_width() == alloca->getAllocatedType()->getIntegerBitWidth() &&
             value(actual->result(), alloca) &&
             ctx.bundle->data_layout().alloc_size_in_bytes(actual_type) ==
                 module->getDataLayout().getTypeAllocSize(alloca->getAllocatedType()).getFixedValue(),
             "private source/AR scalar stack storage");
        consumed.insert(actual);
        const auto bytes = module->getDataLayout()
                               .getTypeAllocSize(alloca->getAllocatedType()).getFixedValue();
        unsigned id = roots.size();
        roots.push_back({alloca, nullptr, nullptr, ZNumber(bytes)});
        frame.pointers.emplace(alloca, Address{id, ZNumber(0)});
      } else if (auto* store = llvm::dyn_cast< llvm::StoreInst >(&i)) {
        auto* actual = dyn_cast_or_null< ar::Store >(original(store));
        need(actual && !store->isAtomic() && !store->isVolatile() &&
             !actual->is_atomic() && !actual->is_volatile() &&
             value(actual->pointer(), store->getPointerOperand()) &&
             value(actual->value(), store->getValueOperand()) && width(actual->value()->type(), store->getValueOperand()->getType()),
             "store correspondence");
        consumed.insert(actual);
        auto dest = pointer(store->getPointerOperand(), frame);
        need(imported_pointer(actual->pointer(), frame) == dest,
             "store source/AR byte address");
        ZNumber bytes(module->getDataLayout().getTypeStoreSize(store->getValueOperand()->getType()).getFixedValue());
        range(dest, bytes); ordinary.push_back({dest, bytes});
        if (store->getValueOperand()->getType()->isPointerTy()) {
          auto* g = llvm::dyn_cast< llvm::GlobalVariable >(roots.at(dest.first).source);
          need(startup && !spawned && g && g->getValueType()->isPointerTy() &&
               dest.second == 0 && bytes == abi.word && !relays.count(g), "immutable global pointer relay");
          auto origin = pointer(store->getValueOperand(), frame);
          need(imported_pointer(actual->value(), frame) == origin,
               "relay source/AR pointer value");
          need(origin.first && roots.at(origin.first).allocation && origin.second == 0,
               "relay allocation origin");
          relays.emplace(g, origin);
          ordinary.back().relay_ok = true;
        } else {
          need(store->getValueOperand()->getType()->isIntegerTy(), "noninteger application effect");
          if (startup && !spawned && roots.at(dest.first).allocation && dest.second == 0)
            born.insert(dest.first);
        }
      } else if (auto* load = llvm::dyn_cast< llvm::LoadInst >(&i)) {
        auto* actual = dyn_cast_or_null< ar::Load >(original(load));
        need(actual && !load->isAtomic() && !load->isVolatile() &&
             !actual->is_atomic() && !actual->is_volatile() && value(actual->operand(), load->getPointerOperand()) &&
             value(actual->result(), load) && width(actual->result()->type(), load->getType()), "load correspondence");
        consumed.insert(actual);
        auto source = pointer(load->getPointerOperand(), frame);
        need(imported_pointer(actual->operand(), frame) == source,
             "load source/AR byte address");
        ZNumber bytes(module->getDataLayout().getTypeStoreSize(load->getType()).getFixedValue());
        range(source, bytes); ordinary.push_back({source, bytes});
        if (load->getType()->isPointerTy()) {
          auto* g = llvm::dyn_cast< llvm::GlobalVariable >(roots.at(source.first).source);
          need(g && relays.count(g) && source.second == 0 && bytes == abi.word, "closed pointer load");
          frame.pointers.emplace(load, relays.at(g));
          ordinary.back().relay_ok = true;
        } else need(load->getType()->isIntegerTy(), "noninteger application load");
      } else if (auto* ret = llvm::dyn_cast< llvm::ReturnInst >(&i)) {
        auto* value = ret->getReturnValue();
        need(!value || value->getType()->isIntegerTy() || llvm::isa< llvm::ConstantPointerNull >(value),
             "pointer return escape");
      } else if (i.getType()->isPointerTy()) {
        // An unused NULL-based layout token cannot serve as a memory address.
        auto* gep = llvm::dyn_cast< llvm::GEPOperator >(&i);
        if (gep && llvm::isa< llvm::ConstantPointerNull >(gep->getPointerOperand())) layout(&i);
        else frame.pointers.emplace(&i, pointer(&i, frame));
      } else if (llvm::isa< llvm::PtrToIntInst >(&i)) layout(&i);
      else {
        need(llvm::isa< llvm::BinaryOperator >(&i) || llvm::isa< llvm::CastInst >(&i), "unsupported CFG/instruction");
        for (auto& operand : i.operands()) need(!operand->getType()->isPointerTy(), "numeric pointer escape");
      }
    }
    active.erase(f);
  }
  llvm::ConstantInt* scalar(llvm::Value* v) const {
    if (auto* c = llvm::dyn_cast< llvm::ConstantInt >(v)) return c;
    auto i = integers.find(v);
    return i == integers.end() ? nullptr : i->second;
  }
  // Enumerate only a deterministic main prefix. Unknown control stops it; all
  // initialization instructions must subsequently be accounted for by closure.
  void array_prefix() {
    Frame frame;
    llvm::BasicBlock* block = &main->getEntryBlock();
    llvm::BasicBlock* previous = nullptr;
    while (block) {
      std::map< llvm::Value*, llvm::ConstantInt* > phis;
      for (auto& phi : block->phis()) {
        step(); need(previous != nullptr && !phi.getType()->isPointerTy(), "prefix PHI");
        auto* c = scalar(phi.getIncomingValueForBlock(previous));
        need(c, "nonconstant prefix PHI"); phis.emplace(&phi, c);
      }
      for (const auto& phi : phis) integers[phi.first] = phi.second;
      llvm::BasicBlock* next = nullptr;
      for (auto& i : *block) {
        step(); prefix_stop = &i;
        if (llvm::isa< llvm::PHINode >(i)) continue;
        integers.erase(&i); frame.pointers.erase(&i);
        if (auto* branch = llvm::dyn_cast< llvm::BranchInst >(&i)) {
          if (branch->isUnconditional()) next = branch->getSuccessor(0);
          else {
            auto* c = scalar(branch->getCondition());
            if (!c) return;
            next = branch->getSuccessor(c->isZero() ? 1 : 0);
          }
        } else if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) {
          auto* f = llvm::dyn_cast< llvm::Function >(c->getCalledOperand()->stripPointerCasts());
          need(f, "prefix direct call");
          if (f->getName() == "pthread_mutexattr_init" || f->getName() == "pthread_mutexattr_settype" ||
              f->getName() == "pthread_mutex_init") {
            auto* actual = dyn_cast_or_null< ar::CallBase >(original(c));
            need(f->isDeclaration() && actual && pthread_initialization_signature(actual),
                 "native initialization declaration");
            call(c, frame, true);
          }
          else {
            auto* a = source_call_binding(c, original(c));
            if (!a || !native_handle_callee(a) ||
                native_handle_callee(a)->intrinsic_id() != ar::Intrinsic::IkosNonDet ||
                !c->arg_empty() || !c->getType()->isIntegerTy()) return;
          }
        } else if (llvm::isa< llvm::ReturnInst >(i)) return;
        else if (llvm::isa< llvm::AllocaInst >(i) || llvm::isa< llvm::StoreInst >(i)) {
          // No memory-derived value is used by this prefix interpreter; the
          // complete storage closure below separately rejects runtime writes.
        } else if (i.getType()->isPointerTy()) {
          // Resolve pointer expressions afresh at each actual init occurrence.
        } else if (llvm::isa< llvm::BinaryOperator >(i) || llvm::isa< llvm::ICmpInst >(i) ||
                   llvm::isa< llvm::TruncInst >(i) || llvm::isa< llvm::SExtInst >(i) ||
                   llvm::isa< llvm::ZExtInst >(i)) {
          std::vector< llvm::Constant* > operands;
          for (auto& operand : i.operands()) {
            auto* c = scalar(operand.get());
            if (!c) { operands.clear(); break; }
            operands.push_back(c);
          }
          if (operands.empty()) continue;
          auto* c = closed_integer_fold(&i, operands, module->getDataLayout());
          need(c, "closed constant prefix result");
          integers[&i] = c;
        } else if (!llvm::isa< llvm::LoadInst >(i)) need(false, "unsupported prefix instruction");
      }
      previous = block; block = next;
    }
  }
  void array_run() {
    array_prefix();
    need(!mutexes.empty() && !effects.empty(), "no array initialization");
    std::map< ar::Value*, unsigned > objects;
    std::unordered_set< ar::CallBase* > covered;
    for (const auto& effect : effects) {
      covered.insert(effect.first);
    }
    need(prefix_stop, "finite prefix exit");
    std::vector< llvm::BasicBlock* > suffix{prefix_stop->getParent()};
    std::set< llvm::BasicBlock* > seen;
    bool first = true;
    while (!suffix.empty()) {
      auto* block = suffix.back(); suffix.pop_back();
      if (!seen.insert(block).second) continue;
      bool after = !first;
      first = false;
      for (auto& instruction : *block) {
        step();
        if (&instruction == prefix_stop) after = true;
        if (!after) continue;
        auto* c = llvm::dyn_cast< llvm::CallBase >(&instruction);
        if (!c) continue;
        auto* f = llvm::dyn_cast< llvm::Function >(c->getCalledOperand()->stripPointerCasts());
        need(!f || (f->getName() != "pthread_mutexattr_init" &&
             f->getName() != "pthread_mutexattr_settype" && f->getName() != "pthread_mutex_init"),
             "suffix can reenter initialization");
      }
      for (auto* next : llvm::successors(block)) {
        // Reentering the suffix start requires checking its entire block.
        if (next == prefix_stop->getParent()) need(false, "suffix cycle to prefix boundary");
        suffix.push_back(next);
      }
    }
    std::set< unsigned > mutex_roots;
    std::set< ar::GlobalVariable* > mutex_arrays;
    for (const auto& leaf : mutexes) mutex_roots.insert(leaf.first.first);
    for (unsigned id : mutex_roots) {
      const auto& root = roots.at(id);
      auto* source = llvm::dyn_cast< llvm::GlobalVariable >(root.source);
      auto* type = source ? llvm::dyn_cast< llvm::ArrayType >(source->getValueType()) : nullptr;
      auto* element = type ? llvm::dyn_cast< llvm::StructType >(type->getElementType()) : nullptr;
      need(root.global && type && type->getNumElements() > 0 && type->getNumElements() <= 128 &&
           element && !element->isOpaque() && element->hasName() &&
           element->getName() == "union.pthread_mutex_t" &&
           module->getDataLayout().getTypeAllocSize(element) == abi.mutex &&
           module->getDataLayout().getTypeStoreSize(element) == abi.mutex &&
           root.bytes == ZNumber(type->getNumElements()) * abi.mutex, "homogeneous native mutex array");
      for (unsigned index = 0; index < type->getNumElements(); ++index)
        need(mutexes.count({id, ZNumber(index) * abi.mutex}), "complete initialized array");
      need(!source->isConstant(), "writable runtime array");
      objects.emplace(root.global, 2); mutex_arrays.insert(root.global);
    }
    for (const auto& attr : attributes) objects.emplace(roots.at(attr.first.first).global, 1);
    std::set< ar::CallBase* > consumers;
    need(pthread_initialization_storage_closed(ctx.bundle, objects, covered, budget,
                                              &mutex_arrays, &consumers),
         "closed array runtime storage");
    // Every non-init use of these arrays must be an actual typed mutex leaf.
    // Dynamic indices become obligations consumed at the final checker invariant.
    for (const auto& mapping : originals) {
      step();
      auto* a = dyn_cast< ar::CallBase >(mapping.second);
      if (!a || covered.count(a) || !pthread_initialized_mutex_consumer(a)) continue;
      auto* c = llvm::dyn_cast< llvm::CallBase >(mapping.first);
      need(c && c->arg_size() == 1 && binding(c) == a, "runtime consumer source");
      auto* gep = llvm::dyn_cast< llvm::GEPOperator >(c->getArgOperand(0));
      if (!gep) continue;
      auto* g = llvm::dyn_cast< llvm::GlobalVariable >(gep->getPointerOperand());
      if (!g || !identities.count(g) || !mutex_roots.count(identities.at(g))) continue;
      auto* zero = gep->getNumIndices() == 2 ? llvm::dyn_cast< llvm::ConstantInt >(gep->getOperand(1)) : nullptr;
      need(zero && zero->isZero() && gep->getOperand(2)->getType()->isIntegerTy(abi.word * 8) &&
           native_leaf(c->getArgOperand(0), "union.pthread_mutex_t", abi.mutex),
           "typed native array leaf expression");
      // All leaves of this exact AR global were initialized with matching
      // source/AR layout above. Consumer arguments may canonically be opaque;
      // the complete actual root/stride/index chain is checked below.
      auto* leaf = pointer_copies(a->argument(0));
      ar::Value* index = nullptr;
      for (auto* block : *a->code()) for (auto* statement : *block) {
        step();
        auto* shift = dyn_cast< ar::PointerShift >(statement);
        if (!shift || shift->result() != leaf || shift->pointer() != globals.at(g)) continue;
        auto* result = dyn_cast< ar::InternalVariable >(leaf);
        need(result && unique_def(result) == shift &&
             (!shift->has_frontend() || shift->frontend< llvm::Value >() == gep),
             "unique source/AR leaf definition");
        unsigned nonzero = 0;
        for (auto it = shift->term_begin(); it != shift->term_end(); ++it) {
          auto* constant = dyn_cast< ar::IntegerConstant >((*it).second);
          if (constant && constant->value().is_zero()) continue;
          need((*it).first == abi.mutex && source_integer_binding((*it).second, gep->getOperand(2)), "source/AR leaf stride/index");
          index = (*it).second; ++nonzero;
        }
        need(nonzero <= 1, "array displacement shape");
        if (!nonzero) {
          for (auto it = shift->term_begin(); it != shift->term_end(); ++it)
            if ((*it).first == abi.mutex && source_integer_binding((*it).second, gep->getOperand(2))) index = (*it).second;
        }
      }
      if (!index) {
        auto* native_zero = llvm::dyn_cast< llvm::ConstantInt >(gep->getOperand(2));
        if (native_zero && native_zero->isZero() &&
            exact_handle_slot(a->argument(0), globals.at(g))) {
          index = ar::IntegerConstant::get(ctx.bundle->context(),
              ar::IntegerType::size_type(ctx.bundle), 0);
        }
      }
      need(index, "complete source/AR leaf mapping");
      array_accesses.emplace(a, PthreadMutexArrayAccess{
          globals.at(g), index,
          llvm::cast< llvm::ArrayType >(g->getValueType())->getNumElements()});
    }
    for (auto* consumer : consumers)
      need(array_accesses.count(consumer), "unmapped mutex array consumer");
    // Runtime storage cannot be changed by a hidden external effect. Preserve
    // independent debts, and refuse this success contract on unknown calls.
    for (const auto& mapping : originals) {
      step();
      auto* a = dyn_cast< ar::CallBase >(mapping.second);
      if (!a || covered.count(a)) continue;
      auto* c = llvm::dyn_cast< llvm::CallBase >(mapping.first);
      auto* f = c ? llvm::dyn_cast< llvm::Function >(c->getCalledOperand()->stripPointerCasts()) : nullptr;
      auto* target = native_handle_callee(a);
      need(f && target && source_call_binding(c, a) == a,
           "closed array direct calls");
      if (!f->isDeclaration()) continue; // Every body is scanned by AR closure.
      if (pthread_initialized_mutex_consumer(a)) continue;
      const auto name = f->getName();
      const bool internal_nondet =
          target->intrinsic_id() == ar::Intrinsic::IkosNonDet &&
          c->getMetadata("ikos.freeze_uninit") &&
          ((name == "__ikos_nondet_int" && c->getType()->isIntegerTy(32)) ||
           (name == "__ikos_nondet_int64" && c->getType()->isIntegerTy(64)));
      auto* returned = dyn_cast< ar::IntegerType >(target->type()->return_type());
      const bool verifier_nondet =
          name == "__VERIFIER_nondet_int" && f->arg_empty() &&
          f->getReturnType()->isIntegerTy(32) &&
          c->getType()->isIntegerTy(32) &&
          target->has_frontend() && target->frontend< llvm::Value >() == f &&
          returned && returned->bit_width() == 32 && returned->is_signed();
      if (c->arg_empty() && (internal_nondet || verifier_nondet)) continue;
      if (name == "abort" && c->arg_empty() && c->getType()->isVoidTy() &&
          target->intrinsic_id() == ar::Intrinsic::LibcAbort) continue;
      need(f->getName() == "pthread_create" && native_handle_api(c, true) &&
           unread_private_thread_output(a), "uncovered array library effect");
    }
    for (auto& effect : effects) effect.second.complete_prefix = true;
    valid = true;
  }
  #include "recursive_mutex_frame.inc"

  void run() {
    need(ctx.opts.enable_thread_modular && ctx.opts.use_checks &&
         std::find(ctx.opts.analyses.begin(), ctx.opts.analyses.end(), CheckerName::DataRace) != ctx.opts.analyses.end() &&
         ctx.opts.entry_points.size() == 1 &&
         ctx.opts.entry_points.front()->name() == "main" && ctx.opts.entry_points.front()->num_parameters() == 0 &&
         ctx.opts.no_init_globals.empty() &&
         !ctx.bundle->global_or_null("ar.global_ctors") && !ctx.bundle->global_or_null("ar.global_dtors"),
         "entry/ABI/options");
    for (auto it = ctx.bundle->function_begin(); it != ctx.bundle->function_end(); ++it) {
      auto* f = *it;
      if (!f->is_definition()) continue;
      need(f->has_frontend(), "function source");
      auto* source = f->frontend< llvm::Function >(); functions.emplace(source, f);
      if (f == ctx.opts.entry_points.front()) { main = source; module = source->getParent(); }
      for (auto* block : *f->body()) for (auto* s : *block) {
        step();
        if (isa< ar::Load >(s) || isa< ar::Store >(s) || isa< ar::CallBase >(s) || isa< ar::Allocate >(s))
          need(s->has_frontend() && originals.emplace(s->frontend< llvm::Value >(), s).second,
               "complete source memory/call mapping");
      }
    }
    need(main && main->use_empty() && module->getModuleInlineAsm().empty(), "closed main/representations");
    abi = pthread_object_layout(ctx.bundle, module);
    need(abi && ctx.bundle->data_layout().is_little_endian() &&
         module->getDataLayout().getIndexSizeInBits(0) == abi.word * 8,
         "GNU native/AR pointer and index ABI");
    for (auto it = ctx.bundle->global_begin(); it != ctx.bundle->global_end(); ++it) {
      auto* g = *it;
      if (g->has_frontend()) globals.emplace(g->frontend< llvm::GlobalVariable >(), g);
    }
    if (recursive_mode) { recursive_run(); return; }
    if (arrays) { array_run(); return; }
    Frame frame; walk(main, frame, true);
    need(heap_mutex && !workers.empty(), "no heap initialization use");
    for (auto* worker : workers) {
      Frame child; child.pointers.emplace(worker->getArg(0), Address{0, ZNumber(0)});
      walk(worker, child, false);
    }
    for (auto* function : visited)
      for (auto* block : *functions.at(function)->body()) for (auto* statement : *block)
        if (isa< ar::Load >(statement) || isa< ar::Store >(statement) || isa< ar::CallBase >(statement) || isa< ar::Allocate >(statement))
          need(consumed.count(statement), "unconsumed AR memory/call effect");
    for (const auto& access : ordinary) {
      for (const auto& relay : relays) {
        auto id = identities.at(relay.first);
        need(access.address.first != id || access.address.second >= abi.word || access.relay_ok,
             "ordinary pointer-relay representation access");
      }
      for (const auto& leaf : mutexes)
        need(access.address.first != leaf.first.first || access.address.second + access.bytes <= leaf.first.second ||
             leaf.first.second + abi.mutex <= access.address.second, "ordinary runtime mutex-byte access");
      for (const auto& leaf : attributes)
        need(access.address.first != leaf.first.first || access.address.second + access.bytes <= leaf.first.second ||
             leaf.first.second + 4 <= access.address.second, "ordinary attribute-byte access");
    }
    for (auto a = mutexes.begin(); a != mutexes.end(); ++a)
      for (auto b = std::next(a); b != mutexes.end(); ++b)
        need(a->first.first != b->first.first || a->first.second + abi.mutex <= b->first.second ||
             b->first.second + abi.mutex <= a->first.second, "overlapping runtime mutex leaves");
    valid = true;
  }
public:
  explicit Impl(Context& context, bool array_mode = false, bool recursive = false)
      : ctx(context), arrays(array_mode), recursive_mode(recursive) {
    try { run(); } catch (const Refuse&) { effects.clear(); }
  }
  const PthreadInitializationEffect* get(ar::CallBase* call) const {
    auto i = effects.find(call);
    return valid && i != effects.end() ? &i->second : nullptr;
  }
  const PthreadMutexArrayAccess* access(ar::CallBase* call) const {
    auto i = array_accesses.find(call);
    return valid && i != array_accesses.end() ? &i->second : nullptr;
  }
  const PthreadRecursiveMutexEffect* recursive_effect(ar::CallBase* call) const {
    auto it = recursive_effects.find(call);
    return valid && it != recursive_effects.end() ? &it->second : nullptr;
  }
  const PthreadRecursiveMutexRegion* recursive_region(ar::GlobalVariable* global, ar::Function* root) const {
    auto it = recursive_regions.find({global, root});
    return valid && it != recursive_regions.end() ? &it->second : nullptr;
  }
  bool recursive_object(ar::GlobalVariable* global) const { return valid && recursive_objects.count(global); }
  bool recursive_attribute(ar::LocalVariable* local) const { return valid && recursive_attributes.count(local); }
  bool recursive_key(std::uint64_t key) const {
    if (!valid || !key || (key & 0xffffffffULL)) return false;
    for (auto* global : recursive_objects) {
      auto* location = ctx.mem_factory->get_global(global);
      auto address = core::IndexableTraits< MemoryLocation* >::index(location);
      if (address && address <= 0xffffffffULL && key == (address << 32)) return true;
    }
    return false;
  }
  bool ok() const { return valid; }
  const std::string& reason() const { return refusal; }
};
bool PthreadInitializationEffect::matches(MemoryLocation* location, const ZNumber& offset) const {
  for (const auto& target : destinations) {
    if (offset != target.offset) continue;
    if (auto* heap = dyn_cast< DynAllocMemoryLocation >(location)) {
      if (target.allocation && heap->call() == target.allocation && heap->context()->empty()) return true;
    } else if (auto* global = dyn_cast< GlobalMemoryLocation >(location)) {
      if (target.global && global->global_var() == target.global) return true;
    } else if (auto* local = dyn_cast< LocalMemoryLocation >(location))
      if (target.local && local->local_var() == target.local) return true;
  }
  return false;
}
PthreadInitializationFrame::PthreadInitializationFrame(Context& ctx) : _impl(new Impl(ctx)) {
  if (!_impl->ok()) _impl.reset(new Impl(ctx, true));
  if (!_impl->ok()) _impl.reset(new Impl(ctx, false, true));
}
PthreadInitializationFrame::~PthreadInitializationFrame() = default;
const PthreadInitializationEffect* PthreadInitializationFrame::effect(ar::CallBase* c) const { return _impl->get(c); }
const PthreadMutexArrayAccess* PthreadInitializationFrame::array_access(ar::CallBase* c) const { return _impl->access(c); }
bool PthreadInitializationEffect::matches_range(MemoryLocation* location, const ZNumber& lower, const ZNumber& upper) const {
  if (lower == upper) return matches(location, lower);
  if (!complete_prefix || lower > upper) return false;
  // The source prefix proves all actual offsets. This hull is only an abstract
  // state compatibility check; it does not certify leaf membership.
  bool first = true;
  ZNumber lo(0), hi(0);
  for (const auto& target : destinations) {
    auto* global = dyn_cast< GlobalMemoryLocation >(location);
    if (!global || !target.global || global->global_var() != target.global) return false;
    if (first) { lo = hi = target.offset; first = false; }
    else { lo = std::min(lo, target.offset); hi = std::max(hi, target.offset); }
  }
  return !first && lower >= lo && upper <= hi;
}
const PthreadRecursiveMutexEffect* PthreadInitializationFrame::recursive_mutex(ar::CallBase* c) const { return _impl->recursive_effect(c); }
const PthreadRecursiveMutexRegion* PthreadInitializationFrame::recursive_region(ar::GlobalVariable* g, ar::Function* f) const { return _impl->recursive_region(g, f); }
bool PthreadInitializationFrame::recursive_object(ar::GlobalVariable* g) const { return _impl->recursive_object(g); }
bool PthreadInitializationFrame::recursive_attribute(ar::LocalVariable* l) const { return _impl->recursive_attribute(l); }
bool PthreadInitializationFrame::recursive_key(std::uint64_t key) const { return _impl->recursive_key(key); }
const std::string& PthreadInitializationFrame::refusal_reason() const { return _impl->reason(); }
void configure_pthread_initialization_frame(Context& ctx) {
  if (ctx.pthread_initialization_frame()) return;
  ctx._pthread_initialization_frame = std::make_shared< const PthreadInitializationFrame >(ctx);
  if (!ctx.pthread_initialization_frame()->refusal_reason().empty())
    log::debug("Heap pthread initialization frame refused: " + ctx.pthread_initialization_frame()->refusal_reason());
}
} // namespace analyzer
} // namespace ikos
