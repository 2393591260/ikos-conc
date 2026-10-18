// A successful fast-path prefix, not a general semaphore or safety analysis.
#pragma once
#include "facts.hpp"
#include "traces.hpp"

namespace ikos { namespace analyzer { namespace bmc {

inline bool semaphore_prefix_supported(const std::vector< ExecutionTrace >& traces,
    const CandidateFacts* facts, const CompletionPlan* completion) {
  bool used = false;
  for (const auto& trace : traces) for (const auto& event : trace.events)
    used |= semaphore_event(event.kind);
  if (!used) return true;
  if (completion != nullptr || traces.size() != 3 || facts == nullptr ||
      !facts->semaphore_effect || traces[1].root->name() == "main" ||
      traces[2].root->name() == "main" ||
      traces[1].creator_step >= traces[0].events.size() ||
      traces[2].creator_step >= traces[0].events.size() ||
      traces[1].creator_step == traces[2].creator_step) return false;
  std::map< ar::GlobalVariable*, unsigned > credits;
  for (std::size_t id = 0; id < traces.size(); ++id) {
    const auto& trace = traces[id];
    if (id != 0) {
      const auto target = candidate_index(trace.events);
      if (target >= trace.events.size() ||
          (trace.events[target].kind != EvKind::Read && trace.events[target].kind != EvKind::Write))
        return false;
    }
    for (std::size_t i = 0; i < trace.events.size(); ++i) {
      const auto& event = trace.events[i];
      if (event.synthetic_atomic || event.synthetic_condition ||
          event.mutex == PSEUDO_ATOMIC_LOCK || event.kind == EvKind::Join ||
          (id != 0 && event.kind == EvKind::Create)) return false;
      if (!semaphore_event(event.kind)) continue;
      auto* call = core::dyn_cast< ar::CallBase >(event.stmt);
      auto* global = call != nullptr ? facts->semaphore_effect(call) : nullptr;
      auto* source = call && call->has_frontend()
          ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >()) : nullptr;
      BmcEvent address;
      if (global == nullptr || source == nullptr || !source->use_empty() ||
          call->num_arguments() != (event.kind == EvKind::SemaphoreInit ? 3 : 1) ||
          !pointer_on_path(call->argument(0), trace.events, i, trace.root,
                           trace.argument, address, &traces[0].events) ||
          address.base != reinterpret_cast< std::uint64_t >(global) ||
          address.var_id != 0 || address.const_off != 0) return false;
      if (call->has_result()) for (auto* block : *call->code())
        for (auto* statement : *block) for (auto op = statement->op_begin(); op != statement->op_end(); ++op)
          if (*op == call->result()) return false;
      if (id != 0 && event.kind != EvKind::SemaphoreWait) return false;
      if (event.kind == EvKind::SemaphoreInit) {
        auto* shared = core::dyn_cast< ar::IntegerConstant >(call->argument(1));
        auto* value = core::dyn_cast< ar::IntegerConstant >(call->argument(2));
        if (credits.count(global) || !shared || !shared->value().is_zero() || !value) return false;
        const auto initial = value->value().to_z_number();
        if (initial < 0 || initial > 2147483647) return false;
        credits.emplace(global, initial.to< unsigned >());
      } else {
        auto found = credits.find(global);
        if (found == credits.end()) return false;
        if (event.kind == EvKind::SemaphorePost) {
          if (found->second == 2147483647) return false;
          ++found->second;
        } else {
          if (found->second == 0) return false;
          --found->second;
        }
      }
    }
  }
  // All worker waits fit the preexisting credits: no worker provider is needed.
  return true;
}

} } }
