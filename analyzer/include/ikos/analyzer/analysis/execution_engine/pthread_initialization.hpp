// Copyright and license: see numerical.hpp.
#pragma once

#include <llvm/ADT/Triple.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

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
  std::vector< ar::Statement* > statements;
  for (auto it = query->bundle()->function_begin();
       it != query->bundle()->function_end();
       ++it)
    if (auto* code = (*it)->body_or_null())
      for (auto* block : *code)
        for (auto* statement : *block) {
          if (statements.size() >= 8192)
            return false;
          statements.push_back(statement);
        }
  for (auto it = query->bundle()->global_begin();
       it != query->bundle()->global_end();
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
        if (objects.count(source) && (objects[source] & 3u))
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
  for (auto* statement : statements) {
    if (budget-- == 0)
      return false;
    // Unknown memory aliases can mutate or observe opaque storage without a
    // syntactic use of its address; do not certify that absence as
    // disjointness.
    if (auto* load = dyn_cast< ar::Load >(statement)) {
      auto* base = direct_byte_object(load->operand());
      if (!base || objects.count(base) || load->result()->type()->is_pointer())
        return false;
      if (auto* global = dyn_cast< ar::GlobalVariable >(base))
        if (!global->is_definition())
          return false; // Hidden library-state writes have no AR body.
    } else if (auto* store = dyn_cast< ar::Store >(statement)) {
      auto* base = direct_byte_object(store->pointer());
      if (!base)
        return false;
      if (auto* global = dyn_cast< ar::GlobalVariable >(base))
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
          if (role != 2 || !sync || i != 0)
            return false;
        } else if (role != 4 || i >= 4 || !(runtime & (1u << i)))
          return false;
      }
      for (auto it = call->arg_begin(); it != call->arg_end(); ++it) {
        auto* value = *it;
        if (!value->type()->is_pointer() || objects.count(value) ||
            isa< ar::NullConstant >(value) ||
            isa< ar::FunctionPointerConstant >(value))
          continue;
        auto* base = direct_byte_object(value);
        if (!base || objects.count(base))
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

} // namespace analyzer
} // namespace ikos
