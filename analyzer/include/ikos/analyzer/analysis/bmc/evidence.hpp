#pragma once

#include "result.hpp"
#include "replay.hpp"

namespace ikos { namespace analyzer { namespace bmc {

// Export a selected model only. Instruction semantics come from ReplaySnapshot;
// the certificate contains occurrences and values, not trusted transfer rules.
template < typename ValueCache >
Counterexample export_counterexample(ar::Function* funcA, ar::Function* funcB,
    const std::vector< BmcEvent >& startup, const std::vector< BmcEvent >& pathA,
    const std::vector< BmcEvent >& pathB, const ValueCache& cache_init,
    const ValueCache& cache_a, const ValueCache& cache_b,
    const std::vector< z3::expr >& clock,
    const std::map< const BmcEvent*, z3::expr >& replay_offsets, z3::model& model) {
  Counterexample evidence;
  auto& ctx = model.ctx();
  const auto nA = pathA.size(), nB = pathB.size();
  const auto targetA = candidate_index(pathA), targetB = candidate_index(pathB);
  evidence.model = model.to_string();
  ReplaySnapshot snapshot(funcA->bundle());
  evidence.replay.put("schema", 1);
  evidence.replay.put("validation_status", "UNVALIDATED");
  evidence.replay.put("program", snapshot.program());
  JsonList traces;
  auto number = [&](const z3::expr& expression) {
    return core::ZNumber::from_string(Z3_get_numeral_string(ctx, model.eval(expression, true)));
  };
  auto offset_of = [&](const BmcEvent& event) {
    auto found = replay_offsets.find(&event);
    return found != replay_offsets.end() ? number(found->second) : event.const_off;
  };
  auto read_sources = [&](const BmcEvent& read, const std::string& id, std::size_t index) {
    JsonList bytes;
    const auto offset = offset_of(read);
    for (std::uint64_t byte = 0; byte < read.width; ++byte) {
      JsonDict latest; bool written = false;
      auto position = offset + byte;
      auto collect_startup = [&](std::size_t end) {
        for (std::size_t j = 0; j < end; ++j) {
          const auto& write = startup[j];
          if (write.kind == EvKind::Write && !abstract_initializer(write.stmt) &&
              write.base == read.base && write.var_id == 0 &&
              write.const_off <= position && position < write.const_off + write.width) {
            latest = JsonDict{{"trace", "startup"}, {"step", j}}; written = true;
          }
        }
      };
      collect_startup(id == "startup" ? index : startup.size());
      if (id != "startup") {
        core::ZNumber last_time; bool has_time = false;
        const auto read_time = number(clock[(id == "a" ? 0 : nA) + index]);
        auto collect = [&](const auto& path, const std::string& name, std::size_t start) {
          for (std::size_t j = 0; j < path.size(); ++j) {
            const auto& write = path[j];
            if (write.kind != EvKind::Write || abstract_initializer(write.stmt) || write.base != read.base) continue;
            auto write_offset = offset_of(write), time = number(clock[start + j]);
            if (write_offset <= position && position < write_offset + write.width && time < read_time &&
                (!has_time || time > last_time)) {
              latest = JsonDict{{"trace", name}, {"step", j}};
              written = has_time = true; last_time = time;
            }
          }
        };
        collect(pathA, "a", 0); collect(pathB, "b", nA);
      }
      if (written) bytes.add(latest); else bytes.add("initial");
    }
    return bytes;
  };
  auto export_trace = [&](const std::vector< BmcEvent >& path, const ValueCache& values,
                          const std::string& id, ar::Function* entry, std::size_t start) {
    JsonDict trace; trace.put("id", id); trace.put("entry", entry->name()); JsonList steps;
    if (id != "startup" && entry->name() != "main") {
      const auto& creator = funcA->name() == "main" ? pathA : funcB->name() == "main" ? pathB : startup;
      std::size_t occurrence = id == "b" && funcA == funcB ? 1 : 0;
      for (std::size_t j = 0; j < creator.size(); ++j)
        if (creator[j].kind == EvKind::Create && creator[j].thread == reinterpret_cast< std::uint64_t >(entry)) {
          if (occurrence-- != 0) continue;
          trace.put("creator_trace", funcA->name() == "main" ? "a" : funcB->name() == "main" ? "b" : "startup");
          trace.put("creator_step", j); break;
        }
    }
    for (std::size_t i = 0; i < path.size(); ++i) {
      JsonDict step; step.put("statement", snapshot.statement_id(path[i].stmt));
      if (path[i].synthetic_atomic)
        step.put("synthetic_event", path[i].kind == EvKind::Lock ? "atomic_begin" : "atomic_end");
      if (path[i].synthetic_condition)
        step.put("synthetic_event", path[i].kind == EvKind::Lock ? "condition_reacquire" : "condition_release");
      if (id != "startup") step.put("clock", Z3_get_numeral_string(ctx, model.eval(clock[start + i], true)));
      if (path[i].called_function != nullptr) step.put("expanded_callee", path[i].called_function->name());
      auto* result = core::dyn_cast_or_null< ar::InternalVariable >(path[i].stmt->result_or_null());
      if (result != nullptr) {
        auto cached = values.find({result, i});
        if (cached != values.end()) step.put("encoded_result", Z3_get_numeral_string(ctx, model.eval(cached->second, true)));
      }
      const auto& e = path[i];
      if (e.kind == EvKind::Read && e.base != 0 && e.width <= 256 &&
          (e.var_id == 0 || replay_offsets.count(&e) != 0))
        step.put("read_from", read_sources(e, id, i));
      if (e.base != 0 || e.local_base != nullptr || e.heap_allocation != nullptr) {
        auto offset = replay_offsets.find(&e);
        if (offset != replay_offsets.end() || e.var_id == 0) {
          JsonDict object = e.heap_allocation != nullptr
              ? JsonDict{{"kind", "heap"}, {"allocation", snapshot.statement_id(e.heap_allocation)},
                         {"owner", "main"}, {"occurrence", 1}, {"extent", e.heap_extent.str()}}
              : snapshot.value(e.base != 0 ? static_cast< ar::Value* >(reinterpret_cast< ar::GlobalVariable* >(e.base)) : e.local_base);
          step.put("encoded_address", JsonDict{
              {"object", object},
              {"offset", offset != replay_offsets.end() ? std::string(Z3_get_numeral_string(ctx, model.eval(offset->second, true))) : e.const_off.str()},
              {"width", e.width}});
        }
      }
      steps.add(step);
    }
    trace.put("steps", steps); traces.add(trace);
  };
  if (!startup.empty()) export_trace(startup, cache_init, "startup", funcA->bundle()->function_or_null("main"), 0);
  export_trace(pathA, cache_a, "a", funcA, 0);
  export_trace(pathB, cache_b, "b", funcB, nA);
  evidence.replay.put("traces", traces);
  evidence.replay.put("targets", JsonList(JsonDict{{"trace", "a"}, {"step", targetA}},
                                             JsonDict{{"trace", "b"}, {"step", targetB}}));
  for (std::size_t i = 0; i < startup.size(); ++i)
    evidence.confirmed_startup.push_back({startup[i].stmt, "startup_" + std::to_string(i)});
  for (std::size_t i = 0; i < nA; ++i)
    evidence.confirmed_a.push_back({pathA[i].stmt, Z3_get_numeral_string(ctx, model.eval(clock[i], true))});
  for (std::size_t i = 0; i < nB; ++i)
    evidence.confirmed_b.push_back({pathB[i].stmt, Z3_get_numeral_string(ctx, model.eval(clock[nA + i], true))});
  return evidence;
}

} } }
