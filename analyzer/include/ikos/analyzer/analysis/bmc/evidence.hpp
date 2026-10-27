#pragma once

#include "result.hpp"
#include "function_values.hpp"
#include "facts.hpp"
#include "replay.hpp"

namespace ikos { namespace analyzer { namespace bmc {

// Export a selected model only. Instruction semantics come from ReplaySnapshot;
// the certificate contains occurrences and values, not trusted transfer rules.
template < typename ValueCache >
Counterexample export_counterexample(const std::vector< ExecutionTrace >& traces,
    const std::vector< ValueCache >& caches,
    const std::vector< z3::expr >& clock,
    const std::map< const BmcEvent*, z3::expr >& replay_offsets, z3::model& model,
    const FunctionValues* functions = nullptr,
    const std::map< const BmcEvent*, BmcEvent >* object_pointer_reads = nullptr) {
  Counterexample evidence;
  auto& ctx = model.ctx();
  const auto& startup = traces[0].events;
  const auto& pathA = traces[1].events;
  const auto& pathB = traces[2].events;
  const auto nA = pathA.size(), nB = pathB.size();
  const auto targetA = candidate_index(pathA), targetB = candidate_index(pathB);
  evidence.model = model.to_string();
  ReplaySnapshot snapshot(traces[1].root->bundle());
  evidence.replay.put("schema", 1);
  evidence.replay.put("validation_status", "UNVALIDATED");
  for (const auto& trace : traces) for (const auto& event : trace.events)
    if (event.recursive_profile || event.recursive_mutex) evidence.replay.put("mutex_profile", "gnu-x86-recursive-v1");
  evidence.replay.put("program", snapshot.program());
  JsonList exported_traces;
  auto number = [&](const z3::expr& expression) {
    return core::ZNumber::from_string(Z3_get_numeral_string(ctx, model.eval(expression, true)));
  };
  auto offset_of = [&](const BmcEvent& event) {
    auto found = replay_offsets.find(&event);
    return found != replay_offsets.end() ? number(found->second) : event.const_off;
  };
  const auto main_index = traces[1].root->name() == "main" ? 1 :
                          traces[2].root->name() == "main" ? 2 : 0;
  auto heap_object = [&](const BmcEvent& pointer) {
    return JsonDict{{"kind", "heap"},
                    {"allocation", snapshot.statement_id(pointer.heap_allocation)},
                    {"owner", "main"}, {"occurrence", pointer.heap_occurrence},
                    {"allocation_event", JsonDict{{"trace", traces[main_index].id},
                                                  {"step", pointer.heap_allocation_step}}},
                    {"extent", pointer.heap_extent.str()}};
  };
  auto read_sources = [&](const BmcEvent& read, std::size_t trace_index, std::size_t index) {
    JsonList bytes;
    const auto offset = offset_of(read);
    auto same_object = [&](const BmcEvent& write, std::size_t writer_trace) {
      if (read.base != 0) return read.base == write.base;
      if (read.heap_allocation != nullptr) return same_heap_object(read, write);
      return read.local_base != nullptr && read.local_base == write.local_base &&
             (trace_index == writer_trace || (read.shared_main_local && write.shared_main_local));
    };
    for (std::uint64_t byte = 0; byte < read.width; ++byte) {
      JsonDict latest; bool written = false;
      auto position = offset + byte;
      auto collect_startup = [&](std::size_t end) {
        for (std::size_t j = 0; j < end; ++j) {
          const auto& write = startup[j];
          if ((write.kind == EvKind::Write || write.zero_fill) && !abstract_initializer(write.stmt) &&
              same_object(write, 0) && write.var_id == 0 &&
              write.const_off <= position && position < write.const_off + write.width) {
            latest = JsonDict{{"trace", "startup"}, {"step", j}}; written = true;
          }
        }
      };
      collect_startup(trace_index == 0 ? index : startup.size());
      if (trace_index != 0) {
        core::ZNumber last_time; bool has_time = false;
        const auto read_time = number(clock[traces[trace_index].clock_start + index]);
        auto collect = [&](const auto& path, const std::string& name, std::size_t start, std::size_t writer_trace) {
          for (std::size_t j = 0; j < path.size(); ++j) {
            const auto& write = path[j];
            const bool handle_source = read.joined_handle_read && writer_trace == trace_index &&
                                       write.kind == EvKind::Create;
            if ((write.kind != EvKind::Write && !write.zero_fill && !handle_source) || abstract_initializer(write.stmt) ||
                !same_object(write, writer_trace)) continue;
            auto write_offset = offset_of(write), time = number(clock[start + j]);
            if (write_offset <= position && position < write_offset + write.width && time < read_time &&
                (!has_time || time > last_time)) {
              latest = JsonDict{{"trace", name}, {"step", j}};
              written = has_time = true; last_time = time;
            }
          }
        };
        for (std::size_t k = 1; k < traces.size(); ++k)
          collect(traces[k].events, traces[k].id, traces[k].clock_start, k);
      }
      if (written) bytes.add(latest); else bytes.add("initial");
    }
    return bytes;
  };
  auto export_trace = [&](std::size_t trace_index) {
    const auto& selected = traces[trace_index];
    const auto& path = selected.events;
    const auto& values = caches[trace_index];
    const auto& id = selected.id;
    auto* entry = selected.root;
    const auto start = selected.clock_start;
    JsonDict trace; trace.put("id", id); trace.put("entry", entry->name()); JsonList steps;
    if (id != "startup" && entry->name() != "main") {
      if (selected.creator_step != ExecutionTrace::no_creation) {
        trace.put("creator_trace", traces[selected.creator_path].id);
        trace.put("creator_step", selected.creator_step);
      }
    }
    for (std::size_t i = 0; i < path.size(); ++i) {
      JsonDict step; step.put("statement", snapshot.statement_id(path[i].stmt));
      if (path[i].synthetic_barrier) step.put("synthetic_event", "barrier_departure");
      if (path[i].synthetic_atomic)
        step.put("synthetic_event", path[i].kind == EvKind::Lock ? "atomic_begin" : "atomic_end");
      if (path[i].synthetic_condition)
        step.put("synthetic_event", path[i].kind == EvKind::Lock ? "condition_reacquire" : "condition_release");
      if (id != "startup") step.put("clock", Z3_get_numeral_string(ctx, model.eval(clock[start + i], true)));
      if (path[i].mutex_result_known) {
        step.put("mutex_semantics", JsonDict{{"result", path[i].mutex_result},
            {"recursive", path[i].recursive_mutex},
            {"depth_before", path[i].recursive_before}, {"depth_after", path[i].recursive_after}});
      }
      if (path[i].join_completion != nullptr) {
        bool retained = false;
        for (const auto& child : traces) {
          if (child.creator_path != trace_index || child.creator_step != path[i].join_creation || child.events.empty()) continue;
          step.put("joined_trace", JsonDict{{"trace", child.id}, {"step", child.events.size() - 1}});
          retained = true; break;
        }
        if (!retained)
          step.put("joined_completion", JsonDict{{"creator_trace", id},
              {"creator_step", path[i].join_creation},
              {"statement", snapshot.statement_id(path[i].join_completion)}});
      }
      if (path[i].once_stage != OnceStage::None) {
        step.put("once_call_step", path[i].once_origin);
        if (path[i].once_stage == OnceStage::Call) step.put("once_role", path[i].once_active ? "active" : "passive");
        if (path[i].once_stage == OnceStage::Enter) step.put("synthetic_event", "once_enter");
        if (path[i].once_stage == OnceStage::CallbackReturn) step.put("once_callback_return", true);
        if (path[i].once_stage == OnceStage::Return) step.put("synthetic_event", "once_return");
      }
      if (path[i].called_function != nullptr) step.put("expanded_callee", path[i].called_function->name());
      auto* result = core::dyn_cast_or_null< ar::InternalVariable >(path[i].stmt->result_or_null());
      if (result != nullptr) {
        auto cached = values.find({result, i});
        if (cached != values.end()) step.put("encoded_result", Z3_get_numeral_string(ctx, model.eval(cached->second, true)));
      }
      if (result != nullptr && functions != nullptr) {
        const auto& values = functions->caches[trace_index];
        auto found = values.find({result, i});
        if (found != values.end()) step.put("encoded_pointer", functions->identity(found->second, model));
      }
      const auto& e = path[i];
      if (object_pointer_reads != nullptr) {
        auto found = object_pointer_reads->find(&e);
        if (found != object_pointer_reads->end()) {
          const auto& pointer = found->second;
          step.put("encoded_object_pointer", JsonDict{
              {"object", heap_object(pointer)},
              {"offset", pointer.const_off.str()}, {"width", 0}});
        }
      }
      if (e.kind == EvKind::Read && (e.base != 0 || e.heap_allocation != nullptr || e.shared_main_local ||
          (e.local_base != nullptr && native_sc_source_descriptor(e.stmt))) && e.width <= 256 &&
          (e.var_id == 0 || replay_offsets.count(&e) != 0))
        step.put("read_from", read_sources(e, trace_index, i));
      if (e.base != 0 || e.local_base != nullptr || e.heap_allocation != nullptr) {
        auto offset = replay_offsets.find(&e);
        if (offset != replay_offsets.end() || e.var_id == 0) {
          JsonDict object = e.heap_allocation != nullptr
              ? heap_object(e)
              : snapshot.value(e.base != 0 ? static_cast< ar::Value* >(reinterpret_cast< ar::GlobalVariable* >(e.base)) : e.local_base);
          JsonDict encoded_address{
              {"object", object},
              {"offset", offset != replay_offsets.end() ? std::string(Z3_get_numeral_string(ctx, model.eval(offset->second, true))) : e.const_off.str()},
              {"width", e.width}};
          if (e.kind == EvKind::Create) {
            auto* call = core::cast< ar::CallBase >(e.stmt);
            auto* type = core::cast< ar::PointerType >(call->argument(0)->type());
            encoded_address.put("alignment", call->bundle()->data_layout().abi_alignment(type->pointee()).to< std::uint64_t >());
          }
          step.put("encoded_address", encoded_address);
        }
      }
      steps.add(step);
    }
    trace.put("steps", steps); exported_traces.add(trace);
  };
  for (std::size_t k = 0; k < traces.size(); ++k)
    if (k != 0 || !startup.empty()) export_trace(k);
  evidence.replay.put("traces", exported_traces);
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
