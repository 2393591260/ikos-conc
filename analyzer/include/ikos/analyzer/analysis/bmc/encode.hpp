// Candidate-prefix encoding: clocks, selected guards/addresses, spawn/join,
// mutex intervals and complete verifier atomic blocks. Confirmation also needs
// supported value provenance, object bounds and concrete thread activation.
// Missing read-from/value dependencies are rejected, not free SMT inputs.

#pragma once

#include <z3++.h>
#include <llvm/IR/Instructions.h>

#include <functional>
#include <chrono>
#include <map>
#include <set>
#include <vector>

#include "unroll.hpp"
#include "facts.hpp"
#include "initial.hpp"
#include "evidence.hpp"
#include "function_values.hpp"
#include "result.hpp"

namespace ikos {
namespace analyzer {
namespace bmc {

inline bool stream_output(const BmcEvent& event) {
  auto* call = core::dyn_cast< ar::CallBase >(event.stmt);
  return call != nullptr && constant_output_call(call);
}

inline bool atomic_query_barrier(const BmcEvent& event) {
  return event.kind != EvKind::Local && event.kind != EvKind::Assume &&
         !((event.kind == EvKind::Lock || event.kind == EvKind::Unlock) &&
           event.mutex == PSEUDO_ATOMIC_LOCK);
}

struct GuardRejection {
  unsigned path = 0; // 1 = A, 2 = B; these need the caller's read/write check.
  bool startup_infeasible = false; // Only the fixed main-before-workers prefix.
  std::set< ar::GlobalVariable* > reads;
};

/// Check the selected accesses, including any necessary atomic completion.
/// Failed outcomes never establish unbounded safety; SAT retains its evidence.
inline Result check_race(const std::vector< BmcEvent >& inputA,
                       const std::vector< BmcEvent >& inputB,
                       ar::Function* funcA, ar::Function* funcB,
                       Diagnostics* diagnostics = nullptr,
                       const CandidateFacts* facts = nullptr,
                       GuardRejection* rejection = nullptr, Metrics* metrics = nullptr,
                       const CompletionPlan* completion = nullptr,
                       const std::chrono::steady_clock::time_point* deadline = nullptr,
                       unsigned solver_timeout_ms = 1000) {
  if (metrics != nullptr) {
    ++metrics->path_queries;
    metrics->prefix_events += inputA.size() + inputB.size() + (facts != nullptr ? facts->startup.size() : 0);
    if (completion != nullptr)
      for (const auto& trace : completion->auxiliary) metrics->prefix_events += trace.events.size();
  }
  if (rejection != nullptr) *rejection = {};
  std::vector< ExecutionTrace > traces;
  traces.reserve(3 + (completion != nullptr ? completion->auxiliary.size() : 0));
  traces.push_back({"startup", funcA->bundle()->function_or_null("main"),
                    facts != nullptr ? facts->startup : std::vector< BmcEvent >{}});
  traces.push_back({"a", funcA, inputA});
  traces.push_back({"b", funcB, inputB});
  if (completion != nullptr)
    for (const auto& auxiliary : completion->auxiliary) traces.push_back(auxiliary);
  auto& startup = traces[0].events;
  auto& pathA = traces[1].events;
  auto& pathB = traces[2].events;
  if (pathA.empty() || pathB.empty()) {
    record(diagnostics, "empty_path");
    return Result(Status::Unsupported);
  }
  // Static initializers are not the pre-main state when constructors execute.
  // Until their actual prefixes are retained, neither values nor activation
  // derived from main alone establish a concrete execution.
  if (funcA->bundle()->global_or_null("ar.global_ctors") != nullptr) {
    record(diagnostics, "global_constructor_prefix");
    return Result(Status::Unsupported);
  }
  if (facts != nullptr && facts->single_execution) {
    for (auto& trace : traces) for (auto& event : trace.events) {
      auto* store = core::dyn_cast< ar::Store >(event.stmt);
      auto* function = store != nullptr ? store->code()->function_or_null() : nullptr;
      if (function != nullptr && function->name() != "main" && store->value()->type()->is_pointer()) {
        event.single_function_execution = facts->single_execution(function);
        if (event.single_function_execution) {
          record(diagnostics, "ai_single_execution", store);
          if (metrics != nullptr) ++metrics->ai_single_execution;
        }
      }
    }
  }
  ThreadArgument argumentA = created_argument(funcA, funcB->name() == "main" ? pathB : startup);
  ThreadArgument argumentB = created_argument(funcB, funcA->name() == "main" ? pathA : startup, funcA == funcB ? 1 : 0);
  const bool selected_workers = facts != nullptr && facts->selected_worker_occurrences;
  if (selected_workers &&
      (funcA->name() == "main" || funcB->name() == "main" ||
       (funcA == funcB && facts->worker_a_occurrence == facts->worker_b_occurrence) ||
       !bind_created_argument(funcA, startup, facts->worker_a_occurrence, argumentA) ||
       !bind_created_argument(funcB, startup, facts->worker_b_occurrence, argumentB))) {
    record(diagnostics, "selected_creation_missing");
    return Result(Status::Unsupported);
  }
  const std::size_t main_index = funcA->name() == "main" ? 1 : funcB->name() == "main" ? 2 : 0;
  if (completion != nullptr) {
    if (main_index == 0 || !startup.empty()) return Result(Status::Unsupported);
    auto& child = traces[main_index == 1 ? 2 : 1];
    child.creator_path = main_index;
    child.creator_step = completion->target_creation;
    if (!bind_creation_step(child.root, traces[main_index].events, child.creator_step, child.argument))
      return Result(Status::Unsupported);
    argumentA = traces[1].argument; argumentB = traces[2].argument;
  } else {
    traces[1].argument = argumentA; traces[2].argument = argumentB;
    for (std::size_t id = 1; id < 3; ++id) {
      auto& trace = traces[id];
      if (trace.root->name() == "main") continue;
      trace.creator_path = main_index;
      const auto ordinal = selected_workers ? (id == 1 ? facts->worker_a_occurrence : facts->worker_b_occurrence) :
                           (id == 2 && funcA == funcB ? 1 : 0);
      auto step = created_occurrence(trace.root, traces[main_index].events, ordinal);
      if (step < traces[main_index].events.size()) trace.creator_step = step;
    }
  }
  for (std::size_t id = 3; id < traces.size(); ++id) {
    auto& trace = traces[id];
    if (trace.creator_path != main_index || trace.events.empty() ||
        !bind_creation_step(trace.root, traces[main_index].events, trace.creator_step, trace.argument))
      return Result(Status::Unsupported);
  }
  const auto& allocation_origin = traces[main_index].events;
  using ObjectId = std::pair< std::uint64_t, core::ZNumber >;
  std::map< ObjectId, unsigned > attributes;
  std::vector< BmcEvent > initialized_mutexes;
  std::vector< const std::vector< BmcEvent >* > lifecycle_paths;
  for (const auto& trace : traces) lifecycle_paths.push_back(&trace.events);
  bool has_attributes = false;
  for (const auto* path : lifecycle_paths)
    for (const auto& event : *path)
      if (auto* call = core::dyn_cast< ar::CallBase >(event.stmt))
        if (auto* function = exact_callee(call))
          has_attributes |= function->name() == "pthread_mutexattr_init" ||
                            function->name() == "pthread_mutexattr_settype" ||
                            function->name() == "pthread_mutexattr_destroy" ||
                            (function->name() == "pthread_mutex_init" && call->num_arguments() == 2 &&
                             !core::isa< ar::NullConstant >(call->argument(1)));
  auto attribute_lifecycle = [&](const std::vector< BmcEvent >& path, ar::Function* root, ThreadArgument root_argument) {
    if (!has_attributes) return true;
    bool spawned = false;
    std::vector< BmcEvent > used_mutexes;
    auto object = [&](ar::CallBase* call, unsigned argument, std::size_t before,
                      bool mutex, BmcEvent& address, bool symbolic = false) {
      core::ZNumber extent;
      if (!pointer_on_path(call->argument(argument), path, before, root, root_argument, address, &allocation_origin) ||
          address.base == 0 || (address.var_id != 0 && !symbolic) || !memory_extent(address, extent)) return false;
      auto* global = reinterpret_cast< ar::GlobalVariable* >(address.base);
      if (!global->has_frontend() || global->is_thread_local()) return false;
      auto* source = global->frontend< llvm::GlobalVariable >();
      const llvm::Triple target(source->getParent()->getTargetTriple());
      const auto bits = call->bundle()->data_layout().pointers.bit_width;
      // Verified glibc x86 ABI: ERRORCHECK=2, attr=4, mutex=24/40 bytes.
      if (!target.isOSLinux() || target.getEnvironment() != llvm::Triple::GNU ||
          !((target.getArch() == llvm::Triple::x86 && bits == 32) ||
            (target.getArch() == llvm::Triple::x86_64 && bits == 64))) return false;
      const unsigned width = mutex ? (bits == 32 ? 24 : 40) : 4;
      const unsigned alignment = mutex ? bits / 8 : 4;
      const auto& layout = call->bundle()->data_layout();
      std::uint64_t source_width, source_alignment;
      if (!synchronization_layout(call, argument, source_width, source_alignment) ||
          source_width != width || source_alignment != alignment) return false;
      address.width = width;
      if (address.var_id != 0) {
        // An in-bounds aligned element of this homogeneous array is a mutex.
        // Its actual index and unlock ownership still need the SMT constraints.
        auto* array = llvm::dyn_cast< llvm::ArrayType >(source->getValueType());
        auto* element = array != nullptr ? llvm::dyn_cast< llvm::StructType >(array->getElementType()) : nullptr;
        return mutex && element != nullptr && !element->isOpaque() && element->hasName() &&
               element->getName() == "union.pthread_mutex_t" &&
               source->getParent()->getDataLayout().getTypeAllocSize(element) == width &&
               address.const_off == 0 && address.coeff == width &&
               layout.abi_alignment(global->type()->pointee()) >= alignment;
      }
      if (address.const_off < 0 || address.const_off + width > extent) return false;
      // Find the actual declared library subobject, including array/struct
      // fields. A cast into another control object cannot establish its type.
      auto* selected = source->getValueType();
      auto offset = address.const_off.to< std::uint64_t >();
      const auto& native = source->getParent()->getDataLayout();
      bool member = false;
      for (unsigned depth = 0; depth < 32; ++depth) {
        if (auto* structure = llvm::dyn_cast< llvm::StructType >(selected)) {
          if (structure->isOpaque()) return false;
          if (structure->hasName() && structure->getName() ==
              (mutex ? "union.pthread_mutex_t" : "union.pthread_mutexattr_t")) {
            member = offset == 0 && native.getTypeAllocSize(structure) == width;
            break;
          }
          const auto* fields = native.getStructLayout(structure);
          bool found = false;
          for (unsigned j = 0; j < structure->getNumElements(); ++j) {
            auto start = fields->getElementOffset(j);
            auto* field = structure->getElementType(j);
            if (offset >= start && offset - start < native.getTypeAllocSize(field)) {
              offset -= start; selected = field; found = true; break;
            }
          }
          if (!found) return false;
        } else if (auto* array = llvm::dyn_cast< llvm::ArrayType >(selected)) {
          auto stride = native.getTypeAllocSize(array->getElementType());
          if (stride == 0 || offset / stride >= array->getNumElements()) return false;
          offset %= stride; selected = array->getElementType();
        } else return false;
      }
      return member &&
             layout.abi_alignment(global->type()->pointee()) >= alignment &&
             address.const_off >= 0 && address.const_off + width <= extent &&
             address.const_off % alignment == 0;
    };
    for (std::size_t i = 0; i < path.size(); ++i) {
      spawned |= path[i].kind == EvKind::Create;
      auto* call = core::dyn_cast< ar::CallBase >(path[i].stmt);
      auto* function = call != nullptr ? exact_callee(call) : nullptr;
      if (function == nullptr) continue;
      if (has_attributes && (path[i].kind == EvKind::Lock || path[i].kind == EvKind::Unlock) &&
          !is_verifier_atomic(call)) {
        BmcEvent address;
        if (call->num_arguments() != 1 || !object(call, 0, i, true, address, true)) return false;
        used_mutexes.push_back(address);
      }
      const auto& name = function->name();
      const bool init_mutex = name == "pthread_mutex_init";
      if (!init_mutex && name != "pthread_mutexattr_init" && name != "pthread_mutexattr_settype" &&
          name != "pthread_mutexattr_destroy") continue;
      if (!has_attributes && init_mutex && call->num_arguments() == 2 &&
          core::isa< ar::NullConstant >(call->argument(1))) continue;
      if (root == nullptr || root->name() != "main" || spawned || !mutex_attribute_call(call)) return false;
      BmcEvent address;
      if (!object(call, 0, i, init_mutex, address)) return false;
      const ObjectId identity{address.base, address.const_off};
      if (init_mutex) {
        for (const auto& previous : used_mutexes)
          if (previous.base == address.base &&
              (previous.var_id != 0 || (previous.const_off < address.const_off + address.width &&
               address.const_off < previous.const_off + previous.width))) return false;
        if (!core::isa< ar::NullConstant >(call->argument(1))) {
          BmcEvent attr;
          if (!object(call, 1, i, false, attr)) return false;
          if (attr.base == address.base && attr.const_off < address.const_off + address.width &&
              address.const_off < attr.const_off + attr.width) return false;
          auto value = attributes.find({attr.base, attr.const_off});
          if (value == attributes.end() || value->second != 2) return false;
        }
        for (const auto& previous : initialized_mutexes)
          if (previous.base == address.base && previous.const_off < address.const_off + address.width &&
              address.const_off < previous.const_off + previous.width) return false;
        initialized_mutexes.push_back(address);
      } else if (name == "pthread_mutexattr_init") {
        if (!attributes.emplace(identity, 0).second) return false;
      } else {
        auto value = attributes.find(identity);
        if (value == attributes.end()) return false;
        if (name == "pthread_mutexattr_destroy") attributes.erase(value);
        else {
          core::ZNumber kind;
          if (!constant_integer_on_path(call->argument(1), path, i, kind) || kind != 2) return false;
          value->second = 2;
        }
      }
    }
    return true;
  };
  for (const auto& trace : traces)
    if (!attribute_lifecycle(trace.events, trace.root, trace.argument)) {
      record(diagnostics, "mutex_attribute_lifecycle"); return Result(Status::Unsupported);
    }
  using ConditionId = std::pair< std::uint64_t, core::ZNumber >;
  std::map< ConditionId, std::uint64_t > conditions;
  std::map< ConditionId, std::pair< const void*, std::size_t > > condition_origins;
  auto condition_address = [&](ar::CallBase* call, const auto& path, std::size_t before,
                               ar::Function* root, ThreadArgument argument, BmcEvent& address) {
    auto* type = core::dyn_cast< ar::PointerType >(call->argument(0)->type());
    core::ZNumber size;
    if (type == nullptr || type->pointee()->is_opaque() ||
        !pointer_on_path(call->argument(0), path, before, root, argument, address,
                         selected_workers ? &allocation_origin : nullptr) ||
        address.base == 0 || address.var_id != 0 || !memory_extent(address, size)) return false;
    address.width = call->bundle()->data_layout().store_size_in_bytes(type->pointee()).to< std::uint64_t >();
    auto alignment = call->bundle()->data_layout().abi_alignment(type->pointee());
    auto* global = reinterpret_cast< ar::GlobalVariable* >(address.base);
    return address.width != 0 && address.const_off >= 0 && address.const_off + address.width <= size &&
           address.const_off % alignment == 0 &&
           call->bundle()->data_layout().abi_alignment(global->type()->pointee()) >= alignment;
  };
  auto condition_initializers = [&](const auto& path, ar::Function* root, ThreadArgument argument) {
    bool spawned = false;
    for (std::size_t i = 0; i < path.size(); ++i) {
      spawned |= path[i].kind == EvKind::Create;
      auto* call = core::dyn_cast< ar::CallBase >(path[i].stmt);
      if (call == nullptr || !condition_call(call) || exact_callee(call)->name() != "pthread_cond_init") continue;
      BmcEvent address;
      // Initialize once in main before children exist. Concurrent initialization
      // and implicit/static initializer recognition need separate lifetime evidence.
      if (root == nullptr || root->name() != "main" || spawned ||
          !condition_address(call, path, i, root, argument, address)) return false;
      for (const auto& previous : conditions)
        if (previous.first.first == address.base &&
            previous.first.second < address.const_off + address.width &&
            address.const_off < previous.first.second + previous.second) return false;
      ConditionId identity{address.base, address.const_off};
      conditions.emplace(identity, address.width);
      condition_origins.emplace(identity, std::make_pair(static_cast< const void* >(&path), i));
    }
    return true;
  };
  auto* main = funcA->bundle()->function_or_null("main");
  for (const auto& trace : traces)
    if (!condition_initializers(trace.events, trace.root, trace.argument)) {
      record(diagnostics, "condition_initialization"); return Result(Status::Unsupported);
    }
  std::map< ConditionId, ConditionId > waiting_mutexes;
  auto condition_uses = [&](const auto& path, ar::Function* root, ThreadArgument argument, bool is_startup) {
    for (std::size_t i = 0; i < path.size(); ++i) {
      auto* call = core::dyn_cast< ar::CallBase >(path[i].stmt);
      if (call == nullptr || !condition_call(call)) continue;
      const auto& name = exact_callee(call)->name();
      BmcEvent address;
      if (!condition_address(call, path, i, root, argument, address)) return false;
      auto initialized = conditions.find({address.base, address.const_off});
      if (initialized == conditions.end() && static_condition(address)) {
        const ConditionId identity{address.base, address.const_off};
        initialized = conditions.emplace(identity, address.width).first;
        condition_origins.emplace(identity, std::make_pair(nullptr, 0));
      }
      if (initialized == conditions.end() || initialized->second != address.width ||
          (is_startup && name == "pthread_cond_wait")) return false;
      const auto& origin = condition_origins.at(initialized->first);
      if (origin.first == &path && i < origin.second) return false;
      if (name == "pthread_cond_wait") {
        BmcEvent mutex;
        if (!pointer_on_path(call->argument(1), path, i, root, argument, mutex,
                             selected_workers ? &allocation_origin : nullptr) ||
            mutex.base == 0 || mutex.var_id != 0) return false;
        ConditionId identity{mutex.base, mutex.const_off};
        auto binding = waiting_mutexes.emplace(ConditionId{address.base, address.const_off}, identity);
        // Concurrent waiters with different mutexes violate POSIX's dynamic binding.
        if (binding.first->second != identity) return false;
      }
    }
    return true;
  };
  for (std::size_t id = 0; id < traces.size(); ++id)
    if (!condition_uses(traces[id].events, traces[id].root, traces[id].argument, id == 0)) {
      record(diagnostics, "condition_object"); return Result(Status::Unsupported);
    }
  for (std::size_t i = 0; !conditions.empty() && i < startup.size(); ++i) {
    const auto& event = startup[i];
    if (event.kind != EvKind::Read && event.kind != EvKind::Write) continue;
    auto* pointer = event.kind == EvKind::Read ? core::cast< ar::Load >(event.stmt)->operand()
                                             : core::cast< ar::Store >(event.stmt)->pointer();
    BmcEvent address;
    if (!pointer_on_path(pointer, startup, i, main, nullptr, address)) return Result(Status::Unsupported);
    for (const auto& condition : conditions)
      if (address.base == condition.first.first &&
          (address.var_id != 0 || event.width == 0 ||
           (address.const_off < condition.first.second + condition.second &&
            condition.first.second < address.const_off + event.width))) {
        record(diagnostics, "condition_storage_alias", event.stmt); return Result(Status::Unsupported);
      }
  }
  std::map< std::pair< std::uint64_t, core::ZNumber >, std::uint64_t > lock_ids;
  auto prepare = [&](std::vector< BmcEvent >& path, ar::Function* root, ThreadArgument argument) {
    std::set< std::uint64_t > held;
    std::set< std::size_t > consumed_handles;
    std::uint64_t symbolic_lock = 0;
    for (std::size_t i = 0; i < path.size(); ++i) {
      auto* wait = core::dyn_cast< ar::CallBase >(path[i].stmt);
      if (!path[i].synthetic_condition && wait != nullptr && condition_call(wait) &&
          exact_callee(wait)->name() == "pthread_cond_wait") {
        // POSIX permits a spurious wake: choose a finite successful wait, with
        // the owned mutex released and reacquired, without inventing signal HB.
        BmcEvent release(wait, EvKind::Unlock), reacquire(wait, EvKind::Lock);
        release.synthetic_condition = reacquire.synthetic_condition = true;
        path.insert(path.begin() + i + 1, {release, reacquire});
      }
      auto& event = path[i];
      if (event.kind == EvKind::Join && completion == nullptr) {
        // Condition expansion may have shifted earlier creation indices.
        event.join_completion = inert_join_source(core::cast< ar::CallBase >(event.stmt),
                                                  path, i, event.join_creation);
      }
      if (event.kind == EvKind::Join && event.join_completion != nullptr &&
          !consumed_handles.insert(event.join_creation).second) {
        record(diagnostics, "thread_handle_consumed", event.stmt); return false;
      }
      auto* handle_call = core::dyn_cast< ar::CallBase >(event.stmt);
      auto* handle_api = handle_call != nullptr ? exact_callee(handle_call) : nullptr;
      if (handle_api != nullptr && handle_api->name() == "pthread_detach") {
        std::size_t creation = 0;
        if (!supported_leaf_call(handle_call) ||
            !handle_creation_on_path(handle_call, path, i, creation) ||
            !consumed_handles.insert(creation).second) {
          record(diagnostics, "detach_handle_unproven", event.stmt); return false;
        }
      }
      ar::Value* pointer = nullptr;
      if (auto* load = core::dyn_cast< ar::Load >(event.stmt)) pointer = load->operand();
      if (auto* store = core::dyn_cast< ar::Store >(event.stmt)) pointer = store->pointer();
      if (pointer != nullptr) {
        event.base = 0; event.local_base = nullptr; event.shared_main_local = false; event.const_off = 0; event.var_id = 0; event.coeff = 0;
        event.heap_allocation = nullptr; event.heap_extent = 0;
        if (!pointer_on_path(pointer, path, i, root, argument, event, &allocation_origin)) {record(diagnostics, "prefix_address", event.stmt); return false;}
      }
      if ((event.kind == EvKind::Lock || event.kind == EvKind::Unlock) &&
          !event.synthetic_atomic &&
          !is_verifier_atomic(core::cast< ar::CallBase >(event.stmt))) {
        auto* call = core::cast< ar::CallBase >(event.stmt);
        BmcEvent address;
        core::ZNumber size;
        auto argument_index = event.synthetic_condition ? 1u : 0u;
        if (call->num_arguments() != (event.synthetic_condition ? 2u : 1u) ||
            !pointer_on_path(call->argument(argument_index), path, i, root, argument, address,
                             selected_workers ? &allocation_origin : nullptr) ||
            address.base == 0 || !memory_extent(address, size)) {
          record(diagnostics, "mutex_identity", event.stmt); return false;
        }
        event.base = address.base; event.const_off = address.const_off;
        event.var_id = address.var_id; event.var_before = address.var_before; event.coeff = address.coeff;
        std::uint64_t alignment;
        if (!synchronization_layout(call, argument_index, event.width, alignment)) {
          record(diagnostics, "mutex_storage_layout", event.stmt); return false;
        }
        if (completion != nullptr) {
          const auto bits = call->bundle()->data_layout().pointers.bit_width;
          auto* api = exact_callee(call);
          const bool rwlock = api != nullptr && api->name().find("pthread_rwlock_") == 0;
          const auto expected = rwlock ? (bits == 32 ? 32u : 56u) : (bits == 32 ? 24u : 40u);
          if (event.width != expected || alignment != bits / 8) {
            record(diagnostics, "completion_mutex_abi", event.stmt); return false;
          }
        }
        if (event.synthetic_condition && event.var_id != 0) return false;
        if (event.var_id == 0 && (event.const_off < 0 || event.const_off + event.width > size)) return false;
        auto id = lock_ids.emplace(std::make_pair(address.base, address.const_off), lock_ids.size() + 1);
        if (event.kind == EvKind::Lock) {
          if (symbolic_lock != 0 || (event.var_id != 0 && !held.empty())) {
            record(diagnostics, "symbolic_mutex_nested", event.stmt); return false;
          }
          event.mutex = event.var_id == 0 ? id.first->second : (~std::uint64_t(0) - i);
          if (event.var_id != 0) symbolic_lock = event.mutex;
          held.insert(event.mutex);
        } else {
          if (symbolic_lock != 0) event.mutex = symbolic_lock;
          else if (event.var_id != 0 && held.size() == 1) event.mutex = *held.begin();
          else if (event.var_id != 0) {record(diagnostics, "mutex_ownership", event.stmt); return false;}
          else event.mutex = id.first->second;
          held.erase(event.mutex);
          symbolic_lock = 0;
        }
      }
      // The library's control storage is opaque here, not ordinary SC data.
      // Explicit reads/writes aliasing it cannot be silently omitted from rf.
      if (event.kind == EvKind::Read || event.kind == EvKind::Write)
        for (const auto& condition : conditions)
          if (event.base == condition.first.first &&
              (event.var_id != 0 || event.width == 0 ||
               (event.const_off < condition.first.second + condition.second &&
                condition.first.second < event.const_off + event.width))) {
            record(diagnostics, "condition_storage_alias", event.stmt); return false;
          }
    }
    return true;
  };
  if (completion != nullptr) {
    for (const auto& joined : completion->joins) {
      if (joined.first >= traces[main_index].events.size() || joined.second >= traces.size() ||
          joined.second == main_index) return Result(Status::Unsupported);
      auto& join = traces[main_index].events[joined.first];
      const auto& child = traces[joined.second];
      auto* ret = core::dyn_cast< ar::ReturnValue >(child.events.back().stmt);
      if (join.kind != EvKind::Join || ret == nullptr || ret->code() != child.root->body_or_null() ||
          child.creator_path != main_index) return Result(Status::Unsupported);
      join.join_completion = ret; join.join_creation = child.creator_step;
    }
  }
  for (auto& trace : traces)
    if (!prepare(trace.events, trace.root, trace.argument)) return Result(Status::Unsupported);
  // Expansion in the old search can shift a creation; recover its ordinal.
  if (completion == nullptr)
    for (std::size_t id = 1; id < 3; ++id) {
      auto& trace = traces[id];
      if (trace.root->name() == "main" || trace.creator_step == ExecutionTrace::no_creation) continue;
      const auto ordinal = selected_workers ? (id == 1 ? facts->worker_a_occurrence : facts->worker_b_occurrence) :
                           (id == 2 && funcA == funcB ? 1 : 0);
      trace.creator_step = created_occurrence(trace.root, traces[trace.creator_path].events, ordinal);
    }
  // Only actually accessed child objects use shared SC sources. Main's
  // private handle storage keeps its existing modeled-create definedness.
  std::unordered_set< ar::LocalVariable* > shared_stack;
  for (const auto& trace : traces) {
    if (trace.root == nullptr || trace.root->name() == "main") continue;
    for (const auto& event : trace.events)
      if (event.shared_main_local) shared_stack.insert(event.local_base);
  }
  for (auto& trace : traces)
    for (auto& event : trace.events)
      if (event.shared_main_local && !shared_stack.count(event.local_base))
        event.shared_main_local = false;
  for (std::size_t id = 0; id < traces.size(); ++id)
    if (!prefix_values_defined(traces[id].events, diagnostics, traces[id].root, traces[id].argument,
          id == 0 ? nullptr : &allocation_origin, id == 0 && selected_workers, completion != nullptr))
      return Result(Status::Unsupported);

  // Calls update handles and opaque synchronization storage without AR Stores.
  // Preserve their footprints so static initialization cannot masquerade as rf.
  struct LibraryStorage {
    BmcEvent address;
    const void* path;
    bool opaque;
    std::size_t occurrence;
    std::uint64_t alignment;
    bool deferred_create;
  };
  std::vector< LibraryStorage > library_storage;
  auto library_outputs = [&](std::vector< BmcEvent >& path, ar::Function* root, ThreadArgument argument) {
    for (std::size_t i = 0; i < path.size(); ++i) {
      const auto& event = path[i];
      if (event.synthetic_atomic || event.synthetic_condition) continue;
      auto* call = core::dyn_cast< ar::CallBase >(event.stmt);
      auto* function = call != nullptr ? exact_callee(call) : nullptr;
      if (function == nullptr || !function->is_declaration()) continue;
      const auto& name = function->name();
      std::vector< unsigned > arguments;
      const bool opaque = ((event.kind == EvKind::Lock || event.kind == EvKind::Unlock) &&
                           !is_verifier_atomic(call)) || mutex_attribute_call(call) || condition_call(call);
      if (opaque || event.kind == EvKind::Create) arguments.push_back(0);
      if (name == "pthread_cond_wait") arguments.push_back(1);
      if (event.kind == EvKind::Join && call->num_arguments() == 2 &&
          !core::isa< ar::NullConstant >(call->argument(1))) arguments.push_back(1);
      for (auto index : arguments) {
        BmcEvent address;
        std::uint64_t alignment = 0;
        auto* type = core::dyn_cast< ar::PointerType >(call->argument(index)->type());
        if (type == nullptr || type->pointee()->is_opaque() ||
            !pointer_on_path(call->argument(index), path, i, root, argument, address, &allocation_origin)) {
          record(diagnostics, "library_storage_identity", call); return false;
        }
        if (opaque) {
          if (!synchronization_layout(call, index, address.width, alignment)) {
            record(diagnostics, "library_storage_layout", call); return false;
          }
        } else {
          address.width = call->bundle()->data_layout().store_size_in_bytes(type->pointee()).to< std::uint64_t >();
        }
        const bool deferred_create = event.kind == EvKind::Create && address.var_id != 0;
        if (deferred_create) {
          core::ZNumber extent;
          if (!selected_workers || &path != &startup ||
              !symbolic_startup_create_output(path, i, path[i], extent, alignment)) {
            record(diagnostics, "startup_create_output", call); return false;
          }
          address = path[i];
        } else if ((selected_workers || completion != nullptr || address.heap_allocation != nullptr) &&
                   event.kind == EvKind::Create) {
          // Preserve the concrete output also when a loop index folded early.
          auto& output = path[i];
          output.base = address.base; output.local_base = address.local_base;
          output.shared_main_local = address.shared_main_local;
          output.heap_allocation = address.heap_allocation; output.heap_extent = address.heap_extent;
          output.const_off = address.const_off; output.var_id = address.var_id;
          output.var_before = address.var_before; output.coeff = address.coeff; output.width = address.width;
        }
        library_storage.push_back({address, &path, opaque, i, alignment, deferred_create});
      }
    }
    return true;
  };
  for (auto& trace : traces)
    if (!library_outputs(trace.events, trace.root, trace.argument)) return Result(Status::Unsupported);
  // A fixed startup may publish live allocations to its selected children.
  // Interleaved main/free retains the narrower private-handle contract.
  auto private_heap_lifetime = [&](std::vector< BmcEvent >& path) {
    bool finite = std::any_of(path.begin(), path.end(),
                              [](const auto& e) {return e.finite_source;});
    bool has_free = std::any_of(path.begin(), path.end(), [](const auto& e) {
      auto* call = core::dyn_cast< ar::CallBase >(e.stmt);
      return call != nullptr && free_call(call);
    });
    const bool joined_heap = completion != nullptr && std::any_of(path.begin(), path.end(),
        [](const auto& e) {return e.joined_handle_read && e.heap_allocation != nullptr;});
    if (!finite && !has_free && !joined_heap) return true;
    const bool shared_startup = selected_workers && &path == &startup && finite;
    if (shared_startup && has_free) return false;
    const bool private_heap = finite || has_free || joined_heap;
    std::set< ar::CallBase* > live, freed;
    unsigned allocations = 0;
    for (std::size_t i = 0; i < path.size(); ++i) {
      auto& event = path[i];
      if (event.heap_allocation != nullptr && freed.count(event.heap_allocation)) return false;
      auto* call = core::dyn_cast< ar::CallBase >(event.stmt);
      if (call != nullptr && malloc_call(call)) {
        core::ZNumber extent;
        if (!main_allocation(call, path, i, extent) || ++allocations > (shared_startup ? 4u : 1u)) return false;
        live.insert(call);
      }
      if (call != nullptr && free_call(call)) {
        BmcEvent pointer;
        if (call->num_arguments() != 1 ||
            !pointer_on_path(call->argument(0), path, i, path[i].stmt->code()->function_or_null(),
                             nullptr, pointer) ||
            pointer.heap_allocation == nullptr || pointer.var_id != 0 ||
            pointer.const_off != 0 || !live.erase(pointer.heap_allocation) ||
            !freed.insert(pointer.heap_allocation).second) return false;
        event.heap_allocation = pointer.heap_allocation;
        event.heap_extent = pointer.heap_extent;
        event.const_off = 0;
        event.width = 0;
      }
      if (call != nullptr && private_heap) {
        for (std::size_t arg = 0; arg < call->num_arguments(); ++arg) {
          auto* value = call->argument(arg);
          if (!value->type()->is_pointer() || core::isa< ar::NullConstant >(value) ||
              core::isa< ar::FunctionPointerConstant >(value)) continue;
          BmcEvent pointer;
          if (!pointer_on_path(value, path, i, path[i].stmt->code()->function_or_null(),
                               nullptr, pointer)) {
            // Numeric arguments retain the existing native-width round-trip
            // contract; they never grant object-pointer access.
            auto* variable = core::dyn_cast< ar::InternalVariable >(value);
            std::size_t definition = 0;
            auto* cast = variable != nullptr ? core::dyn_cast_or_null< ar::UnaryOperation >(
                reaching_definition(variable, path, i, definition)) : nullptr;
            auto* integer = cast != nullptr ? core::dyn_cast< ar::IntegerType >(cast->operand()->type()) : nullptr;
            if (!shared_startup || event.kind != EvKind::Create || arg != 3 ||
                cast == nullptr || (cast->op() != ar::UnaryOperation::SIToPtr &&
                                    cast->op() != ar::UnaryOperation::UIToPtr) || integer == nullptr ||
                integer->bit_width() != call->bundle()->data_layout().pointers.bit_width) return false;
            continue;
          }
          if (pointer.heap_allocation != nullptr &&
              ((!live.count(pointer.heap_allocation) &&
                !(free_call(call) && arg == 0 && freed.count(pointer.heap_allocation))) ||
               !(((event.kind == EvKind::Create || free_call(call)) && arg == 0) ||
                 (shared_startup && event.kind == EvKind::Create && arg == 3)))) return false;
        }
      }
      if (private_heap) {
        auto* cast = core::dyn_cast< ar::UnaryOperation >(event.stmt);
        if (cast != nullptr && cast->operand()->type()->is_pointer() &&
            !core::isa< ar::NullConstant >(cast->operand()) &&
            !core::isa< ar::FunctionPointerConstant >(cast->operand())) {
          BmcEvent pointer;
          if (!pointer_on_path(cast->operand(), path, i, path[i].stmt->code()->function_or_null(),
                               nullptr, pointer)) return false;
          if (pointer.heap_allocation != nullptr &&
              (freed.count(pointer.heap_allocation) || cast->op() == ar::UnaryOperation::PtrToUI ||
               cast->op() == ar::UnaryOperation::PtrToSI)) return false;
        }
        auto* store = core::dyn_cast< ar::Store >(event.stmt);
        if (store != nullptr && store->value()->type()->is_pointer() &&
            !core::isa< ar::NullConstant >(store->value()) &&
            !core::isa< ar::FunctionPointerConstant >(store->value())) {
          BmcEvent pointer;
          if (!pointer_on_path(store->value(), path, i, path[i].stmt->code()->function_or_null(),
                               nullptr, pointer)) return false;
          if (pointer.heap_allocation != nullptr) {
            auto* slot = global_pointer_slot(store->pointer());
            std::size_t origin = 0;
            if (!shared_startup || !live.count(pointer.heap_allocation) || slot == nullptr ||
                published_pointer(slot, path, origin) != store || origin != i) return false;
          }
        }
        auto* shift = core::dyn_cast< ar::PointerShift >(event.stmt);
        if (shift != nullptr && shift->result()->type()->is_pointer()) {
          BmcEvent base, pointer;
          if (!pointer_on_path(shift->pointer(), path, i, path[i].stmt->code()->function_or_null(),
                               nullptr, base)) return false;
          if (base.heap_allocation != nullptr) {
            auto* native = shift->has_frontend()
                ? llvm::dyn_cast< llvm::GetElementPtrInst >(shift->frontend< llvm::Value >()) : nullptr;
            if (!pointer_on_path(shift->result(), path, i + 1, path[i].stmt->code()->function_or_null(),
                                 nullptr, pointer) ||
                native == nullptr || !native->isInBounds() || pointer.var_id != 0 ||
                pointer.const_off < 0 || pointer.const_off > pointer.heap_extent ||
                freed.count(pointer.heap_allocation)) return false;
          }
        }
      }
    }
    return !private_heap || (shared_startup ? allocations > 0 : allocations == 1);
  };
  for (auto& trace : traces)
    if (trace.root != nullptr && trace.root->name() == "main" &&
        !private_heap_lifetime(trace.events)) {
      record(diagnostics, "heap_lifetime"); return Result(Status::Unsupported);
    }
  auto aliases_library = [&](const BmcEvent& access, const void* path, bool opaque_only) {
    for (const auto& storage : library_storage) {
      if (opaque_only && !storage.opaque) continue;
      const auto& output = storage.address;
      const bool same = access.base != 0 ? access.base == output.base :
          access.heap_allocation != nullptr ? access.heap_allocation == output.heap_allocation :
          access.local_base != nullptr && (path == storage.path ||
              (access.shared_main_local && output.shared_main_local)) && access.local_base == output.local_base;
      // Fields in repeated records can be disjoint for every index. A common
      // stride plus nonwrapping, disjoint residues proves byte nonaliasing.
      core::ZNumber stride = access.var_id != 0 ? (access.coeff < 0 ? -access.coeff : access.coeff) : core::ZNumber(0);
      const auto output_stride = output.coeff < 0 ? -output.coeff : output.coeff;
      const bool common_stride = output.var_id == 0 || stride == 0 || stride == output_stride;
      if (output.var_id != 0 && stride == 0) stride = output_stride;
      if (same && common_stride && stride > 0 && access.width != 0 && output.width != 0) {
        auto read_field = access.const_off % stride, output_field = output.const_off % stride;
        if (read_field < 0) read_field += stride;
        if (output_field < 0) output_field += stride;
        if (read_field + access.width <= stride && output_field + output.width <= stride &&
            (read_field + access.width <= output_field || output_field + output.width <= read_field)) continue;
      }
      if (same && (access.var_id != 0 || output.var_id != 0 || access.width == 0 || output.width == 0 ||
          (access.const_off < output.const_off + output.width && output.const_off < access.const_off + access.width)))
        return true;
    }
    return false;
  };
  const auto& data_paths = lifecycle_paths;
  for (const auto* path : data_paths)
    for (const auto& event : *path)
      if ((event.kind == EvKind::Read || event.kind == EvKind::Write) && aliases_library(event, path, true)) {
        record(diagnostics, "opaque_library_storage", event.stmt); return Result(Status::Unsupported);
      }

  z3::context ctx;
  z3::solver s(ctx);

  const std::size_t nA = pathA.size();
  const std::size_t nB = pathB.size();
  const std::size_t targetA = candidate_index(pathA), targetB = candidate_index(pathB);
  if (targetA == nA || targetB == nB) {record(diagnostics, "candidate_missing"); return Result(Status::Unsupported);}

  std::vector< z3::expr > clock;
  for (std::size_t id = 1; id < traces.size(); ++id) {
    auto& trace = traces[id]; trace.clock_start = clock.size();
    for (std::size_t i = 0; i < trace.events.size(); ++i) {
      clock.push_back(ctx.int_const((trace.id + std::to_string(i)).c_str()));
      if (i != 0) s.add(clock.back() > clock[clock.size() - 2]);
    }
  }
  // SC memory and indivisible stdout calls have a common strict order. Ties
  // must not invent distinct latest writes or omit the stream synchronization.
  z3::expr_vector memory_clocks(ctx);
  for (std::size_t id = 1; id < traces.size(); ++id) {
    const auto& trace = traces[id];
    for (std::size_t i = 0; i < trace.events.size(); ++i)
      if (trace.events[i].kind == EvKind::Read || trace.events[i].kind == EvKind::Write || stream_output(trace.events[i]))
        memory_clocks.push_back(clock[trace.clock_start + i]);
  }
  if (memory_clocks.size() > 1) s.add(z3::distinct(memory_clocks));

  if (completion != nullptr) {
    std::set< std::pair< std::size_t, std::size_t > > bindings;
    for (std::size_t id = 1; id < traces.size(); ++id) {
      const auto& child = traces[id];
      if (id == main_index) continue;
      if (!bindings.emplace(child.creator_path, child.creator_step).second ||
          child.creator_step >= traces[child.creator_path].events.size()) return Result(Status::Unsupported);
      s.add(clock[traces[child.creator_path].clock_start + child.creator_step] < clock[child.clock_start]);
    }
    for (const auto& joined : completion->joins) {
      const auto& child = traces[joined.second];
      s.add(clock[child.clock_start + child.events.size() - 1] < clock[traces[main_index].clock_start + joined.first]);
    }
  } else {
    // Preserve the original fast search's conservative activation order.
    for (std::size_t id = 1; id < 3; ++id) {
      const auto& parent = traces[id]; const auto& child = traces[id == 1 ? 2 : 1];
      for (std::size_t i = 0; i < parent.events.size(); ++i)
        if (parent.events[i].kind == EvKind::Create &&
            parent.events[i].thread == reinterpret_cast< std::uint64_t >(child.root))
          s.add(clock[parent.clock_start + i] < clock[child.clock_start]);
    }
  }
  for (const auto& trace : traces)
    for (const auto& event : trace.events)
      if (event.kind == EvKind::Join && event.join_completion == nullptr) {
        record(diagnostics, "join_completion_unproven", event.stmt);
        return Result(Status::Unsupported);
      }

  // Keep every critical section. Last-lock maps discard earlier exclusion and
  // permit infeasible prefixes. Unsupported ownership/recursion stays UNKNOWN.
  struct Section {std::uint64_t mutex; std::size_t begin; std::size_t end; bool reader;};
  auto sections = [&](const std::vector< BmcEvent >& path, std::vector< Section >& out) {
    std::map< std::uint64_t, std::size_t > active;
    for (std::size_t i = 0; i < path.size(); ++i) {
      const auto& e = path[i];
      if (e.kind == EvKind::Unsupported) {
        record(diagnostics, e.blocker, e.stmt);
        return false;
      }
      if (e.kind != EvKind::Lock && e.kind != EvKind::Unlock) continue;
      if (e.mutex == 0) {record(diagnostics, "mutex_identity", e.stmt); return false;}
      if (e.kind == EvKind::Lock) {
        if (active.count(e.mutex)) {record(diagnostics, "mutex_relock", e.stmt); return false;}
        active[e.mutex] = i;
      } else {
        auto held = active.find(e.mutex);
        if (held == active.end()) {record(diagnostics, "mutex_ownership", e.stmt); return false;}
        out.push_back({e.mutex, held->second, i, path[held->second].reader});
        active.erase(held);
      }
    }
    if (active.count(PSEUDO_ATOMIC_LOCK)) {
      record(diagnostics, "atomic_prefix_open", path.back().stmt);
      return false;
    }
    for (const auto& entry : active) out.push_back({entry.first, entry.second, path.size(), path[entry.second].reader});
    return true;
  };
  std::vector< std::vector< Section > > trace_sections(traces.size());
  for (std::size_t id = 1; id < traces.size(); ++id)
    if (!sections(traces[id].events, trace_sections[id])) return Result(Status::Unsupported);
  const auto& sections_a = trace_sections[1]; const auto& sections_b = trace_sections[2];
  auto atomic_target = [](const std::vector< Section >& sections, std::size_t target) {
    return std::any_of(sections.begin(), sections.end(), [&](const Section& section) {
      return section.mutex == PSEUDO_ATOMIC_LOCK && section.begin < target && target < section.end;
    });
  };
  const bool atomicA = atomic_target(sections_a, targetA), atomicB = atomic_target(sections_b, targetB);
  if (atomicA && atomicB) {record(diagnostics, "atomic_pair"); return Result(Status::BoundedUnsat);}
  auto interior = [](const auto& sections, const auto& path, std::size_t target) {
    for (const auto& section : sections) {
      if (section.mutex != PSEUDO_ATOMIC_LOCK || !(section.begin < target && target < section.end)) continue;
      bool before = false, after = false;
      for (std::size_t i = section.begin + 1; i < section.end; ++i) {
        if (!atomic_query_barrier(path[i])) continue;
        before |= i < target;
        after |= target < i;
      }
      if (before && after) return true;
    }
    return false;
  };
  // The other target is outside this indivisible interval. A mandatory barrier
  // on each side of this target prevents both adjacent orientations in our query.
  if (interior(sections_a, pathA, targetA) || interior(sections_b, pathB, targetB)) {
    record(diagnostics, "atomic_interior_unsat"); return Result(Status::BoundedUnsat);
  }
  FunctionValues functions(ctx, s, funcA->bundle(), traces, clock,
      [&](const BmcEvent& access, const void* path) {return aliases_library(access, path, false);});
  for (unsigned id = 0; id < traces.size(); ++id) {
    const auto& path = traces[id].events;
    for (std::size_t i = 0; i < path.size(); ++i) {
      if (path[i].synthetic_atomic || path[i].synthetic_condition) continue;
      auto* call = core::dyn_cast< ar::CallBase >(path[i].stmt);
      if (call == nullptr || exact_callee(call) != nullptr || path[i].called_function == nullptr) continue;
      if (!functions.bind(call, path[i].called_function, id, i)) {
        record(diagnostics, "function_pointer_source", call); return Result(Status::Unsupported);
      }
      record(diagnostics, "function_pointer_read_from", call);
    }
  }
  // Only source nondet is free. Other demanded values need an executed dynamic
  // definition, exact machine arithmetic, actual arguments/returns or SC rf.
  // Unsupported and uninitialized origins never become fresh SMT inputs.
  bool unencodable = false;

  // 每路径独立的 cache 与符号前缀：同一线程入口函数的两个实例，其局部变量共享同一
  // InternalVariable 指针，但不同实例互不相关——共用同一 SMT 符号会错误相关两实例
  // 的独立值（FN）。
  using ValueKey = std::pair< ar::InternalVariable*, std::size_t >;
  using ValueCache = std::map< ValueKey, z3::expr >;
  std::vector< ValueCache > caches(traces.size());
  auto& cache_init = caches[0]; auto& cache_a = caches[1]; auto& cache_b = caches[2];
  auto trace_index = [&](const ValueCache& cache) {
    for (std::size_t id = 0; id < caches.size(); ++id) if (&caches[id] == &cache) return id;
    ikos_unreachable("unknown BMC value cache");
  };
  bool guard_reusable = false;
  std::set< ar::GlobalVariable* > guard_reads;
  std::map< const BmcEvent*, z3::expr > replay_offsets;
  std::function<
      bool(ar::Value*, z3::expr&, ValueCache&, const std::string&, unsigned, std::size_t) >
      encode_value;
  encode_value = [&](ar::Value* v,
                     z3::expr& out,
                     ValueCache& cache,
                     const std::string& prefix,
                     unsigned depth, std::size_t before) -> bool {
    if (depth >= 64) {
      record(diagnostics, "value_depth");
      return false;
    }
    if (auto* c = core::dyn_cast< ar::IntegerConstant >(v)) {
      out = ctx.int_val(c->value().to_z_number().str().c_str());
      return true;
    }
    auto* iv = core::dyn_cast< ar::InternalVariable >(v);
    if (iv == nullptr) {
      record(diagnostics, "value_non_integer");
      return false;
    }
    std::size_t definition_index = 0;
    const auto id = trace_index(cache);
    const auto& path = traces[id].events;
    ar::Statement* def = reaching_definition(iv, path, before, definition_index);
    std::size_t call_index = 0;
    auto* actual = def == nullptr ? formal_argument(iv, path, before, call_index) : nullptr;
    if (actual != nullptr) {
      guard_reusable = false;
      if (actual->type() != iv->type() || !encode_value(actual, out, cache, prefix, depth + 1, call_index)) return false;
      return true;
    }
    if (def == nullptr) {
      record(diagnostics, "value_no_definition");
      return false;
    }
    ValueKey key{iv, definition_index};
    auto cached = cache.find(key);
    if (cached != cache.end()) {out = cached->second; return true;}
    if (auto* as = core::dyn_cast_or_null< ar::Assignment >(def)) {
      if (as->operand()->type() != iv->type() ||
          !encode_value(as->operand(), out, cache, prefix, depth + 1, definition_index)) {
        return false;
      }
      cache.emplace(key, out);
      return true;
    }
    if (auto* un = core::dyn_cast_or_null< ar::UnaryOperation >(def)) {
      auto* src = core::dyn_cast< ar::IntegerType >(un->operand()->type());
      auto* dst = core::dyn_cast< ar::IntegerType >(iv->type());
      if (src == nullptr || dst == nullptr) {
        // Only a native-width integer round-trip has this bit-pattern identity.
        // Capture its executed definition before the actual creation occurrence.
        if (dst != nullptr &&
            (un->op() == ar::UnaryOperation::PtrToSI ||
             un->op() == ar::UnaryOperation::PtrToUI)) {
          const auto& trace = traces[id];
          if (trace.root != nullptr && trace.root->num_parameters() == 1 &&
              trace.root->param(0) == un->operand() &&
              trace.argument.value != nullptr &&
              trace.creator_path < caches.size() &&
              trace.creator_step < traces[trace.creator_path].events.size()) {
            guard_reusable = false;
            auto* piv = core::dyn_cast< ar::InternalVariable >(trace.argument.value);
            std::size_t argument_step = 0;
            const auto& creator = traces[trace.creator_path];
            ar::Statement* pdef = piv != nullptr
                ? reaching_definition(piv, creator.events, trace.creator_step, argument_step)
                : nullptr;
            auto* inner = core::dyn_cast_or_null< ar::UnaryOperation >(pdef);
            auto* inner_src = inner != nullptr
                ? core::dyn_cast< ar::IntegerType >(inner->operand()->type())
                : nullptr;
            if (inner != nullptr &&
                (inner->op() == ar::UnaryOperation::SIToPtr ||
                 inner->op() == ar::UnaryOperation::UIToPtr) &&
                inner_src != nullptr && inner_src->bit_width() == dst->bit_width() &&
                dst->bit_width() == un->bundle()->data_layout().pointers.bit_width &&
                encode_value(inner->operand(), out, caches[trace.creator_path],
                             traces[trace.creator_path].value_prefix(), depth + 1,
                             argument_step)) {
              if (inner_src->is_signed() != dst->is_signed()) {
                z3::expr bits = z3::int2bv(dst->bit_width(), out);
                out = z3::bv2int(bits, dst->is_signed());
              }
              cache.emplace(key, out);
              return true;
            }
          }
        }
        record(diagnostics, "value_non_integer_cast", def);
        return false;
      }
      bool extension = dst->bit_width() > src->bit_width() &&
                       (un->op() == ar::UnaryOperation::SExt || un->op() == ar::UnaryOperation::ZExt);
      bool bitcast = un->op() == ar::UnaryOperation::Bitcast &&
                     src->bit_width() == dst->bit_width();
      bool truncation = dst->bit_width() < src->bit_width() &&
                        (un->op() == ar::UnaryOperation::UTrunc || un->op() == ar::UnaryOperation::STrunc);
      if (!extension && !bitcast && !truncation) {
        record(diagnostics, "value_cast", def);
        return false;
      }
      if (!encode_value(un->operand(), out, cache, prefix, depth + 1, definition_index)) {
        return false;
      }
      if ((bitcast && src->is_signed() == dst->is_signed()) ||
          (extension && un->op() == ar::UnaryOperation::SExt && src->is_signed() && dst->is_signed()) ||
          (extension && un->op() == ar::UnaryOperation::ZExt && !src->is_signed())) {
        cache.emplace(key, out);
        return true;
      }
      z3::expr bits = z3::int2bv(src->bit_width(), out);
      if (extension) bits = un->op() == ar::UnaryOperation::SExt
                                ? z3::sext(bits, dst->bit_width() - src->bit_width())
                                : z3::zext(bits, dst->bit_width() - src->bit_width());
      if (truncation) bits = bits.extract(dst->bit_width() - 1, 0);
      out = z3::bv2int(bits, dst->is_signed());
      cache.emplace(key, out);
      return true;
    }
    if (auto* binary = core::dyn_cast_or_null< ar::BinaryOperation >(def)) {
      auto* type = core::dyn_cast< ar::IntegerType >(iv->type());
      if (type == nullptr || binary->left()->type() != iv->type() || binary->right()->type() != iv->type()) return false;
      z3::expr left(ctx), right(ctx);
      if (!encode_value(binary->left(), left, cache, prefix, depth + 1, definition_index) ||
          !encode_value(binary->right(), right, cache, prefix, depth + 1, definition_index)) return false;
      unsigned width = type->bit_width();
      z3::expr l = z3::int2bv(width, left), r = z3::int2bv(width, right), result(ctx);
      z3::expr lo = ctx.int_val((type->is_signed() ? -(ZNumber(1) << (width - 1)) : ZNumber(0)).str().c_str());
      z3::expr hi = ctx.int_val(((ZNumber(1) << (type->is_signed() ? width - 1 : width)) - 1).str().c_str());
      z3::expr arithmetic(ctx);
      switch (binary->op()) {
        case ar::BinaryOperation::SAdd: case ar::BinaryOperation::UAdd: arithmetic = left + right; result = l + r; break;
        case ar::BinaryOperation::SSub: case ar::BinaryOperation::USub: arithmetic = left - right; result = l - r; break;
        case ar::BinaryOperation::SMul: case ar::BinaryOperation::UMul: arithmetic = left * right; result = l * r; break;
        case ar::BinaryOperation::SAnd: case ar::BinaryOperation::UAnd: result = l & r; break;
        case ar::BinaryOperation::SOr: case ar::BinaryOperation::UOr: result = l | r; break;
        case ar::BinaryOperation::SXor: case ar::BinaryOperation::UXor: result = l ^ r; break;
        case ar::BinaryOperation::SDiv: case ar::BinaryOperation::UDiv:
        case ar::BinaryOperation::SRem: case ar::BinaryOperation::URem: {
          s.add(right != 0);
          bool signed_op = binary->op() == ar::BinaryOperation::SDiv || binary->op() == ar::BinaryOperation::SRem;
          if (signed_op) s.add(!(left == lo && right == -1));
          if (binary->op() == ar::BinaryOperation::SDiv) result = l / r;
          else if (binary->op() == ar::BinaryOperation::UDiv) result = z3::udiv(l, r);
          else if (binary->op() == ar::BinaryOperation::SRem) result = z3::srem(l, r);
          else result = z3::urem(l, r);
          if (binary->is_exact()) s.add(result * r == l);
          break;
        }
        case ar::BinaryOperation::SShl: case ar::BinaryOperation::UShl:
        case ar::BinaryOperation::SLShr: case ar::BinaryOperation::ULShr:
        case ar::BinaryOperation::SAShr: case ar::BinaryOperation::UAShr: {
          s.add(right >= 0 && right < ctx.int_val(width));
          if (binary->op() == ar::BinaryOperation::SShl || binary->op() == ar::BinaryOperation::UShl) {
            result = z3::shl(l, r);
            if (binary->op() == ar::BinaryOperation::SShl) {
              // C signed left shift needs a nonnegative, representable source
              // execution even when LLVM did not attach a no-wrap flag.
              z3::expr wide = z3::shl(z3::zext(l, width), z3::zext(r, width));
              s.add(left >= 0 && z3::bv2int(wide, false) <= hi);
            }
            if (binary->has_no_wrap()) {
              z3::expr wide = z3::shl(type->is_signed() ? z3::sext(l, width) : z3::zext(l, width), z3::zext(r, width));
              s.add(z3::bv2int(wide, type->is_signed()) == z3::bv2int(result, type->is_signed()));
            }
          } else {
            result = binary->op() == ar::BinaryOperation::SAShr || binary->op() == ar::BinaryOperation::UAShr
                         ? z3::ashr(l, r) : z3::lshr(l, r);
            if (binary->is_exact()) s.add(z3::shl(result, r) == l);
          }
          break;
        }
        default: record(diagnostics, "value_arithmetic", def); return false;
      }
      if (static_cast< bool >(arithmetic) && binary->has_no_wrap()) {
        s.add(arithmetic >= lo && arithmetic <= hi);
        // Within the AR no-wrap range, mathematical and machine results are
        // identical. Avoid an int/BV round trip in every SC counter update.
        out = arithmetic;
      } else out = z3::bv2int(result, type->is_signed());
      cache.emplace(key, out);
      record(diagnostics, "integer_operation", def);
      return true;
    }
    if (auto* load = core::dyn_cast_or_null< ar::Load >(def)) {
      auto* global = core::dyn_cast< ar::GlobalVariable >(load->operand());
      if (global == nullptr) guard_reusable = false;
      else guard_reads.insert(global);
      auto* type = core::dyn_cast< ar::IntegerType >(iv->type());
      const auto& access = path[definition_index];
      if (type == nullptr || abstract_initializer(def)) return false;
      if ((access.heap_allocation != nullptr || access.shared_main_local) &&
          (access.var_id != 0 || access.width == 0 || access.width > 8 ||
           type->bit_width() != access.width * 8 ||
           access.const_off % load->bundle()->data_layout().abi_alignment(type) != 0)) return false;
      auto same_object = [&](const BmcEvent& write) {
        if (access.heap_allocation != nullptr) return write.heap_allocation == access.heap_allocation;
        if (access.base != 0) return write.base == access.base;
        return access.local_base != nullptr && write.local_base == access.local_base &&
               (!access.shared_main_local || write.shared_main_local);
      };
      if (aliases_library(access, &path, false)) {
        record(diagnostics, "library_read_origin", load); return false;
      }
      // Cache the read before following writes: legal dependencies may be
      // syntactically cyclic, but strict SC source order rules out rf cycles.
      out = ctx.int_const((prefix + "read_" + std::to_string(definition_index)).c_str());
      cache.emplace(key, out);
      auto convert = [&](const z3::expr& v) {return z3::bv2int(z3::int2bv(type->bit_width(), v), type->is_signed()).simplify();};
      auto address_offset = [&](const BmcEvent& event, z3::expr& offset,
                                ValueCache& values, const std::string& label) {
        offset = ctx.int_val(event.const_off.str().c_str());
        if (event.var_id == 0) return true;
        z3::expr index(ctx);
        if (!encode_value(reinterpret_cast< ar::InternalVariable* >(event.var_id), index,
                          values, label, depth + 1, event.var_before)) return false;
        offset = offset + ctx.int_val(event.coeff.str().c_str()) * index;
        return true;
      };
      z3::expr read_offset(ctx);
      if (!address_offset(access, read_offset, cache, prefix)) return false;
      // Only primitive, exactly addressed globals enter byte-level SC rf.
      // Keep the existing word encoding for uniform accesses and symbolic fields.
      auto* scalar = access.base != 0 ? core::dyn_cast< ar::IntegerType >(
          reinterpret_cast< ar::GlobalVariable* >(access.base)->type()->pointee()) : nullptr;
      bool mixed = false;
      if (scalar != nullptr && access.var_id == 0 && access.width != 0 &&
          type->bit_width() == access.width * 8 && scalar->bit_width() % 8 == 0 && scalar->bit_width() <= 64) {
        mixed = type->bit_width() != scalar->bit_width() || access.const_off != 0;
        for (const auto* events : data_paths)
          for (const auto& event : *events)
            if (event.kind == EvKind::Write && event.base == access.base &&
                (event.var_id != 0 || (event.const_off < access.const_off + access.width &&
                 access.const_off < event.const_off + event.width)))
              mixed |= event.width != access.width || event.const_off != access.const_off;
      }
      if (mixed) {
        if (type->bit_width() > 64 || access.const_off < 0 ||
            access.const_off + access.width > scalar->bit_width() / 8) return false;
        auto* object = reinterpret_cast< ar::GlobalVariable* >(access.base);
        const bool little = object->bundle()->data_layout().is_little_endian();
        auto byte_of = [&](const z3::expr& value, unsigned bits, unsigned index) {
          const unsigned shift = 8 * (little ? index : bits / 8 - 1 - index);
          return z3::int2bv(bits, value).extract(shift + 7, shift).simplify();
        };
        ZNumber initial_value;
        const bool initialized = initial_integer(object, ZNumber(0), scalar->bit_width(), initial_value);
        std::vector< z3::expr > bytes;
        std::vector< bool > byte_known;
        for (unsigned k = 0; k < access.width; ++k) {
          bytes.push_back(initialized ? byte_of(ctx.int_val(initial_value.str().c_str()), scalar->bit_width(),
                                              access.const_off.to< unsigned >() + k) : ctx.bv_val(0, 8));
          byte_known.push_back(initialized);
        }
        struct ByteWrite {z3::expr time; z3::expr value;};
        std::vector< std::vector< ByteWrite > > byte_writes(access.width);
        auto collect_bytes = [&](const std::vector< BmcEvent >& events, ValueCache& values,
                                 const std::string& label, std::size_t clock_start, bool startup_source) {
          const std::size_t end = startup_source && prefix == "value_init_" ? definition_index : events.size();
          for (std::size_t j = 0; j < end; ++j) {
            const auto& event = events[j];
            if (event.kind != EvKind::Write || event.base != access.base || abstract_initializer(event.stmt) ||
                (!startup_source && &events == &path && j >= definition_index)) continue;
            if (event.var_id != 0) return false;
            if (event.const_off >= access.const_off + access.width || access.const_off >= event.const_off + event.width) continue;
            auto* value = core::cast< ar::Store >(event.stmt)->value();
            auto* stored_type = core::dyn_cast< ar::IntegerType >(value->type());
            z3::expr encoded(ctx);
            if (stored_type == nullptr || event.width == 0 || event.width > 8 ||
                stored_type->bit_width() != event.width * 8 || event.const_off < 0 ||
                event.const_off + event.width > scalar->bit_width() / 8 ||
                !encode_value(value, encoded, values, label, depth + 1, j)) return false;
            for (unsigned k = 0; k < access.width; ++k) {
              auto index = access.const_off + k - event.const_off;
              if (index < 0 || index >= event.width) continue;
              auto value_byte = byte_of(encoded, stored_type->bit_width(), index.to< unsigned >());
              if (startup_source) {bytes[k] = value_byte; byte_known[k] = true;}
              else byte_writes[k].push_back({clock[clock_start + j], value_byte});
            }
          }
          return true;
        };
        if (!collect_bytes(startup, cache_init, "value_init_", 0, true)) return false;
        if (id != 0)
          for (std::size_t k = 1; k < traces.size(); ++k)
            if (!collect_bytes(traces[k].events, caches[k], traces[k].value_prefix(), traces[k].clock_start, false)) return false;
        if (prefix == "value_init_") {
          for (bool known : byte_known) if (!known) return false;
        } else {
          const auto read_time = clock[traces[id].clock_start + definition_index];
          for (unsigned k = 0; k < access.width; ++k) {
            const auto& writes = byte_writes[k];
            auto byte = ctx.bv_const((prefix + "byte_" + std::to_string(definition_index) + "_" + std::to_string(k)).c_str(), 8);
            auto choice = ctx.bool_val(false), first = ctx.bool_val(true);
            for (std::size_t j = 0; j < writes.size(); ++j) {
              auto latest = writes[j].time < read_time;
              first = first && read_time < writes[j].time;
              for (std::size_t l = 0; l < writes.size(); ++l)
                if (l != j) latest = latest && !(writes[j].time < writes[l].time && writes[l].time < read_time);
              choice = choice || (latest && byte == writes[j].value);
            }
            if (byte_known[k]) choice = choice || (first && byte == bytes[k]);
            if (writes.empty()) {
              if (!byte_known[k]) return false;
            } else {s.add(choice); bytes[k] = byte;}
          }
        }
        // All constituent bytes use the same load clock; a word load is one
        // event, not independent byte reads that could invent a torn value.
        auto assembled = bytes[little ? access.width - 1 : 0];
        for (unsigned k = 1; k < access.width; ++k)
          assembled = z3::concat(assembled, bytes[little ? access.width - 1 - k : k]);
        auto source = z3::bv2int(assembled, type->is_signed()).simplify();
        s.add(out == source);
        out = source; cache.find(key)->second = out;
        record(diagnostics, "sc_byte_read_from", load);
        return true;
      }
      z3::expr initial(ctx);
      bool known = false;
      if (access.base != 0) {
        ZNumber bits;
        auto* global = reinterpret_cast< ar::GlobalVariable* >(access.base);
        if (access.var_id == 0) {
          known = initial_integer(global, access.const_off, type->bit_width(), bits);
          if (known) initial = convert(ctx.int_val(bits.str().c_str()));
        } else {
          // Enumerate actual typed initializer fields, including a[i].field.
          // Padding/undef and large objects remain unsupported initial sources.
          ZNumber size, stride = access.coeff < 0 ? -access.coeff : access.coeff;
          if (stride != 0 && memory_extent(access, size) && size / stride <= 256) {
            ZNumber offset = access.const_off % stride;
            if (offset < 0) offset += stride;
            bool complete = true, first = true;
            for (; offset + access.width <= size; offset += stride) {
              if (!initial_integer(global, offset, type->bit_width(), bits)) {complete = false; break;}
              auto value = convert(ctx.int_val(bits.str().c_str()));
              initial = first ? value : z3::ite(read_offset == ctx.int_val(offset.str().c_str()), value, initial).simplify();
              first = false;
            }
            known = complete && !first;
          }
        }
      }
      if (access.base != 0 || access.heap_allocation != nullptr || access.shared_main_local) {
        // malloc has no initialized source. Fold only actual stores to this
        // allocation, never stores to another heap object or a null local base.
        // The retained startup executes before either selected child. Fold
        // actual main writes once, sharing its nondet symbols across reads.
        {
          const auto end = prefix == "value_init_" ? definition_index : startup.size();
          for (std::size_t j = 0; j < end; ++j) {
            const auto& e = startup[j];
            if (e.kind != EvKind::Write || !same_object(e)) continue;
            if (e.var_id == 0 && access.var_id == 0 &&
                (e.const_off >= access.const_off + access.width || access.const_off >= e.const_off + e.width)) continue;
            if (e.width != access.width) return false;
            z3::expr write_offset(ctx);
            if (!address_offset(e, write_offset, cache_init, "value_init_")) return false;
            s.add(write_offset == read_offset || write_offset + ctx.int_val(e.width) <= read_offset ||
                  read_offset + ctx.int_val(access.width) <= write_offset);
            auto* value = core::cast< ar::Store >(e.stmt)->value();
            auto* stored_type = core::dyn_cast< ar::IntegerType >(value->type());
            z3::expr stored(ctx);
            if (stored_type == nullptr || stored_type->bit_width() != type->bit_width() ||
                !encode_value(value, stored, cache_init, "value_init_", depth + 1, j)) return false;
            if (stored_type->is_signed() != type->is_signed()) stored = convert(stored);
            if (e.var_id == 0 && access.var_id == 0 && e.const_off == access.const_off) {
              initial = stored; known = true;
            } else {
              if (!known) return false;
              initial = z3::ite(write_offset == read_offset, stored, initial);
            }
          }
        }
      }
      std::vector< z3::expr > sources;
      struct Write {z3::expr time; z3::expr value; z3::expr matches;};
      std::vector< Write > writes;
      bool ordered_sources = completion == nullptr && (access.base != 0 || access.heap_allocation != nullptr || access.shared_main_local) && access.var_id == 0;
      const auto& other_path = prefix == "value_a_" ? pathB : pathA;
      auto* root = prefix == "value_a_" ? funcA : funcB;
      auto* other_root = prefix == "value_a_" ? funcB : funcA;
      std::size_t creator = 0;
      if (root->name() != "main" && other_root->name() == "main")
        for (std::size_t j = 0; j < other_path.size(); ++j)
          if (other_path[j].kind == EvKind::Create &&
              other_path[j].thread == reinterpret_cast< std::uint64_t >(root)) {
            creator = j;
            break;
          }
      z3::expr last_local(ctx), last_parent(ctx);
      auto collect = [&](const std::vector< BmcEvent >& events, ValueCache& values,
                         const std::string& label, std::size_t clock_start) {
        std::size_t begin = 0;
        if (access.local_base != nullptr && !access.shared_main_local) {
          if (&events != &path) return true;
          for (std::size_t j = 0; j < definition_index; ++j)
            if (events[j].stmt->result_or_null() == access.local_base) begin = j + 1;
        }
        for (std::size_t j = begin; j < events.size(); ++j) {
          const auto& e = events[j];
          if (e.kind != EvKind::Write || abstract_initializer(e.stmt) || (&events == &path && j >= definition_index)) continue;
          if (!same_object(e)) continue;
          if (e.var_id == 0 && access.var_id == 0 &&
              (e.const_off >= access.const_off + access.width || access.const_off >= e.const_off + e.width)) continue;
          if (e.width != access.width) return false;
          z3::expr write_offset(ctx);
          if (!address_offset(e, write_offset, values, label)) return false;
          s.add(write_offset == read_offset || write_offset + ctx.int_val(e.width) <= read_offset ||
                read_offset + ctx.int_val(access.width) <= write_offset);
          auto* value = core::cast< ar::Store >(e.stmt)->value();
          auto* stored_type = core::dyn_cast< ar::IntegerType >(value->type());
          z3::expr encoded(ctx);
          if (stored_type == nullptr || stored_type->bit_width() != type->bit_width() ||
              !encode_value(value, encoded, values, label, depth + 1, j)) return false;
          auto stored_value = stored_type->is_signed() == type->is_signed() ? encoded : convert(encoded);
          const bool local = &events == &path;
          const bool before_spawn = &events == &other_path && j < creator;
          ordered_sources &= (local || before_spawn) && e.var_id == 0 && e.const_off == access.const_off;
          if (local) last_local = stored_value;
          else if (before_spawn) last_parent = stored_value;
          writes.push_back({clock[clock_start + j], stored_value, write_offset == read_offset});
        }
        return true;
      };
      if (prefix == "value_init_") {
        // All selected children may remain unscheduled during this main
        // prefix. A read sees AR initialization or its last preceding write,
        // never a later startup store or an arbitrary fresh value.
        if (!known) {record(diagnostics, "startup_read_origin", load); return false;}
        s.add(out == initial);
        // Substitute the same proved preceding value so impossible startup
        // guards fail before retrying every worker-instance combination.
        out = initial.simplify();
        cache.find(key)->second = out;
        record(diagnostics, "startup_read_from", load);
        return true;
      }
      for (std::size_t k = 1; k < traces.size(); ++k)
        if (!collect(traces[k].events, caches[k], traces[k].value_prefix(), traces[k].clock_start)) return false;
      // po and create->child-start order pre-spawn parent writes before all
      // child writes. Retain collect's checks; post-spawn/symbolic writers
      // still need general SC rf. This is not a program-privacy proof.
      if (ordered_sources && (!writes.empty() || known)) {
        auto source = (static_cast< bool >(last_local) ? last_local :
                       static_cast< bool >(last_parent) ? last_parent : initial).simplify();
        s.add(out == source);
        out = source;
        cache.find(key)->second = out;
        record(diagnostics, static_cast< bool >(last_parent) ? "sc_spawn_read_from" : "sc_same_path_read_from", load);
        return true;
      }
      z3::expr read_time = clock[traces[id].clock_start + definition_index];
      for (std::size_t j = 0; j < writes.size(); ++j) {
        z3::expr latest = writes[j].matches && writes[j].time < read_time;
        for (std::size_t k = 0; k < writes.size(); ++k)
          if (k != j) latest = latest && !(writes[k].matches && writes[j].time < writes[k].time && writes[k].time < read_time);
        sources.push_back(latest && out == writes[j].value);
      }
      if (known) {
        z3::expr first = ctx.bool_val(true);
        for (const auto& write : writes) first = first && (!write.matches || read_time < write.time);
        sources.push_back(first && out == initial);
      }
      if (sources.empty()) return false;
      z3::expr choice = ctx.bool_val(false);
      for (const auto& source : sources) choice = choice || source;
      s.add(choice);
      record(diagnostics, "sc_read_from", load);
      return true;
    }
    if (auto* call = core::dyn_cast_or_null< ar::CallBase >(def)) {
      if (successful_create_result(path[definition_index]) || successful_condition_result(path[definition_index]) || successful_join_result(path[definition_index])) {
        out = ctx.int_val(0);
        cache.emplace(key, out);
        return true;
      }
      if (ffs_call(call)) {
        z3::expr value(ctx);
        if (!encode_value(call->argument(0), value, cache, prefix, depth + 1,
                          definition_index)) return false;
        auto bits = z3::int2bv(32, value);
        out = ctx.int_val(0);
        for (unsigned bit = 32; bit-- > 0;)
          out = z3::ite(bits.extract(bit, bit) == ctx.bv_val(1, 1),
                        ctx.int_val(bit + 1), out);
        cache.emplace(key, out);
        return true;
      }
      // 识别 nondet 调用：直调是
      // FunctionPointerConstant；`__VERIFIER_nondet_int` 常被 frontend 变成 `%5
      // = bitcast @__VERIFIER_nondet_int; %6 = call %5()`， 剥 bitcast
      // 拿底层函数（sound：bitcast 不改函数身份）。
      ar::Value* called = call->called();
      bool is_nondet = false;
      bool is_bool = false;
      for (int depth = 0; depth < 4 && !is_nondet; ++depth) {
        if (auto* fpc = core::dyn_cast< ar::FunctionPointerConstant >(called)) {
          ar::Function* f = fpc->function();
          if (f != nullptr) {
            static const std::set< std::string >
                integer_sources{"__VERIFIER_nondet_bool",
                                "__VERIFIER_nondet_char",
                                "__VERIFIER_nondet_uchar",
                                "__VERIFIER_nondet_short",
                                "__VERIFIER_nondet_ushort",
                                "__VERIFIER_nondet_int",
                                "__VERIFIER_nondet_uint",
                                "__VERIFIER_nondet_long",
                                "__VERIFIER_nondet_ulong",
                                "__VERIFIER_nondet_longlong",
                                "__VERIFIER_nondet_ulonglong",
                                "__VERIFIER_nondet_size_t"};
            auto* returned =
                core::dyn_cast< ar::IntegerType >(f->type()->return_type());
            auto* result = core::dyn_cast< ar::IntegerType >(iv->type());
            // C's old-style f() declarations may be variadic in AR; check the
            // call.
            is_nondet = f->num_parameters() == 0 &&
                        call->num_arguments() == 0 && returned != nullptr &&
                        result != nullptr &&
                        returned->bit_width() == result->bit_width() &&
                        returned->is_signed() == result->is_signed() &&
                        (f->intrinsic_id() == ar::Intrinsic::IkosNonDet ||
                         (f->is_declaration() &&
                          integer_sources.count(f->name()) != 0));
            is_bool = f->name() == "__VERIFIER_nondet_bool";
          }
          break;
        }
        auto* civ = core::dyn_cast< ar::InternalVariable >(called);
        if (civ == nullptr) {
          break;
        }
        ar::Statement* cdef = unique_def_bmc(civ);
        auto* un = core::dyn_cast_or_null< ar::UnaryOperation >(cdef);
        if (un == nullptr || un->op() != ar::UnaryOperation::Bitcast) {
          break;
        }
        called = un->operand();
      }
      if (is_nondet) {
        // Abstract initializers are not executed source nondet operations.
        // Missing DebugLoc also rejects legacy unmarked freeze_uninit calls.
        auto* source = call->has_frontend()
                           ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >())
                           : nullptr;
        if (source == nullptr || !source->getDebugLoc() ||
            source->getMetadata("ikos.freeze_uninit") != nullptr) {
          record(diagnostics, "value_nondet_origin", call);
          return false;
        }
        auto* it = core::dyn_cast< ar::IntegerType >(iv->type());
        if (it == nullptr) {
          return false;
        }
        z3::expr e =
            ctx.int_const((prefix + std::to_string(cache.size())).c_str());
        // 类型范围约束：nondet 只能取该整型范围内的值（sound，防 SMT 取到
        // 超出机器整型范围的值）。
        {
          std::uint64_t bw = it->bit_width();
          ZNumber lo, hi;
          if (it->is_signed()) {
            lo = -(ZNumber(1) << (bw - 1));
            hi = (ZNumber(1) << (bw - 1)) - 1;
          } else {
            lo = 0;
            hi = (ZNumber(1) << bw) - 1;
          }
          s.add(e >= ctx.int_val(lo.str().c_str()) &&
                e <= ctx.int_val(hi.str().c_str()));
          if (is_bool) {
            s.add(e >= 0 && e <= 1);
          }
        }
        if (path[definition_index].finite_source) {
          if (!source_nondet(call)) return false;
          s.add(e == ctx.int_val(path[definition_index].finite_value));
        }
        cache.emplace(key, e);
        out = e;
        return true;
      }
      auto* callee = callee_on_path(call, path, definition_index);
      if (callee != nullptr && callee->is_definition()) {
        for (std::size_t j = definition_index + 1; j < before; ++j) {
          auto* ret = core::dyn_cast< ar::ReturnValue >(path[j].stmt);
          if (ret != nullptr && ret->code() == callee->body_or_null() &&
              invocation_call(path, ret->code(), j) == definition_index) {
            if (!ret->has_operand() || !encode_value(ret->operand(), out, cache, prefix, depth + 1, j)) return false;
            auto* type = core::dyn_cast< ar::IntegerType >(iv->type());
            auto* returned = core::dyn_cast< ar::IntegerType >(ret->operand()->type());
            if (type == nullptr || returned == nullptr || type->bit_width() != returned->bit_width()) return false;
            if (type->is_signed() != returned->is_signed())
              out = z3::bv2int(z3::int2bv(type->bit_width(), out), type->is_signed());
            cache.emplace(key, out);
            return true;
          }
        }
      }
    }
    const char* reason = "value_unsupported";
    if (def == nullptr) {
      reason = "value_no_unique_definition";
    } else if (core::isa< ar::Load >(def)) {
      reason = "value_load";
    } else if (core::isa< ar::BinaryOperation >(def)) {
      reason = "value_arithmetic";
    } else if (core::isa< ar::CallBase >(def)) {
      reason = "value_unknown_call";
    }
    record(diagnostics, reason, def);
    return false;
  };

  // Trap/poison obligations are part of execution, including unused results.
  auto arithmetic_prefix = [&](const std::vector< BmcEvent >& path, ValueCache& cache,
                                const std::string& prefix) {
    for (std::size_t i = 0; i < path.size(); ++i) {
      if (path[i].synthetic_condition) continue;
      // A proven join consumes an actual opaque create identity. Numerical
      // consumers still go through encode_value and its library-source gate.
      if ((path[i].heap_allocation != nullptr || path[i].shared_main_local) && path[i].kind == EvKind::Read &&
          !(completion != nullptr && path[i].joined_handle_read)) {
        // Even an unused shared read must have a defined actual SC source.
        // Pointer or unsupported sources cannot become free malloc data.
        auto* load = core::cast< ar::Load >(path[i].stmt);
        z3::expr value(ctx);
        if (!encode_value(load->result(), value, cache, prefix, 0, i + 1)) {
          record(diagnostics, "heap_read_origin", load);
          return false;
        }
      }
      if (auto* op = core::dyn_cast< ar::BinaryOperation >(path[i].stmt)) {
        if (facts != nullptr && facts->no_wrap != nullptr) {
          auto proof = facts->no_wrap->find(op);
          if (proof != facts->no_wrap->end() && proof->second) {
            record(diagnostics, "ai_no_wrap", op);
            if (metrics != nullptr) ++metrics->ai_no_wrap;
            continue; // Demanded results still go through exact encode_value.
          }
        }
        z3::expr value(ctx);
        if (!encode_value(op->result(), value, cache, prefix, 0, i + 1)) {
          record(diagnostics, "prefix_arithmetic", op);
          return false;
        }
      }
    }
    return true;
  };
  auto encode_assume = [&](ar::Comparison* cmp,
                           ValueCache& cache,
                           const std::string& prefix, std::size_t before) -> z3::expr {
    ar::Value* l = cmp->left();
    ar::Value* r = cmp->right();
    // Only equality of proven function/null identities is modeled for pointers.
    if (cmp->predicate() == ar::Comparison::PEQ || cmp->predicate() == ar::Comparison::PNE) {
      z3::expr x(ctx), y(ctx);
      const auto id = trace_index(cache);
      if (functions.encode(l, id, before, x) && functions.encode(r, id, before, y)) {
        guard_reusable = false;
        return cmp->predicate() == ar::Comparison::PEQ ? x == y : x != y;
      }
      record(diagnostics, "assume_pointer_source", cmp);
      unencodable = true;
      return ctx.bool_val(true);
    }
    switch (cmp->predicate()) {
      case ar::Comparison::PGT:
      case ar::Comparison::PGE:
      case ar::Comparison::PLT:
      case ar::Comparison::PLE:
      case ar::Comparison::FOEQ:
      case ar::Comparison::FOGT:
      case ar::Comparison::FOGE:
      case ar::Comparison::FOLT:
      case ar::Comparison::FOLE:
      case ar::Comparison::FONE:
      case ar::Comparison::FORD:
      case ar::Comparison::FUNO:
      case ar::Comparison::FUEQ:
      case ar::Comparison::FUGT:
      case ar::Comparison::FUGE:
      case ar::Comparison::FULT:
      case ar::Comparison::FULE:
      case ar::Comparison::FUNE:
        record(diagnostics, "assume_predicate", cmp);
        unencodable = true;
        return ctx.bool_val(true);
      default:
        break;
    }
    // A1：编码两个 operand（常量 或 追溯到合法来源的值）。任一无法编码 → 不确认。
    z3::expr x(ctx), y(ctx);
    bool left_ok = encode_value(l, x, cache, prefix, 0, before);
    bool right_ok = encode_value(r, y, cache, prefix, 0, before);
    if (!left_ok || !right_ok) {
      record(diagnostics, "assume_value", cmp);
      unencodable = true;
      return ctx.bool_val(true);
    }
    switch (cmp->predicate()) {
      case ar::Comparison::UIEQ:
      case ar::Comparison::SIEQ:
        return x == y;
      case ar::Comparison::UINE:
      case ar::Comparison::SINE:
        return x != y;
      case ar::Comparison::UIGT:
      case ar::Comparison::SIGT:
        return x > y;
      case ar::Comparison::UIGE:
      case ar::Comparison::SIGE:
        return x >= y;
      case ar::Comparison::UILT:
      case ar::Comparison::SILT:
        return x < y;
      case ar::Comparison::UILE:
      case ar::Comparison::SILE:
        return x <= y;
      default:
        record(diagnostics, "assume_predicate", cmp);
        unencodable = true;
        return ctx.bool_val(true);
    }
  };

  auto assumptions = [&](const std::vector< BmcEvent >& path, ValueCache& cache, const std::string& prefix) {
    // A prepopulated cache may hide dependencies encoded for the other path.
    // A fixed startup contradiction can stop this group; worker rejections
    // additionally need the read/write check in the caller.
    guard_reusable = completion == nullptr && prefix != "value_init_" && cache.empty();
    guard_reads.clear();
    for (std::size_t i = 0; i < path.size(); ++i) {
      const auto& e = path[i];
      if (e.kind != EvKind::Assume) continue;
      z3::expr guard(ctx);
      if (auto* cmp = core::dyn_cast< ar::Comparison >(e.stmt)) {
        guard = encode_assume(cmp, cache, prefix, i);
      } else if (auto* call = core::dyn_cast< ar::CallBase >(e.stmt)) {
        z3::expr value(ctx);
        if (call->num_arguments() != 1 || !encode_value(call->argument(0), value, cache, prefix, 0, i)) {
          record(diagnostics, "assume_argument", e.stmt);
          unencodable = true;
        } else {
          guard = value != 0;
        }
      } else {
        record(diagnostics, "assume_statement", e.stmt);
        unencodable = true;
      }
      if (static_cast< bool >(guard)) {
        guard = guard.simplify();
        if (guard.is_false()) {
          record(diagnostics, "constant_guard_unsat", e.stmt);
          if (rejection != nullptr && selected_workers && prefix == "value_init_" && !unencodable)
            rejection->startup_infeasible = true;
          if (rejection != nullptr && guard_reusable && !unencodable) {
            rejection->path = prefix == "value_a_" ? 1 : 2;
            rejection->reads = guard_reads;
          }
          return false;
        }
        s.add(guard);
      }
    }
    return true;
  };
  for (std::size_t id = 0; id < traces.size(); ++id)
    if (!assumptions(traces[id].events, caches[id], traces[id].value_prefix()))
      return Result(unencodable ? Status::Unsupported : Status::BoundedUnsat);
  if (unencodable) return Result(Status::Unsupported);
  // The structural heap extent must be the value passed to this executed
  // malloc. In particular, a finite source choice cannot size the object
  // independently of the same nondet value used by loop guards.
  for (std::size_t id = 0; id < traces.size(); ++id) {
    const auto& path = traces[id].events;
    if (traces[id].root == nullptr || traces[id].root->name() != "main") continue;
    for (std::size_t i = 0; i < path.size(); ++i) {
      auto* call = core::dyn_cast< ar::CallBase >(path[i].stmt);
      if (call == nullptr || !malloc_call(call)) continue;
      core::ZNumber extent;
      z3::expr actual(ctx);
      if (!main_allocation(call, path, i, extent) ||
          !encode_value(call->argument(0), actual, caches[id],
                        traces[id].value_prefix(), 0, i)) {
        record(diagnostics, "heap_extent_source", call);
        return Result(Status::Unsupported);
      }
      s.add(actual == ctx.int_val(extent.str().c_str()));
    }
  }
  for (std::size_t id = 0; id < traces.size(); ++id)
    if (!arithmetic_prefix(traces[id].events, caches[id], traces[id].value_prefix())) return Result(Status::Unsupported);

  // A verifier atomic interval excludes every other dynamic instance.
  for (std::size_t id = 1; id < traces.size(); ++id)
    for (const auto& section : trace_sections[id]) {
      if (section.mutex != PSEUDO_ATOMIC_LOCK) continue;
      for (std::size_t other = 1; other < traces.size(); ++other) {
        if (other == id) continue;
        for (std::size_t j = 0; j < traces[other].events.size(); ++j)
          s.add(clock[traces[other].clock_start + j] < clock[traces[id].clock_start + section.begin] ||
                clock[traces[id].clock_start + section.end] < clock[traces[other].clock_start + j]);
      }
    }

  // Executed pointer definitions must select the same global object, even
  // when the checker's may-points-to sets contain several objects.
  {
    const BmcEvent& ea = pathA[targetA];
    const BmcEvent& eb = pathB[targetB];
    // 同址必须可证：base 提取失败（0）或 base 不同 → 缺失同址约束会让两访问被
    // 「假相邻」确认 → FP。保守不确认（UNKNOWN）。
    const bool same_heap = ea.heap_allocation != nullptr && ea.heap_allocation == eb.heap_allocation;
    const bool same_stack = ea.shared_main_local && eb.shared_main_local &&
                            ea.local_base != nullptr && ea.local_base == eb.local_base;
    if (!same_heap && !same_stack && (ea.base == 0 || eb.base == 0 || ea.base != eb.base)) {
      record(diagnostics, "address_base", ea.base == 0 ? ea.stmt : eb.stmt);
      return Result(Status::Unsupported);
    }
    auto encode_offset = [&](const BmcEvent& e, z3::expr& out,
                             ValueCache& cache, const std::string& prefix, std::size_t before) {
      out = ctx.int_val(e.const_off.str().c_str());
      if (e.var_id != 0) {
        z3::expr value(ctx);
        if (!encode_value(reinterpret_cast< ar::InternalVariable* >(e.var_id), value, cache, prefix, 0, e.var_before)) {
          record(diagnostics, "address_value", e.stmt); return false;
        }
        out = out + ctx.int_val(e.coeff.str().c_str()) * value;
      }
      replay_offsets.emplace(&e, out);
      return true;
    };
    auto valid_memory = [&](const std::vector< BmcEvent >& path, ValueCache& cache, const std::string& prefix) {
      for (std::size_t i = 0; i < path.size(); ++i) {
        const auto& e = path[i];
        if (e.kind != EvKind::Read && e.kind != EvKind::Write &&
            !((e.kind == EvKind::Lock || e.kind == EvKind::Unlock) && e.mutex != PSEUDO_ATOMIC_LOCK)) continue;
        core::ZNumber size;
        if (e.width == 0 || !memory_extent(e, size)) {record(diagnostics, "prefix_address", e.stmt); return false;}
        z3::expr offset(ctx);
        if (!encode_offset(e, offset, cache, prefix, i)) return false;
        s.add(offset >= 0 && offset + ctx.int_val(std::to_string(e.width).c_str()) <= ctx.int_val(size.str().c_str()));
        if (e.heap_allocation != nullptr || e.shared_main_local) {
          auto* type = e.kind == EvKind::Read ? core::cast< ar::Load >(e.stmt)->result()->type()
                                            : core::cast< ar::Store >(e.stmt)->value()->type();
          const auto alignment = e.stmt->bundle()->data_layout().abi_alignment(type);
          s.add(z3::mod(offset, ctx.int_val(alignment.str().c_str())) == 0);
        }
      }
      return true;
    };
    // Prove every retained heap GEP's intermediate offsets, including unused
    // results. Rejecting unproved poison is a supported-path boundary, not a
    // claim that unused LLVM poison itself is immediate UB.
    const bool has_heap = std::any_of(allocation_origin.begin(), allocation_origin.end(), [](const auto& event) {
      auto* call = core::dyn_cast< ar::CallBase >(event.stmt);
      return call != nullptr && malloc_call(call);
    });
    for (std::size_t id = 0; has_heap && id < traces.size(); ++id) {
      const auto& trace = traces[id];
      for (std::size_t i = 0; i < trace.events.size(); ++i) {
        auto* shift = core::dyn_cast< ar::PointerShift >(trace.events[i].stmt);
        if (shift == nullptr) continue;
        BmcEvent base;
        if (!pointer_on_path(shift->pointer(), trace.events, i, trace.root, trace.argument,
                             base, &allocation_origin)) return Result(Status::Unsupported);
        if (base.heap_allocation == nullptr) continue;
        auto* native = shift->has_frontend()
            ? llvm::dyn_cast< llvm::GetElementPtrInst >(shift->frontend< llvm::Value >()) : nullptr;
        if (native == nullptr || !native->isInBounds()) return Result(Status::Unsupported);
        const auto bits = native->getModule()->getDataLayout().getIndexSizeInBits(native->getPointerAddressSpace());
        if (bits == 0 || bits > 64) return Result(Status::Unsupported);
        const auto low = ctx.int_val((-(core::ZNumber(1) << (bits - 1))).str().c_str());
        const auto high = ctx.int_val(((core::ZNumber(1) << (bits - 1)) - 1).str().c_str());
        const auto size = ctx.int_val(base.heap_extent.str().c_str());
        z3::expr offset(ctx), delta = ctx.int_val(0);
        if (!encode_offset(base, offset, caches[id], trace.value_prefix(), i)) return Result(Status::Unsupported);
        replay_offsets.erase(&base);
        s.add(offset >= 0 && offset <= size);
        for (std::size_t term = 0; term < shift->num_terms(); ++term) {
          z3::expr index(ctx);
          auto* type = core::dyn_cast< ar::IntegerType >(shift->term(term).second->type());
          if (type == nullptr) return Result(Status::Unsupported);
          const auto index_width = std::min< std::uint64_t >(bits, type->bit_width());
          if (!encode_value(shift->term(term).second, index, caches[id], trace.value_prefix(), 0, i))
            return Result(Status::Unsupported);
          const auto product = index * ctx.int_val(shift->term(term).first.to_z_number().str().c_str());
          s.add(index >= ctx.int_val((-(core::ZNumber(1) << (index_width - 1))).str().c_str()) &&
                index <= ctx.int_val(((core::ZNumber(1) << (index_width - 1)) - 1).str().c_str()) &&
                product >= low && product <= high);
          delta = delta + product;
          s.add(delta >= low && delta <= high && offset + delta >= 0 && offset + delta <= size);
        }
      }
    }
    // Every deferred output, including dormant children's handles, must have
    // a concrete valid slot in this same main-only startup execution.
    for (const auto& storage : library_storage) {
      if (!storage.deferred_create) continue;
      const auto& event = startup[storage.occurrence];
      core::ZNumber extent;
      z3::expr offset(ctx);
      if (storage.alignment == 0 || !memory_extent(event, extent) ||
          !encode_offset(event, offset, cache_init, "value_init_", storage.occurrence))
        return Result(Status::Unsupported);
      s.add(offset >= 0 && offset + ctx.int_val(std::to_string(event.width).c_str()) <=
            ctx.int_val(extent.str().c_str()));
      s.add(z3::mod(offset, ctx.int_val(std::to_string(storage.alignment).c_str())) == 0);
      record(diagnostics, "startup_create_offset", event.stmt);
    }
    for (std::size_t id = 1; id < traces.size(); ++id)
      if (!valid_memory(traces[id].events, caches[id], traces[id].value_prefix())) return Result(Status::Unsupported);
    auto ownership = [&](const std::vector< Section >& sections, const std::vector< BmcEvent >& path,
                         ValueCache& cache, const std::string& prefix) {
      for (const auto& section : sections) {
        if (section.mutex == PSEUDO_ATOMIC_LOCK || section.end == path.size()) continue;
        const auto& lock = path[section.begin]; const auto& unlock = path[section.end];
        if (lock.base != unlock.base) return false;
        z3::expr first(ctx), last(ctx);
        if (!encode_offset(lock, first, cache, prefix, section.begin) ||
            !encode_offset(unlock, last, cache, prefix, section.end)) return false;
        s.add(first == last);
      }
      return true;
    };
    for (std::size_t id = 1; id < traces.size(); ++id)
      if (!ownership(trace_sections[id], traces[id].events, caches[id], traces[id].value_prefix())) return Result(Status::Unsupported);
    for (std::size_t x = 1; x < traces.size(); ++x) for (std::size_t y = x + 1; y < traces.size(); ++y)
      for (const auto& a : trace_sections[x]) for (const auto& b : trace_sections[y]) {
        if (a.mutex == PSEUDO_ATOMIC_LOCK || b.mutex == PSEUDO_ATOMIC_LOCK || (a.reader && b.reader)) continue;
        const auto& lock_a = traces[x].events[a.begin]; const auto& lock_b = traces[y].events[b.begin];
        if (lock_a.base != lock_b.base) continue;
        z3::expr address_a(ctx), address_b(ctx);
        if (!encode_offset(lock_a, address_a, caches[x], traces[x].value_prefix(), a.begin) ||
            !encode_offset(lock_b, address_b, caches[y], traces[y].value_prefix(), b.begin)) return Result(Status::Unsupported);
        auto a_first = a.end < traces[x].events.size() ? clock[traces[x].clock_start + a.end] < clock[traces[y].clock_start + b.begin] : ctx.bool_val(false);
        auto b_first = b.end < traces[y].events.size() ? clock[traces[y].clock_start + b.end] < clock[traces[x].clock_start + a.begin] : ctx.bool_val(false);
        s.add(address_a != address_b || a_first || b_first);
      }
    z3::expr off_a(ctx), off_b(ctx);
    if (!encode_offset(ea, off_a, cache_a, "value_a_", targetA) || !encode_offset(eb, off_b, cache_b, "value_b_", targetB)) return Result(Status::Unsupported);
    if (facts != nullptr) {
      if (facts->bounded_a) s.add(off_a >= ctx.int_val(facts->lo_a.str().c_str()) && off_a <= ctx.int_val(facts->hi_a.str().c_str()));
      if (facts->bounded_b) s.add(off_b >= ctx.int_val(facts->lo_b.str().c_str()) && off_b <= ctx.int_val(facts->hi_b.str().c_str()));
      if (facts->bounded_a || facts->bounded_b) record(diagnostics, "ai_candidate_bounds");
      if (metrics != nullptr) metrics->ai_bounds += facts->bounded_a + facts->bounded_b;
    }
    {
      // 字节范围重叠（而非起始 offset 相等）：`x`(u32@[0]) 与 `((u8*)&x)[1]`
      // (u8@[1]) 起始不同但范围重叠 → 同址（overlapping-widths.c FN）。width==0
      // （未知）退化为起始相等（保守，只丢 TP 不引 FP）。
      if (ea.width != 0 && eb.width != 0) {
        z3::expr w_a = ctx.int_val(std::to_string(ea.width).c_str());
        z3::expr w_b = ctx.int_val(std::to_string(eb.width).c_str());
        s.add(off_a < off_b + w_b && off_b < off_a + w_a);
      } else {
        s.add(off_a == off_b);
      }
    }
  }

  // 有无法编码的约束（变量值来源 / 指针 / 浮点比较）→ 保守不确认
  // （UNKNOWN），避免误报。
  if (unencodable) {
    return Result(Status::Unsupported);
  }

  // Preserve the old query for ordinary targets. Atomic targets may have a
  // completion suffix: pure local operations and atomic markers are not hb
  // barriers. Keep memory and real synchronization between them excluded.
  // POSIX stdout release/acquire orders the calls. Only the stream direction
  // that would order our targets is excluded; an intervening single output is
  // not itself a proof of HB between them (notably inside verifier atomic).
  for (std::size_t i = 0; i < nA; ++i) {
    if (!stream_output(pathA[i])) continue;
    for (std::size_t j = 0; j < nB; ++j) {
      if (!stream_output(pathB[j])) continue;
      if (targetA < i && j < targetB) s.add(clock[nA + j] < clock[i]);
      if (targetB < j && i < targetA) s.add(clock[i] < clock[nA + j]);
    }
  }
  bool solver_unknown = false;
  for (int dir = 0; dir < 2; ++dir) {
    // Each query is independent. push/pop switches Z3 to incremental solving,
    // disabling preprocessing needed by some mixed Int/BV guards.
    z3::solver query(ctx);
    auto timeout = solver_timeout_ms;
    if (deadline != nullptr) {
      const auto remaining = std::chrono::duration_cast< std::chrono::milliseconds >(*deadline - std::chrono::steady_clock::now()).count();
      if (remaining <= 0) {
        record(diagnostics, "search_deadline");
        return Result(Status::BudgetExceeded);
      }
      timeout = static_cast< unsigned >(std::min< std::int64_t >(remaining, timeout));
    }
    query.set("timeout", timeout);
    query.add(s.assertions());
    auto first = dir == 0 ? nA + targetB : targetA;
    auto last = dir == 0 ? targetA : nA + targetB;
    if (!atomicA && !atomicB) query.add(clock[last] == clock[first] + 1);
    else {
      query.add(clock[first] < clock[last]);
      auto exclude_between = [&](const std::vector< BmcEvent >& path, std::size_t start) {
        for (std::size_t i = 0; i < path.size(); ++i) {
          if (start + i == first || start + i == last) continue;
          const auto& event = path[i];
          if (!atomic_query_barrier(event)) continue;
          query.add(clock[start + i] < clock[first] || clock[last] < clock[start + i]);
        }
      };
      for (std::size_t id = 1; id < traces.size(); ++id)
        exclude_between(traces[id].events, traces[id].clock_start);
    }
    auto solve_start = std::chrono::steady_clock::now();
    z3::check_result result = query.check();
    const auto solve_end = std::chrono::steady_clock::now();
    if (metrics != nullptr) {
      ++metrics->solver_queries;
      metrics->assertions += query.assertions().size();
      metrics->solver_us += std::chrono::duration_cast< std::chrono::microseconds >(
          solve_end - solve_start).count();
    }
    record(diagnostics, result == z3::sat ? "sat" :
                        result == z3::unsat ? "smt_unsat" : "smt_unknown");
    if (diagnostics != nullptr && result == z3::unknown && query.reason_unknown() == "timeout")
      record(diagnostics, "smt_timeout");
    if (result == z3::sat) {
      Result outcome(Status::Confirmed);
      auto model = query.get_model();
      auto export_start = std::chrono::steady_clock::now();
      outcome.evidence = export_counterexample(traces, caches, clock, replay_offsets, model, &functions);
      if (metrics != nullptr) metrics->evidence_us += std::chrono::duration_cast< std::chrono::microseconds >(
          std::chrono::steady_clock::now() - export_start).count();
      return outcome;
    }
    solver_unknown |= result == z3::unknown;
  }
  return Result(solver_unknown ? Status::SolverUnknown : Status::BoundedUnsat);
}

}  // namespace bmc
}  // namespace analyzer
}  // namespace ikos
