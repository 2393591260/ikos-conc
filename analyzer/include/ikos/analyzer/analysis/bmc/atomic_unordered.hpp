// Complete occurrence graph for the optional verifier-atomic unordered query.
// This is a sufficient race-query condition, never a SAFE or source-HB fact.
#pragma once

#include <cstdlib>
#include <map>
#include <string>
#include <set>
#include <utility>
#include <vector>
#include <z3++.h>
#include "traces.hpp"

namespace ikos { namespace analyzer { namespace bmc {

struct AtomicUnorderedSection {
  std::size_t trace, begin, end;
};

class AtomicUnorderedQuery {
  struct OrderedSections {
    std::size_t from, to, end_clock, begin_clock;
  };
  bool _valid = false;
  std::size_t _nodes = 0, _target_a = 0, _target_b = 0;
  std::vector< std::pair< std::size_t, std::size_t > > _edges;
  std::vector< OrderedSections > _sections;
  std::vector< std::pair< std::size_t, std::size_t > > _schedule_edges;

public:
  AtomicUnorderedQuery() = default;
  bool valid() const { return _valid; }

  // The source/effect and value checks in check_race remain mandatory. This
  // additional closed inventory accounts for every synchronization occurrence
  // in the admitted graph; an unsupported event cannot silently lose an edge.
  AtomicUnorderedQuery(
      const std::vector< ExecutionTrace >& traces,
      std::size_t main_index,
      const std::map< std::size_t, std::size_t >* joins,
      bool provider,
      const std::vector< AtomicUnorderedSection >& sections,
      std::size_t target_a, std::size_t target_b) {
    if (std::getenv("IKOS_BMC_DISABLE_ATOMIC_UNORDERED") || provider ||
        traces.size() < 3 || main_index >= traces.size() ||
        !traces[main_index].root || traces[main_index].root->name() != "main" ||
        target_a >= traces[1].events.size() || target_b >= traces[2].events.size() ||
        sections.empty() || sections.size() > 64) return;
    if (main_index != 0 && !traces[0].events.empty()) return;
    std::vector< std::size_t > starts;
    for (const auto& trace : traces) {
      starts.push_back(_nodes);
      _nodes += trace.events.size();
      // Structural refusal only; no larger search or solver budget.
      if (_nodes > 4096) return;
    }
    auto node = [&](std::size_t id, std::size_t step) {
      return starts[id] + step;
    };
    _target_a = node(1, target_a); _target_b = node(2, target_b);
    std::set< std::pair< std::size_t, std::size_t > > creations;
    std::vector< std::vector< std::size_t > > call_returns(traces.size());
    for (std::size_t id = 0; id < traces.size(); ++id) {
      const auto& trace = traces[id];
      const auto& path = trace.events;
      if (id != 0 && (!trace.root || !completion_return(path, trace.root))) return;
      if (id == 0 && completion_return(path, trace.root)) return;
      if (id != main_index && id != 0) {
        ThreadArgument actual;
        if (trace.creator_path != main_index ||
            !creations.emplace(trace.creator_path, trace.creator_step).second ||
            !bind_creation_step(trace.root, traces[main_index].events,
                                trace.creator_step, actual) ||
            actual.value != trace.argument.value ||
            (trace.argument.exact_creation
                ? actual.creation_ordinal != trace.argument.creation_ordinal
                : !core::isa< ar::NullConstant >(actual.value))) return;
        _edges.emplace_back(node(main_index, trace.creator_step), node(id, 0));
        if (main_index != 0) {
          _schedule_edges.emplace_back(
              traces[main_index].clock_start + trace.creator_step, trace.clock_start);
          // A completed implicit main ends the process. Finishing a child
          // before it is a schedule restriction, NOT a child-to-main HB edge.
          _schedule_edges.emplace_back(
              trace.clock_start + path.size() - 1,
              traces[main_index].clock_start + traces[main_index].events.size() - 1);
        }
      }
      // One linear stack binds each helper Return to the actual call occurrence.
      // An equally named call elsewhere cannot discharge this invocation.
      std::vector< std::pair< ar::Code*, std::size_t > > calls;
      call_returns[id].assign(path.size(), path.size());
      for (std::size_t i = 0; i < path.size(); ++i) {
        const auto& event = path[i];
        if (!event.stmt || event.once_stage != OnceStage::None ||
            event.synthetic_condition || event.synthetic_barrier ||
            event.zero_fill) return;
        switch (event.kind) {
          case EvKind::Read: case EvKind::Write: case EvKind::Assume:
          case EvKind::Create: case EvKind::Join: case EvKind::Local:
            if (event.synthetic_atomic) return;
            break;
          case EvKind::Lock: case EvKind::Unlock:
            if (event.mutex != PSEUDO_ATOMIC_LOCK || event.reader) return;
            break;
          default: return; // Including every future synchronization event kind.
        }
        if (i != 0) _edges.emplace_back(node(id, i - 1), node(id, i));
        if ((event.kind == EvKind::Lock || event.kind == EvKind::Unlock) &&
            event.mutex != PSEUDO_ATOMIC_LOCK) return;
        if (auto* load = core::dyn_cast< ar::Load >(event.stmt))
          if (load->is_atomic()) return;
        if (auto* store = core::dyn_cast< ar::Store >(event.stmt))
          if (store->is_atomic()) return;
        if (event.kind == EvKind::Create) {
          if (id != main_index) return;
        }
        if (event.kind == EvKind::Join) {
          if (id != main_index || joins == nullptr) return;
          auto found = joins->find(i);
          if (found == joins->end() || found->second == 0 ||
              found->second >= traces.size() || found->second == main_index) return;
          const auto& child = traces[found->second];
          auto* returned = completion_return(child.events, child.root);
          if (!returned || event.join_completion != returned ||
              child.creator_path != id || event.join_creation != child.creator_step) return;
          _edges.emplace_back(node(found->second, child.events.size() - 1), node(id, i));
        }
        if (auto* ret = core::dyn_cast< ar::ReturnValue >(event.stmt)) {
          if (!calls.empty() && calls.back().first == ret->code()) {
            call_returns[id][calls.back().second] = i;
            calls.pop_back();
          } else if (!calls.empty() || !trace.root || id == 0 ||
                     ret->code() != trace.root->body_or_null() || i + 1 != path.size()) return;
        }
        auto* call = core::dyn_cast< ar::CallBase >(event.stmt);
        if (!call || event.synthetic_atomic || abstract_initializer(event.stmt)) continue;
        if (event.kind == EvKind::Create || event.kind == EvKind::Join ||
            source_nondet(call) || is_assume_call(call) || ffs_call(call)) continue;
        auto* exact = exact_callee(call);
        if (is_verifier_atomic(call) && exact && exact->is_declaration()) continue;
        auto* callee = event.called_function;
        if (!callee || !callee->is_definition() ||
            (exact && exact != callee)) return;
        calls.emplace_back(callee->body_or_null(), i);
      }
      if (!calls.empty()) return;
    }
    if (joins)
      for (const auto& join : *joins)
        if (join.first >= traces[main_index].events.size() ||
            traces[main_index].events[join.first].kind != EvKind::Join) return;
    std::set< std::pair< std::size_t, std::size_t > > boundaries;
    for (const auto& section : sections) {
      if (section.trace >= traces.size() ||
          section.begin >= section.end ||
          section.end >= traces[section.trace].events.size()) return;
      const auto& path = traces[section.trace].events;
      if (path[section.begin].kind != EvKind::Lock ||
          path[section.end].kind != EvKind::Unlock ||
          path[section.begin].mutex != PSEUDO_ATOMIC_LOCK ||
          path[section.end].mutex != PSEUDO_ATOMIC_LOCK ||
          !boundaries.emplace(section.trace, section.begin).second ||
          !boundaries.emplace(section.trace, section.end).second) return;
      const auto& begin = path[section.begin]; const auto& end = path[section.end];
      if (begin.synthetic_atomic || end.synthetic_atomic) {
        if (!begin.synthetic_atomic || !end.synthetic_atomic || section.begin == 0 ||
            begin.stmt != end.stmt || path[section.begin - 1].stmt != begin.stmt ||
            !named_verifier_atomic(path[section.begin - 1].called_function) ||
            call_returns[section.trace][section.begin - 1] != section.end - 1) return;
      } else {
        auto* begin_call = core::dyn_cast< ar::CallBase >(begin.stmt);
        auto* end_call = core::dyn_cast< ar::CallBase >(end.stmt);
        if (!begin_call || !end_call || !is_verifier_atomic(begin_call) ||
            !is_verifier_atomic(end_call) ||
            exact_callee(begin_call)->name() != "__VERIFIER_atomic_begin" ||
            exact_callee(end_call)->name() != "__VERIFIER_atomic_end") return;
      }
    }
    for (std::size_t id = 0; id < traces.size(); ++id)
      for (std::size_t i = 0; i < traces[id].events.size(); ++i)
        if (traces[id].events[i].mutex == PSEUDO_ATOMIC_LOCK &&
            !boundaries.count({id, i})) return;
    // Conservative verifier-atomic order, identical in reachability to the
    // replay's consecutive-section chain. Ordinary memory rf adds NO HB edge.
    // Fixed startup happens before every timed trace in the existing profile.
    for (const auto& before : sections) for (const auto& after : sections) {
      if (before.trace == after.trace) continue; // already PO
      if (after.trace == 0) continue;
      if (before.trace == 0)
        _edges.emplace_back(node(0, before.end), node(after.trace, after.begin));
      else
        _sections.push_back({node(before.trace, before.end), node(after.trace, after.begin),
                             traces[before.trace].clock_start + before.end,
                             traces[after.trace].clock_start + after.begin});
    }
    _valid = true;
  }

  // Two closed sets prove mutual unreachability: each contains its own target,
  // is closed under every enabled edge, and excludes the opposite target.
  // Free Boolean values cannot hide a path; every actual edge propagates true.
  void constrain(z3::context& ctx, z3::solver& query,
                 const std::vector< z3::expr >& clock) const {
    ikos_assert(_valid);
    std::vector< z3::expr > a, b;
    a.reserve(_nodes); b.reserve(_nodes);
    for (std::size_t i = 0; i < _nodes; ++i) {
      a.push_back(ctx.bool_const(("atomic_reach_a_" + std::to_string(i)).c_str()));
      b.push_back(ctx.bool_const(("atomic_reach_b_" + std::to_string(i)).c_str()));
    }
    query.add(a[_target_a] && b[_target_b] && !a[_target_b] && !b[_target_a]);
    for (const auto& edge : _edges) {
      query.add(z3::implies(a[edge.first], a[edge.second]));
      query.add(z3::implies(b[edge.first], b[edge.second]));
    }
    for (const auto& edge : _schedule_edges)
      query.add(clock[edge.first] < clock[edge.second]);
    for (const auto& edge : _sections) {
      const auto enabled = clock[edge.end_clock] < clock[edge.begin_clock];
      query.add(z3::implies(enabled && a[edge.from], a[edge.to]));
      query.add(z3::implies(enabled && b[edge.from], b[edge.to]));
    }
    // Exported event order cannot depend on a cross-thread synchronization tie.
    z3::expr_vector strict(ctx);
    for (const auto& value : clock) strict.push_back(value);
    if (strict.size() > 1) query.add(z3::distinct(strict));
  }
};

} } } // namespace ikos::analyzer::bmc
