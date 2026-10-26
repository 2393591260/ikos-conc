// Closed one-shot barrier executions. Failure leaves the candidate unconfirmed.
#pragma once
#include "facts.hpp"
#include "traces.hpp"
#include <cstdlib>

namespace ikos { namespace analyzer { namespace bmc {

struct BarrierRound {
  ar::GlobalVariable* object;
  std::vector< std::pair< std::size_t, std::size_t > > arrivals;
};
inline bool prepare_barrier_execution(std::vector< ExecutionTrace >& traces,
    const CandidateFacts* facts, const CompletionPlan* completion,
    const StartupProviderPlan* provider, std::vector< BarrierRound >& rounds) {
  bool used = false;
  for (const auto& trace : traces) for (const auto& event : trace.events)
    used |= barrier_event(event.kind);
  if (!used) return true;
  if (std::getenv("IKOS_BMC_DISABLE_BARRIER_EXECUTION") || !facts ||
      !facts->barrier_effect || completion || provider || traces.size() != 3 ||
      (traces[1].root->name() != "main" && traces[2].root->name() != "main")) return false;
  std::map< ar::GlobalVariable*, std::pair< std::size_t, std::size_t > > initialized;
  bool spawned = false;
  for (std::size_t id = 0; id < traces.size(); ++id) {
    auto& trace = traces[id];
    if (id == 0 && !trace.events.empty()) return false;
    std::set< ar::GlobalVariable* > waited;
    for (std::size_t i = 0; i < trace.events.size(); ++i) {
      auto& event = trace.events[i];
      if (!barrier_event(event.kind)) {
        if (event.kind == EvKind::Create) {
          if (trace.root->name() != "main" || !waited.empty()) return false;
          spawned = true;
        }
        if (event.synthetic_atomic || event.synthetic_condition) return false;
        continue;
      }
      auto* call = core::dyn_cast< ar::CallBase >(event.stmt);
      auto* global = call ? facts->barrier_effect(call) : nullptr;
      auto* source = call && call->has_frontend()
          ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >()) : nullptr;
      auto* function = call ? exact_callee(call) : nullptr;
      BmcEvent address;
      if (!global || !source || !function || !function->is_declaration() ||
          !source->use_empty() || unsupported_source_call_abi(call, function) ||
          call->num_arguments() != (event.kind == EvKind::BarrierInit ? 3u : 1u) ||
          !pointer_on_path(call->argument(0), trace.events, i, trace.root, trace.argument, address) ||
          address.base != reinterpret_cast< std::uint64_t >(global) ||
          address.const_off != 0 || address.var_id != 0) return false;
      if (call->has_result()) for (auto* block : *call->code())
        for (auto* statement : *block) for (auto op = statement->op_begin(); op != statement->op_end(); ++op)
          if (*op == call->result()) return false;
      if (event.kind == EvKind::BarrierInit) {
        auto* capacity = core::dyn_cast< ar::IntegerConstant >(call->argument(2));
        auto* native_capacity = llvm::dyn_cast< llvm::ConstantInt >(source->getArgOperand(2));
        if (trace.root->name() != "main" || spawned || !waited.empty() ||
            initialized.count(global) || !core::isa< ar::NullConstant >(call->argument(1)) ||
            !capacity || capacity->value().to_z_number() != 2 ||
            !native_capacity || !native_capacity->equalsInt(2)) return false;
        initialized.emplace(global, std::make_pair(id, i));
      } else {
        if (!waited.insert(global).second) return false;
      }
    }
  }
  // Expand only after activation binding. Creates precede every main wait, so
  // their dynamic indices remain unchanged by these departure markers.
  std::map< ar::GlobalVariable*, std::size_t > round_ids;
  for (std::size_t id = 1; id < traces.size(); ++id) {
    auto& path = traces[id].events;
    for (std::size_t i = 0; i < path.size(); ++i) {
      if (path[i].kind != EvKind::BarrierWait) continue;
      auto* call = core::cast< ar::CallBase >(path[i].stmt);
      auto* global = facts->barrier_effect(call);
      if (!initialized.count(global)) return false;
      auto found = round_ids.find(global);
      if (found == round_ids.end()) {
        found = round_ids.emplace(global, rounds.size()).first;
        rounds.push_back({global, {}});
      }
      rounds[found->second].arrivals.emplace_back(id, i);
      BmcEvent departure(call, EvKind::BarrierDepart);
      departure.synthetic_barrier = true;
      path.insert(path.begin() + i + 1, departure);
      if (path.size() > MAX_PATH_EVENTS) return false;
      ++i;
    }
  }
  for (const auto& round : rounds)
    if (round.arrivals.size() != 2 || round.arrivals[0].first == round.arrivals[1].first) return false;
  return true;
}

} } }
