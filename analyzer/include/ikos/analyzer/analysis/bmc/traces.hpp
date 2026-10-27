#pragma once

#include <ikos/ar/semantic/pthread.hpp>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "unroll.hpp"

namespace ikos { namespace analyzer { namespace bmc {

// Each trace is one dynamic invocation, even when AR variables are shared.
struct ExecutionTrace {
  static constexpr std::size_t no_creation = std::numeric_limits< std::size_t >::max();
  std::string id;
  ar::Function* root = nullptr;
  std::vector< BmcEvent > events;
  ThreadArgument argument;
  std::size_t creator_path = 0;
  std::size_t creator_step = no_creation;
  std::size_t clock_start = 0;

  std::string value_prefix() const {
    return id == "startup" ? "value_init_" : "value_" + id + "_";
  }
};

// A finite completion choice, not an abstract summary of omitted child writes.
struct CompletionPlan {
  std::size_t target_creation = ExecutionTrace::no_creation;
  std::vector< ExecutionTrace > auxiliary;
  std::map< std::size_t, std::size_t > joins; // main join step -> trace index
};

// A bounded main prefix is scheduled alongside its selected children. Index 0
// remains an empty legacy-startup slot, targets remain 1/2, main is 3, provider
// (if present) is 4. The actual dynamic creations, not entry names, bind roots.
struct StartupProviderPlan {
  std::vector< BmcEvent > main;
  ExecutionTrace provider;
  std::size_t creation_a = ExecutionTrace::no_creation;
  std::size_t creation_b = ExecutionTrace::no_creation;
  std::map< std::size_t, std::size_t > joins; // main step -> provider index 4
};

// Preserve exact step indices and the existing supported leaf-call semantics.
// This first timeline profile has direct creation and normal mutexes. Child
// lifecycles, condition expansion, semaphore/rwlock contracts, and dynamic heap
// lifetimes belong to separate profiles; none may be silently erased here.
inline bool startup_provider_prefix_supported(const std::vector< BmcEvent >& path,
                                               bool child = false) {
  for (const auto& event : path) {
    auto* call = core::dyn_cast< ar::CallBase >(event.stmt);
    auto* callee = call != nullptr ? exact_callee(call) : nullptr;
    if (event.kind == EvKind::Unsupported || event.synthetic_condition ||
        semaphore_event(event.kind) ||
        (call != nullptr && (condition_call(call) || heap_allocation_call(call) || free_call(call) ||
                             constant_output_call(call))) ||
        (callee != nullptr && (callee->name() == "pthread_detach" ||
                              callee->name().find("pthread_rwlock_") == 0)) ||
        (child && (event.kind == EvKind::Create || event.kind == EvKind::Join)) ||
        (event.called_function != nullptr && event.called_function->name() == "main")) return false;
    if (event.kind == EvKind::Create) {
      auto* owner = call != nullptr ? call->code()->function_or_null() : nullptr;
      if (call == nullptr || callee == nullptr || !callee->is_declaration() ||
          owner == nullptr || owner->name() != "main" ||
          call->num_arguments() != 4 ||
          !core::isa< ar::FunctionPointerConstant >(call->argument(2))) return false;
    }
  }
  return true;
}

inline bool bind_creation_step(ar::Function* child,
    const std::vector< BmcEvent >& creator, std::size_t step, ThreadArgument& out) {
  std::size_t ordinal = 0;
  for (std::size_t i = 0; i < creator.size(); ++i) {
    if (creator[i].kind != EvKind::Create ||
        creator[i].thread != reinterpret_cast< std::uint64_t >(child)) continue;
    if (i == step) return bind_created_argument(child, creator, ordinal, out);
    ++ordinal;
  }
  return false;
}

// A finite graph of actual successful Create occurrences. Targets remain 1/2,
// slot 0 is empty, timed main is 3; every other trace is a real child prefix.
// No omitted children, dormant invocations, joins or allocation summaries.
struct CreatorGraphPlan {
  std::vector< ExecutionTrace > traces;
};

inline bool creator_graph_create_abi(ar::CallBase* call, ar::Function* child) {
  auto* api = exact_callee(call);
  auto* source = call && call->has_frontend()
      ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >()) : nullptr;
  auto* native_api = api && api->has_frontend() ? api->frontend< llvm::Function >() : nullptr;
  auto* native_child = child && child->has_frontend() ? child->frontend< llvm::Function >() : nullptr;
  if (!source || !llvm::isa< llvm::CallInst >(source) || !native_api || !native_child ||
      !api->is_declaration() || native_api->getName() != "pthread_create" ||
      !native_api->isDeclaration() || native_api->isVarArg() || native_api->arg_size() != 4 ||
      source->arg_size() != 4 || source->getCalledOperand()->stripPointerCasts() != native_api ||
      source->getFunctionType() != native_api->getFunctionType() || source->hasOperandBundles() ||
      source->getCallingConv() != llvm::CallingConv::C || native_api->getCallingConv() != llvm::CallingConv::C ||
      !source->getType()->isIntegerTy(32) || !native_api->getReturnType()->isIntegerTy(32) ||
      !child->is_definition() || child->num_parameters() != 1 || child->type()->is_var_arg() ||
      !child->param(0)->type()->is_pointer() || !child->type()->return_type()->is_pointer() ||
      native_child->isDeclaration() || native_child->isVarArg() || native_child->arg_size() != 1 ||
      native_child->getCallingConv() != llvm::CallingConv::C ||
      source->getArgOperand(2)->stripPointerCasts() != native_child ||
      !ordinary_source_call_abi_attributes(source, native_api) ||
      !ordinary_source_function_abi_attributes(native_child)) return false;
  auto* entry_pointer = llvm::dyn_cast< llvm::PointerType >(native_api->getFunctionType()->getParamType(2));
  auto* parameter = llvm::dyn_cast< llvm::PointerType >(native_child->getFunctionType()->getParamType(0));
  auto* returned = llvm::dyn_cast< llvm::PointerType >(native_child->getReturnType());
  if (!entry_pointer || entry_pointer->isOpaque() || entry_pointer->getAddressSpace() != 0 ||
      entry_pointer->getPointerElementType() != native_child->getFunctionType() ||
      !parameter || parameter->isOpaque() || parameter->getAddressSpace() != 0 ||
      !parameter->getPointerElementType()->isIntegerTy(8) ||
      !returned || returned->isOpaque() || returned->getAddressSpace() != 0 ||
      !returned->getPointerElementType()->isIntegerTy(8)) return false;
  for (unsigned i = 0; i < 4; ++i)
    for (auto attribute : {llvm::Attribute::ByVal, llvm::Attribute::StructRet,
                           llvm::Attribute::InAlloca, llvm::Attribute::Preallocated})
      if (source->paramHasAttr(i, attribute) || native_api->hasParamAttribute(i, attribute) ||
          (i == 0 && native_child->hasParamAttribute(0, attribute))) return false;
  std::uint64_t width, alignment;
  return create_output_layout(call, width, alignment);
}

inline bool creator_graph_prefix_supported(const ExecutionTrace& trace) {
  if (!trace.root || trace.events.empty()) return false;
  std::set< ar::Function* > invoked{trace.root};
  std::set< ar::LocalVariable* > allocated, live;
  for (std::size_t i = 0; i < trace.events.size(); ++i) {
    const auto& event = trace.events[i];
    auto* call = core::dyn_cast< ar::CallBase >(event.stmt);
    auto* callee = call ? exact_callee(call) : nullptr;
    if (event.kind == EvKind::Unsupported || event.kind == EvKind::Join ||
        event.synthetic_atomic || event.synthetic_condition || event.synthetic_barrier ||
        event.mutex == PSEUDO_ATOMIC_LOCK || semaphore_event(event.kind) ||
        barrier_event(event.kind) ||
        (call && (condition_call(call) || thread_exit_call(call) || heap_allocation_call(call) || free_call(call) ||
                  constant_output_call(call) || is_verifier_atomic(call))) ||
        event.recursive_mutex || event.recursive_profile ||
        (callee && (callee->name() == "pthread_detach" ||
                    callee->name().find("pthread_rwlock_") == 0 ||
                    callee->name().find("pthread_mutexattr_") == 0 ||
                    (callee->name() == "pthread_mutex_init" && call->num_arguments() == 2 &&
                     !core::isa< ar::NullConstant >(call->argument(1)))))) return false;
    // Static SSA/local caches are per physical trace. Until their frame key is
    // exported, reject *every* repeated helper/callback activation, not merely
    // repeated alloca instructions. Two different physical traces remain valid.
    auto* invoked_root = event.once_stage == OnceStage::Enter ? event.once_callback :
                         event.called_function;
    if (invoked_root && (invoked_root->name() == "main" ||
                         !invoked.insert(invoked_root).second)) return false;
    if (auto* allocation = core::dyn_cast< ar::Allocate >(event.stmt)) {
      if (!allocated.insert(allocation->result()).second) return false;
      live.insert(allocation->result());
    }
    if (auto* returned = core::dyn_cast< ar::ReturnValue >(event.stmt)) {
      if (returned->code() == trace.root->body_or_null() && trace.root->name() == "main")
        return false; // a main Return ends the process, not just this prefix
      for (auto it = live.begin(); it != live.end();) {
        if ((*it)->function()->body_or_null() == returned->code()) it = live.erase(it);
        else ++it;
      }
    }
    ar::Value* pointer = nullptr;
    auto* load = core::dyn_cast< ar::Load >(event.stmt);
    auto* store = core::dyn_cast< ar::Store >(event.stmt);
    if ((load && load->is_atomic()) || (store && store->is_atomic())) return false;
    if (load) pointer = load->operand();
    if (store) pointer = store->pointer();
    if (event.zero_fill && call) pointer = call->argument(0);
    if (event.kind == EvKind::Create) {
      if (!call || !callee || !callee->is_declaration() || call->num_arguments() != 4 ||
          !core::isa< ar::FunctionPointerConstant >(call->argument(2)) ||
          !core::isa< ar::NullConstant >(call->argument(1)) ||
          !core::isa< ar::NullConstant >(call->argument(3)) ||
          !creator_graph_create_abi(call, reinterpret_cast< ar::Function* >(event.thread))) return false;
      pointer = call->argument(0);
    }
    if (pointer) {
      BmcEvent address;
      if (!pointer_on_path(pointer, trace.events, i, trace.root, trace.argument, address) ||
          address.heap_allocation || (address.shared_main_local && trace.root->name() != "main") ||
          (address.local_base && !live.count(address.local_base))) return false;
      if (event.kind == EvKind::Create) {
        core::ZNumber extent;
        std::uint64_t width, alignment;
        llvm::Type* storage = nullptr;
        if (address.base) {
          auto* global = reinterpret_cast< ar::GlobalVariable* >(address.base);
          if (!global->has_frontend() || global->is_thread_local()) return false;
          storage = global->frontend< llvm::GlobalVariable >()->getValueType();
        } else if (address.local_base && address.local_base->has_frontend()) {
          auto* allocation = llvm::dyn_cast< llvm::AllocaInst >(address.local_base->frontend< llvm::Value >());
          if (allocation) storage = allocation->getAllocatedType();
        }
        while (auto* array = llvm::dyn_cast_or_null< llvm::ArrayType >(storage)) storage = array->getElementType();
        if (!storage || !storage->isIntegerTy(call->bundle()->data_layout().pointers.bit_width) ||
            address.var_id != 0 || !memory_extent(address, extent) ||
            !create_output_layout(call, width, alignment) || !constant_create_output_aligned(call, address) ||
            address.const_off < 0 || address.const_off + width > extent) return false;
      }
      // No automatic object may escape into another physical thread. NULL
      // thread arguments and no global pointer stores close this first scope.
      if (store && store->value()->type()->is_pointer() && !address.local_base) return false;
    }
  }
  return !open_once_prefix(trace.events);
}

inline bool bind_creator_graph(std::vector< ExecutionTrace >& traces) {
  if (traces.size() < 4 || traces.size() > 9 || traces[0].id != "startup" ||
      !traces[0].events.empty() || traces[1].id != "a" || traces[2].id != "b" ||
      traces[3].id != "main" || !traces[3].root || traces[3].root->name() != "main" ||
      traces[3].creator_step != ExecutionTrace::no_creation) return false;
  std::set< std::pair< std::size_t, std::size_t > > bindings, actual;
  std::size_t count = 0;
  for (std::size_t id = 1; id < traces.size(); ++id) {
    auto& child = traces[id];
    if (!child.root || child.events.empty() || (id != 3 && child.root->name() == "main") ||
        (id >= 4 && child.id != "aux_" + std::to_string(id - 4))) return false;
    count += child.events.size();
    std::size_t targets = 0;
    for (std::size_t step = 0; step < child.events.size(); ++step) {
      if (child.events[step].candidate) ++targets;
      if (child.events[step].kind == EvKind::Create) actual.emplace(id, step);
    }
    if (targets != (id == 1 || id == 2 ? 1u : 0u)) return false;
    if (id == 3) continue;
    if (child.creator_path == 0 || child.creator_path >= traces.size() ||
        child.creator_path == id ||
        !bindings.emplace(child.creator_path, child.creator_step).second ||
        !bind_creation_step(child.root, traces[child.creator_path].events,
                            child.creator_step, child.argument)) return false;
    // Parent indices need not be topologically sorted after target placement.
    std::set< std::size_t > visited;
    auto ancestor = id;
    while (ancestor != 3) {
      if (ancestor == 0 || ancestor >= traces.size() || !visited.insert(ancestor).second) return false;
      ancestor = traces[ancestor].creator_path;
    }
  }
  if (count > MAX_PATH_EVENTS || actual != bindings) return false;
  for (std::size_t id = 1; id < traces.size(); ++id)
    if (!creator_graph_prefix_supported(traces[id])) return false;
  return true;
}

// A complete first helper-heap slice: one timed physical main and one child,
// with all allocator/helper frames returned before the sole direct Create.
// Static publication is still sealed by published_pointer, never guessed RF.
inline bool main_helper_heap_profile_supported(
    const std::vector< ExecutionTrace >& traces, std::size_t main_index) {
  if (traces.size() != 3 || !traces[0].events.empty() ||
      (main_index != 1 && main_index != 2) || !traces[main_index].root ||
      traces[main_index].root->name() != "main") return false;
  const auto& origin = traces[main_index].events;
  if (!helper_heap_path(origin)) return false;
  std::size_t creation = origin.size();
  unsigned allocations = 0;
  for (std::size_t i = 0; i < origin.size(); ++i) {
    const auto& event = origin[i];
    if (event.kind != EvKind::Create) continue;
    auto* call = core::dyn_cast< ar::CallBase >(event.stmt);
    if (creation != origin.size() || !call || call->code()->function_or_null() != traces[main_index].root ||
        invocation_call(origin, call->code(), i) != origin.size() || call->num_arguments() != 4 ||
        !core::isa< ar::NullConstant >(call->argument(1)) ||
        !core::isa< ar::NullConstant >(call->argument(3))) return false;
    BmcEvent output;
    if (!pointer_on_path(call->argument(0), origin, i, traces[main_index].root,
                         traces[main_index].argument, output, &origin) || output.heap_allocation) return false;
    creation = i;
  }
  if (creation == origin.size()) return false;
  for (std::size_t id = 1; id < traces.size(); ++id) {
    const auto& trace = traces[id];
    if (!trace.root || trace.events.empty() || (id != main_index && trace.root->name() == "main")) return false;
    for (std::size_t i = 0; i < trace.events.size(); ++i) {
      const auto& event = trace.events[i];
      auto* call = core::dyn_cast< ar::CallBase >(event.stmt);
      auto* api = call ? exact_callee(call) : nullptr;
      if (event.kind == EvKind::Unsupported || event.kind == EvKind::Join ||
          event.once_stage != OnceStage::None || event.synthetic_atomic ||
          event.synthetic_condition || event.synthetic_barrier ||
          event.mutex == PSEUDO_ATOMIC_LOCK || event.recursive_profile || event.recursive_mutex ||
          semaphore_event(event.kind) || barrier_event(event.kind) ||
          (id != main_index && event.kind == EvKind::Create) ||
          (call && (free_call(call) || thread_exit_call(call) || condition_call(call) ||
                    constant_output_call(call) || is_verifier_atomic(call))) ||
          (api && (api->name() == "pthread_detach" || api->name() == "realloc" ||
                   api->name().find("pthread_rwlock_") == 0 ||
                   api->name().find("pthread_mutexattr_") == 0 ||
                   (api->name() == "pthread_mutex_init" && call->num_arguments() == 2 &&
                    !core::isa< ar::NullConstant >(call->argument(1)))))) return false;
      if (auto* ret = core::dyn_cast< ar::ReturnValue >(event.stmt))
        if (ret->code() == traces[main_index].root->body_or_null()) return false;
      if (auto* cast = core::dyn_cast< ar::UnaryOperation >(event.stmt))
        if (cast->op() == ar::UnaryOperation::PtrToSI || cast->op() == ar::UnaryOperation::PtrToUI ||
            cast->op() == ar::UnaryOperation::SIToPtr || cast->op() == ar::UnaryOperation::UIToPtr) return false;
      // Frame-local arrays are a separate identity model. This slice needs
      // no helper alloca to represent optimized scalar/pointer temporaries.
      if (core::isa< ar::Allocate >(event.stmt) &&
          event.stmt->code()->function_or_null() != trace.root) return false;
      if (call && heap_allocation_call(call)) {
        core::ZNumber extent;
        if (id != main_index || i >= creation || ++allocations > 8 ||
            !helper_heap_allocator_abi(call) || !main_allocation(call, origin, i, extent)) return false;
      }
      if (id == main_index && i > creation && event.called_function != nullptr) return false;
      auto* store = core::dyn_cast< ar::Store >(event.stmt);
      if (!store || !store->value()->type()->is_pointer()) continue;
      BmcEvent destination;
      if (!pointer_on_path(store->pointer(), trace.events, i, trace.root, trace.argument,
                           destination, &origin)) return false;
      if (id != main_index || i >= creation) return false;
      if (destination.heap_allocation != nullptr) {
        // Initialize pointer fields only with literal NULL. Mutable/cyclic
        // object graphs and typed pointer RF are explicitly a later stage.
        if (!core::isa< ar::NullConstant >(store->value())) return false;
      } else if (destination.base != 0) {
        auto* global = global_pointer_slot(store->pointer());
        std::size_t at = 0;
        if (!global || published_pointer(global, origin, at) != store || at != i) return false;
        if (!core::isa< ar::NullConstant >(store->value())) {
          BmcEvent published;
          if (!pointer_on_path(store->value(), origin, i, trace.root, trace.argument, published, &origin) ||
              !published.heap_allocation || published.var_id != 0) return false;
        }
      } else if (!destination.local_base) return false;
    }
  }
  return allocations != 0;
}

// Version one keeps dynamic step indices stable and excludes nested lifecycles.
inline bool completion_prefix_supported(const std::vector< BmcEvent >& path,
                                        bool child = false) {
  for (const auto& event : path) {
    auto* call = core::dyn_cast< ar::CallBase >(event.stmt);
    auto* function = call != nullptr ? exact_callee(call) : nullptr;
    if (event.synthetic_condition || (call != nullptr && condition_call(call)) ||
        (function != nullptr && function->name() == "pthread_detach") ||
        (child && (event.kind == EvKind::Create || event.kind == EvKind::Join)) ||
        (event.called_function != nullptr && event.called_function->name() == "main")) return false;
  }
  return true;
}

inline ar::ReturnValue* completion_return(const std::vector< BmcEvent >& path,
                                         ar::Function* root) {
  auto* ret = path.empty() ? nullptr : core::dyn_cast< ar::ReturnValue >(path.back().stmt);
  return ret != nullptr && ret->code() == root->body_or_null() ? ret : nullptr;
}

// A proven integer constant cast carries no object access capability. This is
// only the handle non-escape audit; the encoder still binds the actual argument.
inline bool constant_numeric_thread_argument(ar::Value* value,
    const std::vector< BmcEvent >& path, std::size_t before) {
  auto* variable = core::dyn_cast< ar::InternalVariable >(value);
  if (!variable) return false;
  std::size_t index = 0;
  auto* cast = core::dyn_cast_or_null< ar::UnaryOperation >(
      reaching_definition(variable, path, before, index));
  if (!cast || (cast->op() != ar::UnaryOperation::SIToPtr &&
                cast->op() != ar::UnaryOperation::UIToPtr)) return false;
  auto* type = core::dyn_cast< ar::IntegerType >(cast->operand()->type());
  core::ZNumber integer;
  return type && type->bit_width() == cast->bundle()->data_layout().pointers.bit_width &&
         constant_integer_on_path(cast->operand(), path, index, integer);
}

// Match generations at the consuming load, before a later slot overwrite.
// Only an unescaped, typed scalar/array in this main invocation is supported.
inline bool completion_join_creation(const std::vector< BmcEvent >& path,
                                      std::size_t before, std::size_t& creation,
                                      bool completed_main = false,
                                      std::size_t* handle_read = nullptr) {
  auto* join = core::dyn_cast< ar::CallBase >(path[before].stmt);
  auto* api = join != nullptr ? exact_callee(join) : nullptr;
  if (api == nullptr || !api->is_declaration() || join->num_arguments() != 2 ||
      !ar::is_pthread_join(api) ||
      !core::isa< ar::NullConstant >(join->argument(1))) return false;
  int atomic = 0;
  for (std::size_t i = 0; i < before; ++i) {
    if (path[i].mutex != PSEUDO_ATOMIC_LOCK) continue;
    if (path[i].kind == EvKind::Lock) ++atomic;
    if (path[i].kind == EvKind::Unlock) --atomic;
  }
  if (atomic != 0) return false;
  ar::Value* value = join->argument(0);
  ar::Load* load = nullptr;
  std::size_t read = before;
  for (unsigned depth = 0; depth < 32; ++depth) {
    auto* variable = core::dyn_cast< ar::InternalVariable >(value);
    if (variable == nullptr) return false;
    std::size_t index = 0;
    auto* definition = reaching_definition(variable, path, read, index);
    if ((load = core::dyn_cast_or_null< ar::Load >(definition))) {read = index; break;}
    auto* assignment = core::dyn_cast_or_null< ar::Assignment >(definition);
    if (assignment == nullptr || assignment->operand()->type() != value->type()) return false;
    value = assignment->operand(); read = index;
  }
  auto* main = join->bundle()->function_or_null("main");
  BmcEvent address;
  if (load == nullptr || !load->result()->type()->is_integer() ||
      !pointer_on_path(load->operand(), path, read, main, nullptr, address) ||
      address.var_id != 0 || address.base != 0) return false;
  const bool heap = address.heap_allocation != nullptr;
  if (!heap && (address.local_base == nullptr || address.local_base->function() != main)) return false;
  bool live = heap || shared_main_allocation(address.local_base, path);
  if (!live && completed_main) {
    // Other retained children must Return < join < main Return. Only this
    // handle consumption needs the main stack live, not the later root Return.
    std::vector< BmcEvent > prefix(path.begin(), path.begin() + before + 1);
    live = shared_main_allocation(address.local_base, prefix);
  }
  if (!live) return false;
  const auto width = join->bundle()->data_layout().pointers.bit_width / 8;
  core::ZNumber extent;
  auto* type = heap ? core::cast< ar::PointerType >(load->operand()->type())->pointee()
                    : address.local_base->type()->pointee();
  while (auto* array = core::dyn_cast< ar::ArrayType >(type)) type = array->element_type();
  auto* integer = core::dyn_cast< ar::IntegerType >(type);
  if (integer == nullptr || integer->bit_width() != width * 8 ||
      join->bundle()->data_layout().store_size_in_bytes(load->result()->type()) != core::ZNumber(width) ||
      !memory_extent(address, extent) || address.const_off < 0 ||
      address.const_off + width > extent || address.const_off % width != 0) return false;
  auto same_object = [&](const BmcEvent& other) {
    return heap ? same_heap_object(other, address)
                : other.local_base == address.local_base;
  };
  bool matched = false;
  for (std::size_t j = read; j-- > 0;) {
    if (heap ? j == address.heap_allocation_step
             : path[j].stmt->result_or_null() == address.local_base) break;
    if (path[j].kind != EvKind::Create) continue;
    auto* call = core::cast< ar::CallBase >(path[j].stmt);
    BmcEvent output;
    if (!pointer_on_path(call->argument(0), path, j, main, nullptr, output)) return false;
    if (!same_object(output)) continue;
    std::uint64_t output_width, alignment;
    if (output.var_id != 0 || !create_output_layout(call, output_width, alignment)) return false;
    if (output.const_off + output_width <= address.const_off ||
        address.const_off + width <= output.const_off) continue;
    if (output.const_off != address.const_off || output_width != width ||
        !successful_create_result(path[j]) || !constant_create_output_aligned(call, output)) return false;
    creation = j; matched = true; break;
  }
  if (!matched) return false;
  // Calls and explicit stores cannot mutate or publish the opaque handle object.
  for (std::size_t i = 0; i < path.size(); ++i) {
    if (auto* store = core::dyn_cast< ar::Store >(path[i].stmt)) {
      if (abstract_initializer(store)) continue;
      BmcEvent output;
      if (!pointer_on_path(store->pointer(), path, i, main, nullptr, output) ||
          same_object(output)) return false;
      if (store->value()->type()->is_pointer() &&
          !core::isa< ar::NullConstant >(store->value()) &&
          !core::isa< ar::FunctionPointerConstant >(store->value())) {
        BmcEvent stored;
        if (!pointer_on_path(store->value(), path, i, main, nullptr, stored) ||
            same_object(stored)) return false;
      }
    }
    auto* call = core::dyn_cast< ar::CallBase >(path[i].stmt);
    if (call == nullptr) continue;
    for (std::size_t arg = 0; arg < call->num_arguments(); ++arg) {
      auto* operand = call->argument(arg);
      if (!operand->type()->is_pointer() || core::isa< ar::NullConstant >(operand) ||
          core::isa< ar::FunctionPointerConstant >(operand)) continue;
      if (completed_main && path[i].kind == EvKind::Create && arg == 3 &&
          constant_numeric_thread_argument(operand, path, i)) continue;
      BmcEvent passed;
      if (!pointer_on_path(operand, path, i, main, nullptr, passed) ||
          (same_object(passed) &&
           !(path[i].kind == EvKind::Create && arg == 0) &&
           !(heap && free_call(call) && arg == 0 && i > before && passed.const_off == 0))) return false;
    }
  }
  if (heap) {
    // Opaque identity is copied to this join, never interpreted as an integer.
    std::set< ar::Value* > aliases{load->result()};
    bool consumed = false;
    for (std::size_t i = read + 1; i < path.size(); ++i) {
      auto* statement = path[i].stmt;
      bool used = false;
      for (auto operand = statement->op_begin(); operand != statement->op_end(); ++operand)
        used |= aliases.count(*operand) != 0;
      auto* copy = core::dyn_cast< ar::Assignment >(statement);
      const bool forwarding = used && copy != nullptr &&
                              copy->result()->type() == copy->operand()->type();
      if (used && !forwarding) {
        if (i != before || !aliases.count(join->argument(0))) return false;
        consumed = true;
      }
      if (auto* result = statement->result_or_null()) aliases.erase(result);
      if (forwarding) aliases.insert(copy->result());
    }
    if (!consumed) return false;
  }
  if (handle_read != nullptr) *handle_read = read;
  return true;
}

} } }
