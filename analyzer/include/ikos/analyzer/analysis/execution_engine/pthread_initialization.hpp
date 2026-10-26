// Copyright and license: see numerical.hpp.
#pragma once

#include <llvm/ADT/Triple.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

#include <ikos/analyzer/analysis/source_call_binding.hpp>

#include <ikos/analyzer/analysis/context.hpp>
#include <ikos/analyzer/analysis/execution_engine/concurrent_semantics.hpp>

namespace ikos {
namespace analyzer {

inline ar::Function* pthread_initialization_callee(ar::CallBase* call) {
  ar::Value* value = call->called();
  for (unsigned depth = 0; depth != 32; ++depth) {
    if (auto* function = dyn_cast< ar::FunctionPointerConstant >(value))
      return function->function();
    auto* variable = dyn_cast< ar::InternalVariable >(value);
    auto* definition = variable ? unique_def(variable) : nullptr;
    if (auto* copy = dyn_cast_or_null< ar::Assignment >(definition))
      value = copy->operand();
    else if (auto* cast = dyn_cast_or_null< ar::UnaryOperation >(definition)) {
      if (cast->op() != ar::UnaryOperation::Bitcast)
        return nullptr;
      value = cast->operand();
    } else
      return nullptr;
  }
  return nullptr;
}

// Share the checker exemption table with the storage-use certificate.
inline unsigned synchronization_ignored_arguments(ar::CallBase* call,
                                                  ar::Function* exact) {
  if (!exact || !exact->is_declaration())
    return 0;
  unsigned ignored = 0;
  struct SyncArguments {
    const char* name;
    std::size_t arity;
    unsigned ignored;
  };
  static const SyncArguments summaries[] =
      {{"pthread_mutex_lock", 1, 1},
       {"pthread_mutex_unlock", 1, 1},
       {"pthread_mutex_trylock", 1, 1},
       {"pthread_mutex_timedlock", 2, 1},
       {"pthread_mutex_init", 2, 1},
       {"pthread_mutex_destroy", 1, 1},
       {"pthread_mutex_consistent", 1, 1},
       {"pthread_rwlock_rdlock", 1, 1},
       {"pthread_rwlock_wrlock", 1, 1},
       {"pthread_rwlock_unlock", 1, 1},
       {"pthread_rwlock_tryrdlock", 1, 1},
       {"pthread_rwlock_trywrlock", 1, 1},
       {"pthread_rwlock_timedrdlock", 2, 1},
       {"pthread_rwlock_timedwrlock", 2, 1},
       {"pthread_rwlock_init", 2, 1},
       {"pthread_rwlock_destroy", 1, 1},
       {"pthread_spin_lock", 1, 1},
       {"pthread_spin_unlock", 1, 1},
       {"pthread_spin_trylock", 1, 1},
       {"pthread_spin_init", 2, 1},
       {"pthread_spin_destroy", 1, 1},
       {"pthread_barrier_wait", 1, 1},
       {"pthread_barrier_init", 3, 1},
       {"pthread_barrier_destroy", 1, 1},
       {"pthread_cond_wait", 2, 3},
       {"pthread_cond_timedwait", 3, 3},
       {"pthread_cond_signal", 1, 1},
       {"pthread_cond_broadcast", 1, 1},
       {"pthread_cond_init", 2, 1},
       {"pthread_cond_destroy", 1, 1},
       {"pthread_once", 2, 3},
       // create writes arg0, reads attr arg1, and merely passes arg3 to the
       // child.
       {"pthread_create", 4, 12},
       {"sem_init", 3, 1},
       {"sem_destroy", 1, 1},
       {"sem_wait", 1, 1},
       {"sem_trywait", 1, 1},
       {"sem_timedwait", 2, 1},
       {"sem_post", 1, 1},
       {"sem_close", 1, 1},
       {"sem_getvalue", 2, 1}};
  auto* returned = dyn_cast< ar::IntegerType >(exact->type()->return_type());
  if (returned != nullptr && returned->bit_width() == 32 &&
      returned->is_signed()) {
    for (const auto& summary : summaries) {
      if (exact->name() == summary.name &&
          exact->num_parameters() == summary.arity &&
          call->num_arguments() == summary.arity) {
        ignored = summary.ignored;
        break;
      }
    }
    // AR intrinsics carry the contract even after source names are translated.
    if (exact->intrinsic_id() == ar::Intrinsic::PthreadMutexLock ||
        exact->intrinsic_id() == ar::Intrinsic::PthreadMutexUnlock) {
      if (call->num_arguments() == 1)
        ignored = 1;
    } else if (exact->intrinsic_id() == ar::Intrinsic::PthreadCreate) {
      if (call->num_arguments() == 4)
        ignored = 12;
    }
    for (std::size_t i = 0; i < call->num_arguments() && i < 4; ++i) {
      if ((ignored & (1u << i)) != 0 &&
          !call->argument(i)->type()->is_pointer()) {
        ignored = 0;
        break;
      }
    }
  }
  return ignored;
}

inline unsigned synchronization_runtime_arguments(ar::CallBase* call,
                                                  ar::Function* exact) {
  if (!exact)
    return 0;
  if (exact->name() == "pthread_create" ||
      exact->intrinsic_id() == ar::Intrinsic::PthreadCreate)
    return 0;
  unsigned ignored = synchronization_ignored_arguments(call, exact);
  // once's callback is not runtime storage; create's callback/user args are not
  // either.
  return exact->name() == "pthread_once" ? ignored & 1u : ignored;
}

// Verify the source ABI, not just a suggestive extern name or pointer cast.
inline bool pthread_initialization_signature(ar::CallBase* call) {
  auto* function = pthread_initialization_callee(call);
  if (!function || !function->is_declaration())
    return false;
  const auto& name = function->name();
  if (name != "pthread_mutexattr_init" && name != "pthread_mutexattr_settype" &&
      name != "pthread_mutexattr_destroy" && name != "pthread_mutex_init")
    return false;
  unsigned count =
      name == "pthread_mutexattr_settype" || name == "pthread_mutex_init" ? 2
                                                                          : 1;
  auto* result = dyn_cast< ar::IntegerType >(function->type()->return_type());
  auto* source =
      call->has_frontend()
          ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >())
          : nullptr;
  auto* declaration = source
                          ? llvm::dyn_cast< llvm::Function >(
                                source->getCalledOperand()->stripPointerCasts())
                          : nullptr;
  if (!result || result->bit_width() != 32 || !result->is_signed() ||
      call->num_arguments() != count || !declaration ||
      !declaration->isDeclaration() || declaration->getName() != name ||
      declaration->isVarArg() || declaration->arg_size() != count ||
      source->arg_size() != count ||
      !declaration->getReturnType()->isIntegerTy(32) ||
      !source->getType()->isIntegerTy(32) ||
      source->getCallingConv() != llvm::CallingConv::C ||
      declaration->getCallingConv() != llvm::CallingConv::C)
    return false;
  for (unsigned i = 0; i != count; ++i) {
    auto* formal = declaration->getFunctionType()->getParamType(i);
    auto* actual = source->getArgOperand(i)->getType();
    if (name == "pthread_mutexattr_settype" && i == 1) {
      if (!formal->isIntegerTy(32) || !actual->isIntegerTy(32))
        return false;
    } else if (!formal->isPointerTy() || !actual->isPointerTy())
      return false;
  }
  return true;
}

inline ar::GlobalVariable* pthread_initialization_object(ar::Value* value,
                                                         bool mutex) {
  ar::Value* base = value;
  for (unsigned depth = 0; depth != 32; ++depth) {
    if (isa< ar::GlobalVariable >(base))
      break;
    auto* variable = dyn_cast< ar::InternalVariable >(base);
    if (!variable)
      return nullptr;
    auto* definition = unique_def(variable);
    if (auto* copy = dyn_cast_or_null< ar::Assignment >(definition))
      base = copy->operand();
    else if (auto* cast = dyn_cast_or_null< ar::UnaryOperation >(definition)) {
      if (cast->op() != ar::UnaryOperation::Bitcast)
        return nullptr;
      base = cast->operand();
    } else
      return nullptr;
  }
  auto* global = dyn_cast< ar::GlobalVariable >(base);
  if (!global || !global->is_definition() || global->is_thread_local() ||
      !global->has_frontend() || !exact_handle_slot(value, global))
    return nullptr;
  auto* source = global->frontend< llvm::GlobalVariable >();
  auto* type = llvm::dyn_cast< llvm::StructType >(source->getValueType());
  const llvm::Triple target(source->getParent()->getTargetTriple());
  const auto bits = global->bundle()->data_layout().pointers.bit_width;
  if (!target.isOSLinux() || target.getEnvironment() != llvm::Triple::GNU ||
      !((target.getArch() == llvm::Triple::x86 && bits == 32) ||
        (target.getArch() == llvm::Triple::x86_64 && bits == 64)) ||
      !type || type->isOpaque() || !type->hasName() || source->isConstant() ||
      type->getName() !=
          (mutex ? "union.pthread_mutex_t" : "union.pthread_mutexattr_t"))
    return nullptr;
  const auto& layout = source->getParent()->getDataLayout();
  unsigned width = mutex ? (bits == 32 ? 24 : 40) : 4;
  unsigned alignment = mutex ? bits / 8 : 4;
  auto allocated_alignment = source->getAlign()
                                 ? source->getAlign()->value()
                                 : layout.getABITypeAlign(type).value();
  return layout.getTypeAllocSize(type) == width &&
                 allocated_alignment >= alignment
             ? global
             : nullptr;
}

inline bool pthread_initialized_mutex_consumer(ar::CallBase* call) {
  auto* source =
      call->has_frontend()
          ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >())
          : nullptr;
  auto* function = source ? llvm::dyn_cast< llvm::Function >(
                                source->getCalledOperand()->stripPointerCasts())
                          : nullptr;
  return function && function->isDeclaration() && !function->isVarArg() &&
         (function->getName() == "pthread_mutex_lock" ||
          function->getName() == "pthread_mutex_unlock" ||
          function->getName() == "pthread_mutex_trylock") &&
         function->arg_size() == 1 && source->arg_size() == 1 &&
         call->num_arguments() == 1 &&
         function->getReturnType()->isIntegerTy(32) &&
         source->getType()->isIntegerTy(32) &&
         function->getFunctionType()->getParamType(0)->isPointerTy() &&
         source->getArgOperand(0)->getType()->isPointerTy() &&
         function->getCallingConv() == llvm::CallingConv::C &&
         source->getCallingConv() == llvm::CallingConv::C;
}

// This supplemental frame proves only disjointness from sealed global/stack
// runtime storage. It exports no nonnull, validity, uniqueness or value fact.
// The old frame remains authoritative; failed refinement retains its rejection.
class PthreadInitializationFreshStorage {
  ar::Bundle* _bundle;
  const std::vector< ar::Statement* >& _statements;
  const std::map< ar::Value*, unsigned >& _objects;
  unsigned& _budget;

  bool step() { if (_budget == 0) return false; --_budget; return true; }
  ar::Statement* definition(ar::InternalVariable* value) {
    ar::Statement* result = nullptr;
    for (auto* statement : _statements) {
      if (!step()) return nullptr;
      if (statement->result_or_null() != value) continue;
      if (result) return nullptr;
      result = statement;
    }
    return result;
  }
  ar::Statement* original(llvm::Instruction* source) {
    ar::Statement* result = nullptr;
    for (auto* statement : _statements) {
      if (!step()) return nullptr;
      if (!statement->has_frontend() ||
          statement->frontend< llvm::Value >() != source) continue;
      // Importer casts can share metadata with a call. Only the actual call
      // participates; its actual callee/arguments are checked separately.
      if (llvm::isa< llvm::CallBase >(source) && !isa< ar::CallBase >(statement))
        continue;
      if (result) return nullptr;
      result = statement;
    }
    return result;
  }
  llvm::Value* native_zero_base(llvm::Value* source) {
    for (unsigned depth = 0; depth != 32; ++depth) {
      if (!step() || !source || !source->getType()->isPointerTy() ||
          llvm::cast< llvm::PointerType >(source->getType())->getAddressSpace())
        return nullptr;
      if (auto* cast = llvm::dyn_cast< llvm::BitCastOperator >(source)) {
        source = cast->getOperand(0); continue;
      }
      if (auto* gep = llvm::dyn_cast< llvm::GEPOperator >(source)) {
        for (auto& index : gep->indices()) {
          auto* constant = llvm::dyn_cast< llvm::ConstantInt >(index.get());
          if (!constant || !constant->isZero()) return nullptr;
        }
        source = gep->getPointerOperand(); continue;
      }
      return source;
    }
    return nullptr;
  }
  bool private_runtime_roots() {
    for (const auto& item : _objects) {
      if (!step()) return false;
      auto* base = direct_byte_object(item.first);
      if (auto* global = dyn_cast_or_null< ar::GlobalVariable >(base)) {
        if (!global->has_frontend()) return false;
        auto* native = global->frontend< llvm::GlobalVariable >();
        if (!native->hasLocalLinkage() || native->isExternallyInitialized())
          return false;
      } else if (!dyn_cast_or_null< ar::LocalVariable >(base)) return false;
    }
    for (auto* statement : _statements) {
      if (!step()) return false;
      auto* call = dyn_cast< ar::CallBase >(statement);
      if (!call) continue;
      auto* target = pthread_initialization_callee(call);
      if (!target || target->intrinsic_id() == ar::Intrinsic::LibcFree ||
          target->name() == "free" || target->name() == "realloc" ||
          target->name() == "ar.libc.realloc" ||
          (target->type()->return_type()->is_pointer() &&
           target->intrinsic_id() != ar::Intrinsic::LibcMalloc)) return false;
    }
    return true;
  }
  // Real create with the same callback in source and AR. No formal substitution
  // is licensed by a name, frontend annotation or one observed spawn alone.
  ar::CallBase* create(llvm::CallBase* native, llvm::Function* worker) {
    auto* call = dyn_cast_or_null< ar::CallBase >(original(native));
    auto* target = call ? pthread_initialization_callee(call) : nullptr;
    auto* function = llvm::dyn_cast< llvm::Function >(
        native->getCalledOperand()->stripPointerCasts());
    const unsigned bits = _bundle->data_layout().pointers.bit_width;
    if (!call || !target || !source_call_binding(native, call) || !function ||
        !function->isDeclaration() || function->getName() != "pthread_create" ||
        function->arg_size() != 4 || native->arg_size() != 4 ||
        !native->use_empty() || !native->getType()->isIntegerTy(32) ||
        !function->getReturnType()->isIntegerTy(32) ||
        !pthread_object_layout(bits, native->getModule()) ||
        !target->is_declaration() ||
        (target->intrinsic_id() != ar::Intrinsic::PthreadCreate &&
         target->name() != "pthread_create") ||
        !target->has_frontend() || target->frontend< llvm::Value >() != function ||
        call->num_arguments() != 4 ||
        !isa< ar::NullConstant >(call->argument(1)) ||
        !llvm::isa< llvm::ConstantPointerNull >(native->getArgOperand(1)) ||
        native->getArgOperand(2) != worker || worker->isDeclaration() ||
        worker->isVarArg() || worker->arg_size() != 1 ||
        worker->getCallingConv() != llvm::CallingConv::C ||
        !worker->getReturnType()->isPointerTy() ||
        !worker->getArg(0)->getType()->isPointerTy() ||
        !ordinary_source_function_abi_attributes(worker) ||
        llvm::cast< llvm::PointerType >(worker->getReturnType())->getAddressSpace() ||
        llvm::cast< llvm::PointerType >(worker->getArg(0)->getType())->getAddressSpace() ||
        native->getModule()->getDataLayout().getPointerSizeInBits() != bits)
      return nullptr;
    for (unsigned i = 0; i != 4; ++i) {
      auto* type = llvm::dyn_cast< llvm::PointerType >(native->getArgOperand(i)->getType());
      if (!type || type->getAddressSpace() || type->isOpaque() ||
          type != function->getFunctionType()->getParamType(i)) return nullptr;
      if ((i == 0 && !type->getPointerElementType()->isIntegerTy(bits)) ||
          (i == 2 && type->getPointerElementType() != worker->getFunctionType()))
        return nullptr;
    }
    auto* actual = dyn_cast< ar::FunctionPointerConstant >(call->argument(2));
    if (!actual || !actual->function()->is_definition() ||
        !actual->function()->has_frontend() ||
        actual->function()->frontend< llvm::Value >() != worker ||
        actual->function()->num_parameters() != 1 ||
        !actual->function()->param(0)->type()->is_pointer() ||
        !actual->function()->type()->return_type()->is_pointer()) return nullptr;
    return call;
  }
  // Full source use closure (all CFG arms). Every extra pointer escape is a
  // refusal, including a stored pointer, free, realloc, helper or integer cast.
  bool closed_pointer_uses(llvm::Value* root) {
    std::vector< llvm::Value* > pending{root};
    std::set< llvm::Value* > seen;
    while (!pending.empty()) {
      auto* value = pending.back(); pending.pop_back();
      if (!seen.insert(value).second) continue;
      for (auto* user : value->users()) {
        if (!step()) return false;
        if (auto* cast = llvm::dyn_cast< llvm::BitCastOperator >(user)) {
          if (!cast->getType()->isPointerTy()) return false;
          pending.push_back(cast); continue;
        }
        if (auto* gep = llvm::dyn_cast< llvm::GEPOperator >(user)) {
          auto* base = native_zero_base(gep);
          if (!base || base != native_zero_base(value)) return false;
          pending.push_back(gep); continue;
        }
        if (auto* load = llvm::dyn_cast< llvm::LoadInst >(user)) {
          if (load->getPointerOperand() != value || !load->getType()->isIntegerTy())
            return false;
          continue;
        }
        if (auto* store = llvm::dyn_cast< llvm::StoreInst >(user)) {
          if (store->getPointerOperand() != value ||
              !store->getValueOperand()->getType()->isIntegerTy()) return false;
          continue;
        }
        if (auto* compare = llvm::dyn_cast< llvm::ICmpInst >(user)) {
          if (!compare->isEquality()) return false;
          auto* other = compare->getOperand(compare->getOperand(0) == value ? 1 : 0);
          if (!llvm::isa< llvm::ConstantPointerNull >(other)) return false;
          continue;
        }
        auto* call = llvm::dyn_cast< llvm::CallBase >(user);
        auto* worker = call && call->arg_size() == 4
            ? llvm::dyn_cast< llvm::Function >(call->getArgOperand(2)) : nullptr;
        if (!call || !worker || call->getArgOperand(3) != value ||
            !create(call, worker)) return false;
      }
    }
    return true;
  }
  bool extent(ar::Value* value, llvm::Value* native,
              core::ZNumber& bytes, std::set< ar::Value* >& active) {
    if (!step() || !value || !value->type()->is_pointer() ||
        !active.insert(value).second) return false;
    auto* variable = dyn_cast< ar::InternalVariable >(value);
    auto* source = native_zero_base(native);
    if (!variable || !source) return false;
    auto* function = variable->code()->function_or_null();
    if (function && function->num_parameters() == 1 && function->param(0) == variable) {
      auto* argument = llvm::dyn_cast< llvm::Argument >(source);
      auto* worker = argument ? argument->getParent() : nullptr;
      if (!worker || argument->getArgNo() != 0 || !function->has_frontend() ||
          function->frontend< llvm::Value >() != worker ||
          !worker->hasLocalLinkage() || !closed_pointer_uses(argument)) return false;
      bool found = false;
      for (auto* user : worker->users()) {
        if (!step()) return false;
        auto* c = llvm::dyn_cast< llvm::CallBase >(user);
        auto* a = c ? create(c, worker) : nullptr;
        if (!a) return false;
        // A recursive create or any unsupported incoming actual rejects.
        auto ancestors = active;
        core::ZNumber incoming;
        if (!extent(a->argument(3), c->getArgOperand(3), incoming, ancestors))
          return false;
        if (!found || incoming < bytes) bytes = incoming;
        found = true;
      }
      // Independently audit every imported occurrence of the callback address.
      // Aggregate/indirect/copy uses do not fit this narrow profile.
      for (auto* s : _statements) for (auto it = s->op_begin(); it != s->op_end(); ++it) {
        if (!step()) return false;
        auto* pointer = dyn_cast< ar::FunctionPointerConstant >(*it);
        if (!pointer || pointer->function() != function) continue;
        auto* a = dyn_cast< ar::CallBase >(s);
        auto* c = a && a->has_frontend()
            ? llvm::dyn_cast< llvm::CallBase >(a->frontend< llvm::Value >()) : nullptr;
        if (!c || create(c, worker) != a || a->argument(2) != *it) return false;
      }
      return found;
    }
    auto* d = definition(variable);
    if (auto* copy = dyn_cast_or_null< ar::Assignment >(d))
      return extent(copy->operand(), native, bytes, active);
    if (auto* cast = dyn_cast_or_null< ar::UnaryOperation >(d))
      return cast->op() == ar::UnaryOperation::Bitcast &&
             cast->operand()->type()->is_pointer() &&
             extent(cast->operand(), native, bytes, active);
    if (auto* shift = dyn_cast_or_null< ar::PointerShift >(d)) {
      for (auto it = shift->term_begin(); it != shift->term_end(); ++it) {
        auto* index = dyn_cast< ar::IntegerConstant >((*it).second);
        if (!index || !index->value().is_zero()) return false;
      }
      return extent(shift->pointer(), native, bytes, active);
    }
    auto* allocation = dyn_cast_or_null< ar::CallBase >(d);
    auto* c = llvm::dyn_cast< llvm::CallBase >(source);
    auto* f = c ? llvm::dyn_cast< llvm::Function >(
        c->getCalledOperand()->stripPointerCasts()) : nullptr;
    auto* target = allocation ? pthread_initialization_callee(allocation) : nullptr;
    const auto bits = _bundle->data_layout().pointers.bit_width;
    if (!allocation || !c || !allocation->has_frontend() ||
        allocation->frontend< llvm::Value >() != c ||
        !source_call_binding(c, allocation) || !f || !f->isDeclaration() ||
        f->getName() != "malloc" || !target ||
        target->intrinsic_id() != ar::Intrinsic::LibcMalloc ||
        !target->has_frontend() || target->frontend< llvm::Value >() != f ||
        c->arg_size() != 1 || !c->getArgOperand(0)->getType()->isIntegerTy(bits) ||
        !f->getReturnType()->isPointerTy() || !c->getType()->isPointerTy() ||
        c->getModule()->getDataLayout().getPointerSizeInBits() != bits ||
        !closed_pointer_uses(c)) return false;
    auto* count = llvm::dyn_cast< llvm::ConstantInt >(c->getArgOperand(0));
    auto* actual = dyn_cast< ar::IntegerConstant >(allocation->argument(0));
    if (!count || !actual || count->getValue().getActiveBits() > 63 ||
        count->isZero() || actual->value().bit_width() != bits ||
        actual->value().cast(bits, core::Unsigned).to_z_number() !=
            core::ZNumber(count->getZExtValue())) return false;
    bytes = core::ZNumber(count->getZExtValue());
    return true;
  }
public:
  PthreadInitializationFreshStorage(
      ar::Bundle* bundle, const std::vector< ar::Statement* >& statements,
      const std::map< ar::Value*, unsigned >& objects, unsigned& budget)
      : _bundle(bundle), _statements(statements), _objects(objects), _budget(budget) {}
  bool scalar_access(ar::Statement* statement) {
    if (!private_runtime_roots() || !statement->has_frontend()) return false;
    ar::Value* address = nullptr;
    ar::Type* datum = nullptr;
    llvm::Value* native_address = nullptr;
    llvm::Type* native_datum = nullptr;
    if (auto* load = dyn_cast< ar::Load >(statement)) {
      auto* native = llvm::dyn_cast< llvm::LoadInst >(statement->frontend< llvm::Value >());
      if (!native || load->is_atomic() != native->isAtomic() ||
          load->is_volatile() != native->isVolatile()) return false;
      address = load->operand(); datum = load->result()->type();
      native_address = native->getPointerOperand(); native_datum = native->getType();
    } else if (auto* store = dyn_cast< ar::Store >(statement)) {
      auto* native = llvm::dyn_cast< llvm::StoreInst >(statement->frontend< llvm::Value >());
      if (!native || store->is_atomic() != native->isAtomic() ||
          store->is_volatile() != native->isVolatile()) return false;
      address = store->pointer(); datum = store->value()->type();
      native_address = native->getPointerOperand(); native_datum = native->getValueOperand()->getType();
    } else return false;
    auto* integer = dyn_cast< ar::IntegerType >(datum);
    if (!integer || !native_datum->isIntegerTy(integer->bit_width()) ||
        (integer->bit_width() != 32 && integer->bit_width() != 64)) return false;
    std::set< ar::Value* > active;
    core::ZNumber bytes;
    return extent(address, native_address, bytes, active) &&
           _bundle->data_layout().store_size_in_bytes(datum) <= bytes;
  }
  bool create_argument(ar::CallBase* call, std::size_t index) {
    if (index != 3 || !private_runtime_roots() || !call->has_frontend()) return false;
    auto* source = llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >());
    auto* worker = source && source->arg_size() == 4
        ? llvm::dyn_cast< llvm::Function >(source->getArgOperand(2)) : nullptr;
    if (!worker || create(source, worker) != call) return false;
    std::set< ar::Value* > active;
    core::ZNumber bytes;
    return extent(call->argument(index), source->getArgOperand(index), bytes, active);
  }
};

// Additional initialized-mutex consumer, at arg1 only. Keep exact source ABI,
// real disjoint whole global condition storage and the existing storage closure.
inline bool pthread_initialized_condition_consumer(ar::CallBase* call) {
  auto* source = call->has_frontend()
      ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >()) : nullptr;
  auto* function = source ? llvm::dyn_cast< llvm::Function >(
      source->getCalledOperand()->stripPointerCasts()) : nullptr;
  if (!function || !source_call_binding(source, call) || !function->isDeclaration() ||
      function->getName() != "pthread_cond_wait" || function->arg_size() != 2 ||
      source->arg_size() != 2 || !source->getType()->isIntegerTy(32) ||
      !function->getReturnType()->isIntegerTy(32)) return false;
  auto* mutex = pthread_initialization_object(call->argument(1), true);
  auto* condition = dyn_cast_or_null< ar::GlobalVariable >(direct_byte_object(call->argument(0)));
  if (!mutex || !condition || condition == mutex || !condition->has_frontend() ||
      !condition->is_definition() || condition->is_thread_local() ||
      !exact_handle_slot(call->argument(0), condition)) return false;
  auto* native = condition->frontend< llvm::GlobalVariable >();
  auto* native_mutex = mutex->frontend< llvm::GlobalVariable >();
  // This extension uses a static default condition lifetime only. A separate
  // initialization/destruction protocol needs its own certificate.
  if (!native->hasInitializer() || !native->getInitializer()->isNullValue())
    return false;
  unsigned budget = 8192;
  for (auto& function : *native->getParent())
    for (auto& block : function) for (auto& instruction : block) {
      if (budget == 0) return false;
      --budget;
      auto* c = llvm::dyn_cast< llvm::CallBase >(&instruction);
      auto* f = c ? llvm::dyn_cast< llvm::Function >(
          c->getCalledOperand()->stripPointerCasts()) : nullptr;
      if (f && (f->getName() == "pthread_cond_init" ||
                f->getName() == "pthread_cond_destroy")) return false;
    }
  auto* type = llvm::dyn_cast< llvm::StructType >(native->getValueType());
  const auto abi = pthread_object_layout(call->bundle(), native->getParent());
  const auto& layout = native->getParent()->getDataLayout();
  const auto alignment = native->getAlign() ? native->getAlign()->value()
      : layout.getABITypeAlign(native->getValueType()).value();
  return abi && type && native->getAddressSpace() == 0 && !native->isThreadLocal() &&
      layout.getABITypeAlign(type).value() == abi.word && alignment >= abi.word &&
      call->bundle()->data_layout().alloc_size_in_bytes(condition->type()->pointee()) == 48 &&
      call->bundle()->data_layout().store_size_in_bytes(condition->type()->pointee()) == 48 &&
      call->bundle()->data_layout().abi_alignment(condition->type()->pointee()) >= abi.word && type->hasName() && !type->isOpaque() &&
      type->getName() == "union.pthread_cond_t" && !native->isConstant() &&
      native->hasLocalLinkage() && native_mutex->hasLocalLinkage() &&
      !native->isExternallyInitialized() &&
      source->getArgOperand(0)->stripPointerCasts() == native &&
      source->getArgOperand(1)->stripPointerCasts() == native_mutex &&
      function->getFunctionType()->getParamType(0) == native->getType() &&
      function->getFunctionType()->getParamType(1) == native_mutex->getType() &&
      native->getParent()->getDataLayout().getTypeAllocSize(type) == 48;
}

// Check all AR aliases and runtime-storage consumers, including unvisited CFG arms.
inline bool pthread_initialization_storage_closed(
    ar::Bundle* bundle,
    std::map< ar::Value*, unsigned > objects,
    const std::unordered_set< ar::CallBase* >& covered,
    unsigned budget,
    const std::set< ar::GlobalVariable* >* mutex_arrays = nullptr,
    std::set< ar::CallBase* >* array_consumers = nullptr) {
  if (mutex_arrays)
    for (const auto& object : objects)
      if (object.second == 2 &&
          !mutex_arrays->count(dyn_cast< ar::GlobalVariable >(object.first))) return false;
  std::vector< ar::Statement* > statements;
  for (auto it = bundle->function_begin();
       it != bundle->function_end();
       ++it)
    if (auto* code = (*it)->body_or_null())
      for (auto* block : *code)
        for (auto* statement : *block) {
          if (statements.size() >= 8192)
            return false;
          statements.push_back(statement);
        }
  for (auto it = bundle->global_begin();
       it != bundle->global_end();
       ++it)
    if (auto* code = (*it)->initializer_or_null())
      for (auto* block : *code)
        for (auto* statement : *block) {
          if (statements.size() >= 8192)
            return false;
          statements.push_back(statement);
        }
  // Removing the global coverage gate also exposes other omitted runtime
  // writes. Seed their whole allocation (including locals/fields/scalar
  // typedefs), not just the objects initialized above. Callback/user args are
  // not storage.
  for (auto* statement : statements) {
    if (budget-- == 0)
      return false;
    auto* call = dyn_cast< ar::CallBase >(statement);
    if (!call)
      continue;
    unsigned runtime =
        synchronization_runtime_arguments(call,
                                          pthread_initialization_callee(call));
    for (std::size_t i = 0; i < call->num_arguments() && i < 4; ++i) {
      if (!(runtime & (1u << i)))
        continue;
      std::vector< ar::Value* > pending{call->argument(i)};
      std::unordered_set< ar::Value* > visited;
      bool has_root = false;
      while (!pending.empty()) {
        if (budget-- == 0)
          return false;
        auto* value = pending.back();
        pending.pop_back();
        if (!visited.insert(value).second)
          continue;
        if (auto* base = direct_byte_object(value)) {
          if (auto* global = dyn_cast< ar::GlobalVariable >(base))
            if (!global->is_definition())
              return false;
          if (!objects.count(base))
            objects[base] = 4;
          has_root = true;
          continue;
        }
        auto* variable = dyn_cast< ar::InternalVariable >(value);
        if (!variable || !variable->type()->is_pointer())
          return false;
        if (auto* function = variable->code()->function_or_null())
          for (auto it = function->param_begin(); it != function->param_end();
               ++it)
            if (*it == variable)
              return false;
        bool defined = false;
        for (auto* definition : statements) {
          if (definition->result_or_null() != variable)
            continue;
          defined = true;
          if (auto* copy = dyn_cast< ar::Assignment >(definition))
            pending.push_back(copy->operand());
          else if (auto* cast = dyn_cast< ar::UnaryOperation >(definition)) {
            if (cast->op() != ar::UnaryOperation::Bitcast)
              return false;
            pending.push_back(cast->operand());
          } else if (auto* shift = dyn_cast< ar::PointerShift >(definition))
            pending.push_back(shift->pointer());
          else
            return false;
        }
        if (!defined)
          return false;
      }
      if (!has_root)
        return false;
    }
  }
  // Forward may-alias closure also follows copies with multiple definitions.
  // This includes a conditional merge of two initialized mutex addresses.
  bool changed = true;
  while (changed) {
    changed = false;
    for (auto* statement : statements) {
      if (budget-- == 0)
        return false;
      ar::Value* source = nullptr;
      if (auto* copy = dyn_cast< ar::Assignment >(statement))
        source = copy->operand();
      else if (auto* cast = dyn_cast< ar::UnaryOperation >(statement)) {
        if (cast->op() == ar::UnaryOperation::Bitcast)
          source = cast->operand();
      } else if (auto* shift = dyn_cast< ar::PointerShift >(statement)) {
        source = shift->pointer();
        if (objects.count(source) && ((objects[source] & 1u) ||
            ((objects[source] & 2u) && !mutex_arrays)))
          for (auto it = shift->term_begin(); it != shift->term_end(); ++it) {
            auto* index = dyn_cast< ar::IntegerConstant >((*it).second);
            if (!index || !index->value().is_zero())
              return false;
          }
      }
      if (source && objects.count(source) && statement->result_or_null()) {
        auto* result = statement->result();
        if (!result->type()->is_pointer())
          return false;
        unsigned previous = objects[result];
        objects[result] |= objects[source];
        changed |= previous != objects[result];
      }
    }
  }
  auto operand_roles =
      [&](auto&& self, ar::Value* value, unsigned depth) -> unsigned {
    if (depth >= 32)
      return 7;
    if (objects.count(value))
      return objects[value];
    unsigned roles = 0;
    if (auto* aggregate = dyn_cast< ar::StructConstant >(value)) {
      for (const auto& field : aggregate->values())
        roles |= self(self, field.value, depth + 1);
    } else if (auto* aggregate = dyn_cast< ar::SequentialConstant >(value)) {
      for (auto* element : aggregate->values())
        roles |= self(self, element, depth + 1);
    }
    return roles;
  };
  PthreadInitializationFreshStorage fresh(bundle, statements, objects, budget);
  for (auto* statement : statements) {
    if (budget-- == 0)
      return false;
    // Unknown memory aliases can mutate or observe opaque storage without a
    // syntactic use of its address; do not certify that absence as
    // disjointness.
    if (auto* load = dyn_cast< ar::Load >(statement)) {
      auto* base = direct_byte_object(load->operand());
      if (load->result()->type()->is_pointer() || objects.count(base) ||
          (!base && !fresh.scalar_access(load))) return false;
      if (auto* global = dyn_cast_or_null< ar::GlobalVariable >(base))
        if (!global->is_definition())
          return false; // Hidden library-state writes have no AR body.
    } else if (auto* store = dyn_cast< ar::Store >(statement)) {
      auto* base = direct_byte_object(store->pointer());
      if (!base && !fresh.scalar_access(store)) return false;
      if (auto* global = dyn_cast_or_null< ar::GlobalVariable >(base))
        if (!global->is_definition())
          return false;
      if (objects.count(base) && store->code()->is_function_body())
        return false;
    } else if (auto* cast = dyn_cast< ar::UnaryOperation >(statement)) {
      if (cast->op() == ar::UnaryOperation::UIToPtr ||
          cast->op() == ar::UnaryOperation::SIToPtr)
        return false;
    }
    unsigned roles = 0;
    for (auto it = statement->op_begin(); it != statement->op_end(); ++it)
      roles |= operand_roles(operand_roles, *it, 0);
    if (auto* call = dyn_cast< ar::CallBase >(statement)) {
      if (covered.count(call))
        continue;
      bool sync = pthread_initialized_mutex_consumer(call);
      bool wait = pthread_initialized_condition_consumer(call);
      unsigned runtime =
          synchronization_runtime_arguments(call,
                                            pthread_initialization_callee(
                                                call));
      if (operand_roles(operand_roles, call->called(), 0))
        return false;
      for (std::size_t i = 0; i < call->num_arguments(); ++i) {
        auto* value = call->argument(i);
        unsigned role = operand_roles(operand_roles, value, 0);
        if (!role)
          continue;
        if (!objects.count(value))
          return false;
        if (role & 3u) {
          if (role != 2 || !((sync && i == 0) || (wait && i == 1)))
            return false;
          if (mutex_arrays && array_consumers) array_consumers->insert(call);
        } else if (role != 4 || i >= 4 || !(runtime & (1u << i)))
          return false;
      }
      for (std::size_t index = 0; index < call->num_arguments(); ++index) {
        auto* value = call->argument(index);
        if (!value->type()->is_pointer() || objects.count(value) ||
            isa< ar::NullConstant >(value) ||
            isa< ar::FunctionPointerConstant >(value))
          continue;
        auto* base = direct_byte_object(value);
        if (objects.count(base) || (!base && !fresh.create_argument(call, index)))
          return false;
      }
      continue;
    }
    if (!roles)
      continue;
    if (isa< ar::Assignment >(statement) ||
        isa< ar::UnaryOperation >(statement) ||
        isa< ar::PointerShift >(statement)) {
      if (statement->result_or_null() && objects.count(statement->result()))
        continue;
    }
    if (auto* store = dyn_cast< ar::Store >(statement)) {
      if (!store->code()->is_function_body() &&
          objects.count(store->pointer()) &&
          !operand_roles(operand_roles, store->value(), 0))
        continue;
    }
    return false;
  }
  return true;
}

// Glibc default flags + NORMAL/ERRORCHECK initialize successfully. Certify
// activation and all storage consumers before reusing the successful
// blocking-lock model. ponytail: whole globals only; locals/fields/helpers need
// object/lifetime proofs.
inline bool certified_pthread_initialization(ar::CallBase* query,
                                             Context& ctx) {
  if (!pthread_initialization_signature(query))
    return false;
  auto* main = query->bundle()->function_or_null("main");
  if (!main || !main->is_definition() ||
      query->bundle()->global_or_null("ar.global_ctors") ||
      !ctx.opts.no_init_globals.empty() ||
      !ctx.concurrent_env.function_executes_at_most_once(main))
    return false;
  for (auto* root : ctx.opts.entry_points)
    if (root != main)
      return false;
  auto* entry = main->body()->entry_block();
  if (entry->num_predecessors() != 0 || query->parent() != entry)
    return false;
  std::map< ar::GlobalVariable*, unsigned > attributes;
  std::map< ar::Value*, unsigned >
      objects; // attribute=1, initialized mutex=2, other runtime storage=4
  std::unordered_set< ar::CallBase* > covered;
  bool finished = false;
  unsigned budget = 8192;
  for (auto* statement : *entry) {
    if (budget-- == 0)
      return false;
    auto* call = dyn_cast< ar::CallBase >(statement);
    if (!call)
      continue;
    auto* function = pthread_initialization_callee(call);
    if (!pthread_initialization_signature(call)) {
      if (!function || function->intrinsic_id() != ar::Intrinsic::IkosNonDet ||
          call->num_arguments() != 0)
        finished = true;
      continue;
    }
    if (finished)
      return false;
    const auto& name = function->name();
    bool mutex = name == "pthread_mutex_init";
    auto* object = pthread_initialization_object(call->argument(0), mutex);
    if (!object)
      return false;
    if (mutex) {
      auto* attr = pthread_initialization_object(call->argument(1), false);
      if (!attr || attributes[attr] == 0 || !objects.emplace(object, 2).second)
        return false;
    } else if (name == "pthread_mutexattr_init") {
      if (!objects.emplace(object, 1).second)
        return false;
      attributes[object] = 1;
    } else if (name == "pthread_mutexattr_settype") {
      auto* kind = dyn_cast< ar::IntegerConstant >(call->argument(1));
      if (attributes[object] == 0 || !kind ||
          (kind->value().to_z_number() != 0 &&
           kind->value().to_z_number() != 2))
        return false;
      attributes[object] = kind->value().to_z_number() == 2 ? 2 : 1;
    } else {
      if (attributes[object] == 0)
        return false;
      attributes[object] = 0;
    }
    covered.insert(call);
  }
  if (!covered.count(query))
    return false;
  return pthread_initialization_storage_closed(
      query->bundle(), std::move(objects), covered, budget);
}

} // namespace analyzer
} // namespace ikos
