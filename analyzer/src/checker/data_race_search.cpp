/*******************************************************************************
 *
 * \file
 * \brief Data race checker implementation
 *
 * Author: ikos-race-detection
 *
 * Contact: ikos@lists.nasa.gov
 *
 * Notices:
 *
 * Copyright (c) 2026 United States Government as represented by the
 * Administrator of the National Aeronautics and Space Administration.
 * All Rights Reserved.
 *
 * Disclaimers:
 *
 * No Warranty: THE SUBJECT SOFTWARE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY OF
 * ANY KIND, EITHER EXPRESSED, IMPLIED, OR STATUTORY, INCLUDING, BUT NOT LIMITED
 * TO, ANY WARRANTY THAT THE SUBJECT SOFTWARE WILL CONFORM TO SPECIFICATIONS,
 * ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE,
 * OR FREEDOM FROM INFRINGEMENT, ANY WARRANTY THAT THE SUBJECT SOFTWARE WILL BE
 * ERROR FREE, OR ANY WARRANTY THAT DOCUMENTATION, IF PROVIDED, WILL CONFORM TO
 * THE SUBJECT SOFTWARE.  THIS AGREEMENT DOES NOT, IN ANY MANNER, CONSTITUTE AN
 * ENDORSEMENT BY GOVERNMENT AGENCY OR ANY PRIOR RECIPIENT OF ANY RESULTS,
 * RESULTING DESIGNS, HARDWARE, SOFTWARE PRODUCTS OR ANY OTHER APPLICATIONS
 * RESULTING FROM USE OF THE SUBJECT SOFTWARE.  FURTHER, GOVERNMENT AGENCY
 * DISCLAIMS ALL WARRANTIES AND LIABILITIES REGARDING THIRD-PARTY SOFTWARE,
 * IF PRESENT IN THE ORIGINAL SOFTWARE, AND DISTRIBUTES IT "AS IS."
 *
 * Waiver and Indemnity:  RECIPIENT AGREES TO WAIVE ANY AND ALL CLAIMS AGAINST
 * THE UNITED STATES GOVERNMENT, ITS CONTRACTORS AND SUBCONTRACTORS, AS WELL
 * AS ANY PRIOR RECIPIENT.  IF RECIPIENT'S USE OF THE SUBJECT SOFTWARE RESULTS
 * IN ANY LIABILITIES, DEMANDS, DAMAGES, EXPENSES OR LOSSES ARISING FROM SUCH
 * USE, INCLUDING ANY DAMAGES FROM PRODUCTS BASED ON, OR RESULTING FROM,
 * RECIPIENT'S USE OF THE SUBJECT SOFTWARE. RECIPIENT SHALL INDEMNIFY AND HOLD
 * HARMLESS THE UNITED STATES GOVERNMENT, ITS CONTRACTORS AND SUBCONTRACTORS,
 * AS WELL AS ANY PRIOR RECIPIENT, TO THE EXTENT PERMITTED BY LAW.
 * RECIPIENT'S SOLE REMEDY FOR ANY SUCH MATTER SHALL BE THE IMMEDIATE,
 * UNILATERAL TERMINATION OF THIS AGREEMENT.
 *
 ******************************************************************************/

#include <ikos/analyzer/checker/data_race.hpp>
#include <ikos/analyzer/checker/race_rules.hpp>
#include <ikos/analyzer/analysis/posix_semaphore.hpp>
#include <ikos/analyzer/analysis/bmc/bmc.hpp>
#include <ikos/analyzer/analysis/bmc/unroll.hpp>
#include <ikos/analyzer/support/cast.hpp>
#include <ikos/analyzer/util/log.hpp>
#include <ikos/analyzer/util/timer.hpp>
#include <ikos/analyzer/util/source_location.hpp>

#include <ikos/core/domain/concurrent_global_env.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace ikos {
namespace analyzer {

void DataRaceChecker::search_race_candidates(
    const std::vector< PendingRacePair >& all_pairs,
    const JoinedNames& joined_names) {
  if (all_pairs.empty()) {
    return;
  }

  ScopeTimerDatabase timer(this->_ctx.output_db->times,
                           "ikos-analyzer.race.bmc-search");

  // Helper: encode a single side of a race pair as a JSON dict.
  // Captured by reference into the lambda; must outlive the lambda's
  // uses during group emission below.
  auto side_info = [](const AccessRecord& rec,
                      const JsonList& pts,
                      const std::unordered_set< std::string >& joined) {
    JsonDict side;
    side.put("thread_id", rec.thread_id);
    side.put("kind", static_cast< int >(rec.kind));
    JsonList lock_list;
    for (std::uint64_t l : rec.locks) {
      lock_list.add("0x" + std::to_string(l));
    }
    side.put("locks", lock_list);
    JsonList joined_list;
    for (const std::string& t : joined) {
      joined_list.add(t);
    }
    side.put("joined_threads", joined_list);
    side.put("unique_thread", rec.is_unique_thread);
    // Source location of THIS access (for the SV-COMP violation witness:
    // the two target waypoints must carry physical file:line:column of the
    // two conflicting accesses). Omitted when the statement has no frontend.
    if (auto loc = source_location(rec.stmt)) {
      side.put("line", static_cast< int >(loc.line()));
      side.put("column", static_cast< int >(loc.column()));
      side.put("file", loc.path().filename().string());
    }
    side.put("points_to", pts);
    return side;
  };

  // Group pairs by memory_location (the physical site of the race).
  // The location name is the lexicographic key — the dump() format is
  // deterministic across runs of the same IR (memory locations are
  // addressed by their llvm.global / local slot). Using the first cell
  // of `pts` as the key keeps the grouping stable even when a single
  // pair references multiple addresses.
  std::map< std::string, std::vector< std::size_t > > groups;
  for (std::size_t k = 0; k < all_pairs.size(); ++k) {
    const std::string& key =
        all_pairs[k].first_conflict_loc.empty() ? std::string("unknown")
                                                 : all_pairs[k]
                                                       .first_conflict_loc;
    groups[key].push_back(k);
  }

  // pthread_create call sites, for the violation witness's thread
  // registration (function_enter waypoints). Emitted once and reused in
  // every group's info so downstream parsers can read it from any report.
  auto creation_info = [&](const std::vector< ar::Statement* >* selected) {
    JsonList list;
    auto append = [&](const ThreadCreation& tc) {
      JsonDict item;
      if (auto loc = source_location(tc.stmt)) {
        item.put("line", static_cast< int >(loc.line()));
        item.put("column", static_cast< int >(loc.column()));
        item.put("file", loc.path().filename().string());
      }
      item.put("thread_function", tc.child);
      ar::Code* code = tc.stmt->parent()->code();
      if (code != nullptr && code->is_function_body()) item.put("creator", code->function()->name());
      list.add(item);
    };
    if (selected == nullptr) {
      for (const auto& creation : this->_thread_creations) append(creation);
    } else {
      // Preserve execution order and dynamic repetitions, not source order.
      for (auto* stmt : *selected) {
        for (const auto& creation : this->_thread_creations) {
          if (creation.stmt == stmt) {append(creation); break;}
        }
      }
    }
    return list;
  };

  // Every concrete prefix must discharge its own synchronization semantics.

  const bool bmc_enabled = this->_ctx.opts.demote_race_to_unknown &&
                           getenv("IKOS_BMC_RECOVER") != nullptr;
  this->_search_performed = bmc_enabled && this->_ctx.opts.bmc_deadline_ms > 0;
  const bool diagnostics_enabled = getenv("IKOS_BMC_DIAGNOSTICS") != nullptr;
  bmc::SearchBudget search_budget(this->_ctx.opts.bmc_deadline_ms,
                                 this->_ctx.opts.bmc_solver_timeout_ms);
  // A may-multiplicity flag cannot establish two live concrete instances.
  // Initially support two explicit spawns on a linear main prefix, with no
  // intervening waits or unknown calls. Loop/conditional creation needs modeling.
  std::map< std::pair< std::string, std::string >, std::vector< std::vector< bmc::BmcEvent > > > activation_prefixes;
  decltype(activation_prefixes) short_activation_prefixes;
  // Move previously constructed startup choices aside when old passes reset
  // their caches. Later access selection must preserve every legacy activation
  // mode, including short startups, exact create generations and semaphores.
  decltype(activation_prefixes) later_activation_prefixes;
  std::set< std::pair< std::string, std::string > > incomplete_activations;
  std::map< std::pair< std::string, std::string >, bmc::Diagnostics > activation_diagnostics;
  auto two_live_threads = [&](const std::string& thread_a, const std::string& thread_b) {
    ar::Function* main = this->_ctx.bundle->function_or_null("main");
    if (main == nullptr || main->body_or_null() == nullptr ||
        !main->body_or_null()->has_entry_block()) {
      return false;
    }
    std::map< std::string, unsigned > required{{thread_a, 1}, {thread_b, 1}};
    if (thread_a == thread_b) {
      required[thread_a] = 2;
    }
    std::map< std::string, unsigned > sites;
    for (const ThreadCreation& tc : this->_thread_creations) {
      if (required.count(tc.child)) {
        if (tc.stmt->parent()->code() != main->body_or_null()) {
          return false;
        }
        ++sites[tc.child];
      }
    }
    if (sites != required) {
      return false;
    }
    std::set< ar::BasicBlock* > visited;
    ar::BasicBlock* bb = main->body_or_null()->entry_block();
    std::map< std::string, unsigned > spawns;
    std::vector< bmc::BmcEvent > execution;
    while (visited.insert(bb).second) {
      for (ar::Statement* stmt : *bb) {
        bmc::BmcEvent step(stmt, bmc::EvKind::Local);
        bmc::classify_event(stmt, step);
        execution.push_back(step);
        if (isa< ar::Comparison >(stmt) || isa< ar::Unreachable >(stmt)) {
          return false;
        }
        if (auto* store = dyn_cast< ar::Store >(stmt)) {
          bmc::BmcEvent event;
          bmc::classify_event(store, event);
          core::ZNumber size;
          if (event.var_id != 0 || event.width == 0 || !bmc::memory_extent(event, size)) return false;
          if (event.const_off < 0 || event.const_off + event.width > size) return false;
        }
        if (auto* call = dyn_cast< ar::CallBase >(stmt)) {
          bool creation = false;
          for (const ThreadCreation& tc : this->_thread_creations) {
            if (tc.stmt == call) {
              bmc::BmcEvent event;
              ar::Function* child = this->_ctx.bundle->function_or_null(tc.child);
              if (!bmc::classify_event(call, event) || event.kind != bmc::EvKind::Create ||
                  child == nullptr || event.thread != reinterpret_cast< std::uint64_t >(child)) return false;
              creation = true;
              if (required.count(tc.child)) {
                ++spawns[tc.child];
                if (spawns == required) {
                  if (!bmc::prefix_values_defined(execution)) return false;
                  activation_prefixes[std::minmax(thread_a, thread_b)] = {execution};
                  return true;
                }
              }
              break;
            }
          }
          if (!creation && !bmc::supported_leaf_call(call)) return false;
        } else if (!bmc::total_local_statement(stmt) && !isa< ar::Store >(stmt)) {
          return false;
        }
      }
      if (bb->num_successors() != 1) {
        return false;
      }
      bb = *bb->successor_begin();
    }
    return false;
  };
  // Reuse the same bounded AR path engine for conditional creation. Static
  // site counts are not existence evidence; the retained guards are later
  // checked together with both workers' value/scheduling constraints.
  auto conditional_live_threads = [&](const std::string& thread_a, const std::string& thread_b,
                                      unsigned loop_bound, bool extra_occurrences = false,
                                      ar::CallBase* finite_source = nullptr, unsigned finite_value = 0,
                                      bool semaphore_startup = false) {
    auto* main = this->_ctx.bundle->function_or_null("main");
    if (main == nullptr || !main->is_definition()) return false;
    std::map< std::string, unsigned > required{{thread_a, 1}, {thread_b, 1}};
    if (thread_a == thread_b) required[thread_a] = 2;
    std::map< ar::Function*, unsigned > create_limits;
    if (!extra_occurrences) {
      for (const auto& entry : required) {
        auto* function = this->_ctx.bundle->function_or_null(entry.first);
        if (function == nullptr) return false;
        create_limits.emplace(function, entry.second);
      }
    }
    auto& alternatives = activation_prefixes[std::minmax(thread_a, thread_b)];
    auto* startup_diagnostics = diagnostics_enabled
        ? &activation_diagnostics[std::minmax(thread_a, thread_b)] : nullptr;
    std::vector< std::pair< ar::Statement*, unsigned > > endpoints;
    if (semaphore_startup) {
      if (this->_ctx.posix_semaphore()) for (auto* block : *main->body()) for (auto* stmt : *block) {
        auto* call = dyn_cast< ar::CallBase >(stmt);
        auto* function = call ? bmc::exact_callee(call) : nullptr;
        if (function && function->is_declaration() && function->name() == "sem_post" &&
            this->_ctx.posix_semaphore()->effect(call)) endpoints.emplace_back(stmt, 1);
      }
    } else {
      for (const auto& creation : this->_thread_creations) {
        if (required.count(creation.child))
          endpoints.emplace_back(creation.stmt, extra_occurrences ? 2 : required.at(creation.child));
      }
    }
    for (const auto& endpoint : endpoints) {
      if (search_budget.expired()) break;
      const unsigned occurrences = endpoint.second;
      for (unsigned occurrence = 1; occurrence <= occurrences; ++occurrence) {
        if (extra_occurrences && search_budget.expired()) break;
        // Activation below rejects every join. Stop unsupported join suffixes
        // during construction instead of enumerating paths we cannot retain.
        auto result = bmc::unroll_to(
            main, endpoint.first, occurrence, loop_bound, true,
            extra_occurrences ? nullptr : &create_limits,
            &search_budget.deadline, finite_source, finite_value);
        if (result.incomplete) {
          incomplete_activations.insert(std::minmax(thread_a, thread_b));
          bmc::record(startup_diagnostics,
                      result.reason ? result.reason : "unroll_incomplete", endpoint.first);
        }
        if (result.rejected_reason)
          bmc::record(startup_diagnostics, result.rejected_reason, result.rejected_statement);
        // Exhaustion omits alternatives, not the complete prefixes already
        // emitted. Keep their target/guard/definedness/activation obligations.
        for (auto& path : result.paths) {
          if (bmc::candidate_index(path) == path.size()) continue;
          std::map< std::string, unsigned > spawns;
          std::set< std::pair< std::uint64_t, core::ZNumber > > held;
          bool supported = true;
          for (std::size_t step = 0; step < path.size(); ++step) {
            auto& event = path[step];
            if (event.kind == bmc::EvKind::Unsupported || event.kind == bmc::EvKind::Join) {supported = false; break;}
            if (event.kind == bmc::EvKind::Lock || event.kind == bmc::EvKind::Unlock) {
              bmc::BmcEvent address;
              if (event.mutex == bmc::PSEUDO_ATOMIC_LOCK) address.base = bmc::PSEUDO_ATOMIC_LOCK;
              else {
                auto* call = cast< ar::CallBase >(event.stmt);
                core::ZNumber size;
                if (call->num_arguments() != 1 ||
                    !bmc::pointer_on_path(call->argument(0), path, step, main, nullptr, address) ||
                    address.base == 0 || address.var_id != 0 || !bmc::memory_extent(address, size) ||
                    address.const_off < 0 || address.const_off >= size) {supported = false; break;}
              }
              auto identity = std::make_pair(address.base, address.const_off);
              // This is a main-only execution: all spawned children can remain
              // unscheduled until the checked, fully released startup ends.
              if (event.kind == bmc::EvKind::Lock ? !held.insert(identity).second : held.erase(identity) == 0) {
                supported = false; break;
              }
            }
            if (event.kind == bmc::EvKind::Create) {
              auto* child = reinterpret_cast< ar::Function* >(event.thread);
              if (required.count(child->name())) ++spawns[child->name()];
            }
            if (event.kind == bmc::EvKind::Read || event.kind == bmc::EvKind::Write) {
              auto* pointer = event.kind == bmc::EvKind::Read
                                  ? cast< ar::Load >(event.stmt)->operand()
                                  : cast< ar::Store >(event.stmt)->pointer();
              // Resolve each executed iteration, not the static index variable.
              // This raw startup identity is provisional: check_race clears all
              // heap fields and resolves again after canonical event expansion.
              event.base = 0; event.local_base = nullptr; event.const_off = 0; event.local_extent = 0;
              event.shared_main_local = false;
              event.heap_allocation = nullptr; event.heap_extent = 0;
              event.heap_allocation_step = 0; event.heap_occurrence = 0;
              event.var_id = 0; event.var_before = 0; event.coeff = 0;
              core::ZNumber size;
              if (!bmc::pointer_on_path(pointer, path, step, main, nullptr, event) ||
                  event.var_id != 0 || event.width == 0 || !bmc::memory_extent(event, size) ||
                  event.const_off < 0 || event.const_off + event.width > size) {supported = false; break;}
            }
          }
          bool enough = spawns == required;
          if (extra_occurrences) {
            enough = true;
            for (const auto& entry : required) enough &= spawns[entry.first] >= entry.second;
          }
          if (supported && held.empty() && enough &&
              bmc::prefix_values_defined(path, startup_diagnostics, nullptr, nullptr, nullptr, extra_occurrences)) {
            alternatives.push_back(std::move(path));
            if (alternatives.size() == 8) return true;
          }
        }
      }
    }
    return !alternatives.empty();
  };
  std::map< std::pair< std::string, std::string >, bool > instance_proofs;
  std::set< std::pair< std::string, std::string > > small_activation;

  // Preserve the original query pass before probing interrupted enumerations.
  // Deferred groups emit once after their last retry, under the same deadline.
  std::set< std::string > deferred_groups;
  std::set< std::string > larger_activation_groups;
  std::set< std::string > occurrence_groups;
  std::set< std::string > hinted_activation_groups;
  std::set< std::string > completion_groups, semaphore_groups, later_access_groups, provider_groups, provider_later_groups;
  std::map< std::string, bmc::Result > deferred_results;
  std::map< std::string, JsonList > deferred_candidates;
  std::map< std::string, JsonList > occurrence_candidates;
  std::map< std::string, std::map< std::string, std::size_t > > deferred_counts;
  bool file_confirmed = false;
  unsigned startup_hint = 16;
  for (unsigned pass = 0; pass < 10; ++pass) {
    // Try every candidate's short execution before deeper loop alternatives.
    search_budget.defer_large_loops = pass < 2;
    if (pass == 4 && !hinted_activation_groups.empty() && !file_confirmed &&
        !search_budget.expired() && search_budget.candidates != 0 && search_budget.queries != 0) {
      // Even the hint scan follows every original pass; it cannot consume
      // the budget of an already supported short activation.
      auto* main = this->_ctx.bundle->function_or_null("main");
      if (main != nullptr) startup_hint = bmc::literal_loop_search_bound(main);
    }
    if (pass >= 2) {
      // A short startup is a search choice, not an activation/safety proof for
      // other prefixes. Retry deferred groups with the original larger bound.
      instance_proofs.clear();
      // Bounded DFS need not emit a superset: keep short startup + deep worker
      // combinations when the larger startup enumeration saturates.
      if (pass == 2) {
        short_activation_prefixes = std::move(activation_prefixes);
      } else if (pass < 8) {
        for (auto& entry : activation_prefixes) {
          if (search_budget.expired()) break;
          auto& saved = later_activation_prefixes[entry.first];
          for (auto& path : entry.second) saved.push_back(std::move(path));
        }
      }
      activation_prefixes.clear();
      if (pass == 7) {
        for (auto& entry : short_activation_prefixes) {
          if (search_budget.expired()) break;
          auto& saved = later_activation_prefixes[entry.first];
          for (auto& path : entry.second) saved.push_back(std::move(path));
        }
        activation_prefixes = std::move(later_activation_prefixes);
        // Deduplicate only after old searches have run. The concrete source
        // choices and expanded callees are part of this invocation identity.
        for (auto& entry : activation_prefixes) {
          if (search_budget.expired()) break;
          std::vector< std::vector< bmc::BmcEvent > > unique;
          for (auto& path : entry.second) {
            if (search_budget.expired()) break;
            const bool duplicate = std::any_of(unique.begin(), unique.end(), [&](const auto& other) {
              if (search_budget.expired()) return false;
              return path.size() == other.size() &&
                  std::equal(path.begin(), path.end(), other.begin(), [](const auto& a, const auto& b) {
                    return a.blocker == b.blocker && a.stmt == b.stmt &&
                           a.kind == b.kind && a.called_function == b.called_function &&
                           a.synthetic_atomic == b.synthetic_atomic &&
                           a.synthetic_condition == b.synthetic_condition &&
                           a.join_completion == b.join_completion &&
                           a.join_creation == b.join_creation && a.candidate == b.candidate &&
                           a.zero_fill == b.zero_fill && a.finite_source == b.finite_source &&
                           a.joined_handle_read == b.joined_handle_read &&
                           a.finite_value == b.finite_value && a.mutex == b.mutex &&
                           a.reader == b.reader && a.local_base == b.local_base &&
                           a.local_extent == b.local_extent &&
                           a.shared_main_local == b.shared_main_local &&
                           a.single_function_execution == b.single_function_execution &&
                           a.heap_allocation == b.heap_allocation && a.heap_extent == b.heap_extent &&
                           a.heap_allocation_step == b.heap_allocation_step &&
                           a.heap_occurrence == b.heap_occurrence &&
                           a.base == b.base && a.const_off == b.const_off &&
                           a.var_id == b.var_id && a.var_before == b.var_before &&
                           a.coeff == b.coeff && a.width == b.width && a.thread == b.thread;
                  });
            });
            if (!duplicate) unique.push_back(std::move(path));
          }
          entry.second = std::move(unique);
        }
      }
      incomplete_activations.clear();
      if (pass >= 3) small_activation.clear();
    }
    if (pass != 0) {
      search_budget.probe_interrupted = true;
      if (pass < 3) {
        search_budget.candidates = 256;
        search_budget.queries = 1024;
      }
    }
    for (auto& kv : groups) {
    if ((pass == 1 && deferred_groups.count(kv.first) == 0) ||
        (pass == 2 && larger_activation_groups.count(kv.first) == 0) ||
        (pass == 3 && occurrence_groups.count(kv.first) == 0) ||
        (pass == 4 && hinted_activation_groups.count(kv.first) == 0) ||
        (pass == 5 && completion_groups.count(kv.first) == 0) ||
        (pass == 6 && semaphore_groups.count(kv.first) == 0) ||
        (pass == 7 && later_access_groups.count(kv.first) == 0) ||
        (pass == 8 && provider_groups.count(kv.first) == 0) ||
        (pass == 9 && provider_later_groups.count(kv.first) == 0)) continue;
    search_budget.saw_interrupted = false;
    const std::vector< std::size_t >& pair_indices = kv.second;
    if (pair_indices.empty()) {
      continue;
    }

    // Anchor on the FIRST pair (Write of A or B).
    const PendingRacePair& first = all_pairs[pair_indices.front()];
    const PendingRacePair* selected = &first;
    int confirmed_pair_id = -1;

    // Mixed model-boundary pairs have already blocked the task before search.
    bool group_is_unknown = true;
    for (std::size_t pk : pair_indices) {
      if (!all_pairs[pk].is_unknown) {
        group_is_unknown = false;
        break;
      }
    }

    // Build the nested list of all conflicting_accesses.
    JsonList conflicting_accesses;
    std::size_t pair_id = 0;
    for (std::size_t pk : pair_indices) {
      const PendingRacePair& pr = all_pairs[pk];
      const AccessRecord& ai = this->_accesses[pr.i];
      const AccessRecord& bj = this->_accesses[pr.j];

      JsonDict pair_dict;
      pair_dict.put("pair_id", static_cast< int >(pair_id));
      pair_dict.put("access_a", side_info(ai, pr.conflicting_pts,
                                          joined_names[pr.i]));
      pair_dict.put("access_b", side_info(bj, pr.conflicting_pts,
                                          joined_names[pr.j]));
      conflicting_accesses.add(pair_dict);
      ++pair_id;
    }

    bool bmc_confirmed = false;
    bmc::Result group_result(bmc::Status::NotSearched);
    if (pass != 0) group_result.merge(deferred_results.at(kv.first));
    std::vector< ar::Statement* > confirmed_creations;
    JsonList candidate_diagnostics;
    std::map< std::string, std::size_t > reason_counts;
    if (pass != 0 && diagnostics_enabled) reason_counts = deferred_counts.at(kv.first);
    bool group_small_activation = false;
    bool group_worker_pairs = false;
    const bool group_completion_pairs = std::any_of(pair_indices.begin(), pair_indices.end(), [&](std::size_t i) {
      const auto& pair = all_pairs[i];
      return (this->_accesses[pair.i].thread_id == "main") != (this->_accesses[pair.j].thread_id == "main");
    });
    std::vector< std::size_t > query_order;
    for (std::size_t i = 0; i < pair_indices.size(); ++i) query_order.push_back(i);
    // Legacy confirmations precede newly enabled activation alternatives
    // within the same file deadline. Preserve the report's original pair IDs.
    if (pass == 1 && !small_activation.empty()) {
      auto priority = [&](std::size_t i) {
        const auto& pair = all_pairs[pair_indices[i]];
        const auto& a = this->_accesses[pair.i].thread_id;
        const auto& b = this->_accesses[pair.j].thread_id;
        if (a == "main" || b == "main" || small_activation.count(std::minmax(a, b)) == 0) return 0;
        // Different entries may expose a protocol mismatch before spending
        // the deadline on repeated instances of one entry. This only ranks work.
        return a == b ? 2 : 1;
      };
      std::stable_sort(query_order.begin(), query_order.end(), [&](std::size_t i, std::size_t j) {
        return priority(i) < priority(j);
      });
    }
    // Held reports must still be emitted if another group succeeded or the
    // unchanged file deadline expired before the new occurrence-only pass.
    const bool final_budget_exhausted = pass >= 4 &&
        (search_budget.candidates == 0 || search_budget.queries == 0);
    if (pass >= 3 && (file_confirmed || search_budget.expired() || final_budget_exhausted ||
                      (pass == 4 && startup_hint <= 16))) {
      query_order.clear();
      if (pass == 4 && diagnostics_enabled) candidate_diagnostics = occurrence_candidates.at(kv.first);
      if (!file_confirmed && (search_budget.expired() || final_budget_exhausted))
        group_result.merge(bmc::Result(bmc::Status::BudgetExceeded));
      if (!file_confirmed) {
        if (diagnostics_enabled && search_budget.expired()) ++reason_counts["search_deadline"];
        if (diagnostics_enabled && final_budget_exhausted && search_budget.candidates == 0) ++reason_counts["candidate_budget"];
        if (diagnostics_enabled && final_budget_exhausted && search_budget.queries == 0) ++reason_counts["query_budget"];
      }
      if (diagnostics_enabled && (file_confirmed || search_budget.expired() || final_budget_exhausted))
        ++reason_counts[file_confirmed ? "not_searched_after_confirmation" : "search_budget"];
    }
    std::size_t group_attempts = 0;
    for (std::size_t candidate_id : query_order) {
      if (!bmc_enabled && !diagnostics_enabled) {
        break;
      }
      const std::size_t pk = pair_indices[candidate_id];
      const PendingRacePair& pr = all_pairs[pk];
      const AccessRecord& a = this->_accesses[pr.i];
      const AccessRecord& b = this->_accesses[pr.j];
      const bool worker_pair = a.thread_id != "main" && b.thread_id != "main";
      group_worker_pairs |= worker_pair;
      if (pass == 5 ? worker_pair : pass >= 3 && pass != 7 && !worker_pair) continue;
      bmc::Result candidate_result(bmc::Status::NotSearched);
      bmc::Diagnostics diagnostics;
      bmc::Diagnostics* trace = diagnostics_enabled ? &diagnostics : nullptr;
      bool eligible = true;
      auto gate = [&](bool blocked, const char* reason, ar::Statement* stmt = nullptr) {
        if (blocked) {
          eligible = false;
          bmc::record(trace, reason, stmt);
          candidate_result.merge(bmc::Result(bmc::Status::Unsupported));
        }
      };
      // Evaluate independent eligibility gates even when another one fails.
      if (worker_pair && pass < 8) {
        auto key = std::minmax(a.thread_id, b.thread_id);
        auto proof = instance_proofs.find(key);
        if (proof == instance_proofs.end()) {
          bool live = false;
          if (pass == 7) {
            const auto retained = activation_prefixes.find(key);
            live = retained != activation_prefixes.end() && !retained->second.empty();
          } else if (pass >= 3) {
            if (pass == 4) bmc::record(trace, "startup_literal_loop_hint");
            live = conditional_live_threads(a.thread_id, b.thread_id, pass == 4 ? startup_hint : 16, true, nullptr, 0, pass == 6);
            if (!live && (pass == 3 || pass == 6)) {
              auto* main = this->_ctx.bundle->function_or_null("main");
              auto* source = bmc::allocation_finite_source(main, 4);
              if (source != nullptr) {
                for (unsigned value = 1; value <= 4 && !search_budget.expired(); ++value) {
                  live |= conditional_live_threads(a.thread_id, b.thread_id, 16, true, source, value, pass == 6);
                  if (activation_prefixes[key].size() >= 8) break;
                }
                bmc::record(trace, "startup_finite_source");
              }
            }
          } else {
            live = two_live_threads(a.thread_id, b.thread_id);
            if (!live && pass < 2 && conditional_live_threads(a.thread_id, b.thread_id, 2)) {
              live = true;
              small_activation.insert(key);
            }
            if (!live) live = conditional_live_threads(a.thread_id, b.thread_id, 16);
          }
          if (pass == 2) {
            auto previous = short_activation_prefixes.find(key);
            if (previous != short_activation_prefixes.end() && !previous->second.empty()) {
              auto& paths = activation_prefixes[key];
              paths.insert(paths.begin(), previous->second.begin(), previous->second.end());
              live = true;
            }
          }
          proof = instance_proofs.emplace(key, live).first;
        }
        if (incomplete_activations.count(key))
          candidate_result.merge(bmc::Result(bmc::Status::BudgetExceeded));
        if ((!proof->second || incomplete_activations.count(key)) && trace)
          for (const auto& example : activation_diagnostics[key].examples)
            bmc::record(trace, "activation_" + example.reason, example.stmt);
        gate(!proof->second, "thread_activation_unproven");
        if (small_activation.count(key) != 0) {
          group_small_activation = true;
          bmc::record(trace, "activation_small_bound");
          if (pass == 0) {
            search_budget.saw_interrupted = true;
            gate(true, "activation_deferred");
          }
        }
      }
      gate(pr.is_unknown, "mixed_atomic");
      // The numeric identity tokens must not justify branch/value witnesses.
      // A future provenance model can narrow this program-wide boundary.
      // Unsupported synchronization is checked on each executed prefix.
      // Unused functions and operations after the target cannot order a race
      // already present in that finite execution.
      gate(!bmc_enabled, "recovery_disabled");

      bool same_global = false;
      bool singleton = a.pts.is_set() && a.pts.size() == 1 &&
                       b.pts.is_set() && b.pts.size() == 1;
      auto supported_objects = [](const PointsToSet& pts) {
        return pts.is_set() && !pts.is_empty() &&
               std::all_of(pts.begin(), pts.end(), [](MemoryLocation* location) {
                 return isa< GlobalMemoryLocation >(location) || isa< DynAllocMemoryLocation >(location) ||
                        isa< LocalMemoryLocation >(location);
               });
      };
      bool global_candidates = supported_objects(a.pts) && supported_objects(b.pts);
      // May-alias sets only select candidates; executed definitions and the
      // encoder's same-base/overlap checks establish the actual object.
      gate(!singleton && !global_candidates, "points_to_not_singleton");
      gate(!global_candidates, "non_global_object");
      if (singleton) {
        MemoryLocation* ma = *a.pts.begin();
        MemoryLocation* mb = *b.pts.begin();
        bool global = isa< GlobalMemoryLocation >(ma) &&
                      isa< GlobalMemoryLocation >(mb);
        same_global = global &&
                      core::IndexableTraits< MemoryLocation* >::index(ma) ==
                          core::IndexableTraits< MemoryLocation* >::index(mb);
        gate(global && !same_global, "different_global_objects");
      }
      // This is only a may-overlap prefilter; SMT must establish actual overlap.
      auto eff_interval = [](const AccessRecord& access, ZNumber& lo,
                             ZNumber& hi) {
        if (access.offset.is_bottom()) {
          return false;
        }
        if (!access.offset.is_top()) {
          lo = access.offset.lb().to_z_number();
          hi = access.offset.ub().to_z_number();
          return true;
        }
        return false;
      };
      ZNumber lo_a, hi_a, lo_b, hi_b;
      bool off_overlap = false;
      if (eff_interval(a, lo_a, hi_a) && eff_interval(b, lo_b, hi_b)) {
        if (a.width != 0 && b.width != 0) {
          off_overlap = hi_a + ZNumber(a.width) > lo_b &&
                        hi_b + ZNumber(b.width) > lo_a;
        } else {
          off_overlap = lo_a <= hi_b && lo_b <= hi_a;
        }
      }
      bool var_index = a.offset.is_top() || b.offset.is_top();
      gate(!off_overlap && !var_index, "offset_prefilter");
      ar::Function* fa = this->_ctx.bundle->function_or_null(a.thread_id);
      ar::Function* fb = this->_ctx.bundle->function_or_null(b.thread_id);
      gate(fa == nullptr || fb == nullptr, "thread_entry_missing");
      auto direct_creator = [&](const std::string& x, const std::string& y) {
        auto it = this->_thread_creators.find(y);
        return it != this->_thread_creators.end() && it->second.count(x) != 0;
      };
      auto transitive_ancestor = [&](const std::string& x, const std::string& y) {
        auto it = this->_thread_ancestors.find(y);
        return it != this->_thread_ancestors.end() && it->second.count(x) != 0;
      };
      gate((transitive_ancestor(a.thread_id, b.thread_id) &&
            !direct_creator(a.thread_id, b.thread_id)) ||
               (transitive_ancestor(b.thread_id, a.thread_id) &&
                !direct_creator(b.thread_id, a.thread_id)),
           "transitive_thread_order");
      if (eligible) {
        if (bmc_confirmed || (pass != 0 && file_confirmed)) {
          bmc::record(trace, "not_searched_after_confirmation");
        } else if (group_attempts >= 32) {
          bmc::record(trace, "group_search_budget");
          candidate_result.merge(bmc::Result(bmc::Status::BudgetExceeded));
        } else if (search_budget.candidates == 0 || search_budget.expired()) {
          search_budget.record_exhaustion(trace);
          bmc::record(trace, "search_budget");
          candidate_result.merge(bmc::Result(bmc::Status::BudgetExceeded));
        } else {
          ++group_attempts;
          // Each exact access request spends this same budget in the later
          // search. Legacy static candidate charging remains unchanged.
          if (pass != 7 && pass != 9) --search_budget.candidates;
          bmc::CandidateFacts facts;
          facts.main_argc_one = !this->_ctx.opts.argc || *this->_ctx.opts.argc == 1;
          // Keep the historical master switch; independent factors isolate
          // numerical costs from helper-publication support.
          if (this->_ctx.posix_semaphore() &&
              getenv("IKOS_BMC_DISABLE_SEMAPHORE_FACTS") == nullptr) {
            facts.semaphore_effect = [&](ar::CallBase* call) {
              return this->_ctx.posix_semaphore()->effect(call);
            };
          }
          const bool disable_ai_facts = getenv("IKOS_BMC_DISABLE_AI_FACTS") != nullptr;
          if (!disable_ai_facts && getenv("IKOS_BMC_DISABLE_AI_SINGLE_EXECUTION") == nullptr) {
            facts.single_execution = [&](ar::Function* function) {
              return this->_ctx.concurrent_env.function_executes_at_most_once(function);
            };
          }
          if (!disable_ai_facts && getenv("IKOS_BMC_DISABLE_AI_NO_WRAP") == nullptr) {
            facts.no_wrap = &this->_bmc_no_wrap;
          }
          if (!disable_ai_facts && getenv("IKOS_BMC_DISABLE_AI_BOUNDS") == nullptr) {
            facts.bounded_a = eff_interval(a, facts.lo_a, facts.hi_a);
            facts.bounded_b = eff_interval(b, facts.lo_b, facts.hi_b);
          }
          unsigned later_frontier = 0;
          bmc::LaterAccessState* later_state = nullptr;
          auto confirm = [&] {
            if (pass == 9) {
              candidate_result.merge(bmc::confirm_race_provider_later(fa, a.stmt, a.width,
                  fb, b.stmt, b.width, trace, search_budget, &confirmed_creations, &facts));
            } else if (pass == 8) {
              candidate_result.merge(bmc::confirm_race_provider(fa, a.stmt, a.width,
                  fb, b.stmt, b.width, trace, search_budget, &confirmed_creations, &facts));
            } else if (pass == 7) {
              candidate_result.merge(bmc::confirm_race_later(fa, a.stmt, a.width,
                  fb, b.stmt, b.width, trace, search_budget, &confirmed_creations, &facts,
                  later_frontier, later_state));
            } else if (pass == 5) {
              candidate_result.merge(bmc::confirm_race_completion(fa, a.stmt, a.width,
                  fb, b.stmt, b.width, trace, search_budget, &confirmed_creations, &facts));
            } else {
              candidate_result.merge(bmc::confirm_race(fa, a.stmt, a.width,
                  fb, b.stmt, b.width, trace, &search_budget, &confirmed_creations, &facts));
            }
            return race_rules::permits_execution_premise(race_rules::Id::BmcSelectedExecution) &&
                   candidate_result.confirmed();
          };
          if (worker_pair && pass < 8) {
            const auto& startups = activation_prefixes.at(std::minmax(a.thread_id, b.thread_id));
            if (pass < 3) {
              for (const auto& startup : startups) {
                facts.startup = startup;
                if ((bmc_confirmed = confirm())) {
                  confirmed_creations.clear();
                  for (const auto& event : startup)
                    if (event.kind == bmc::EvKind::Create) confirmed_creations.push_back(event.stmt);
                  break;
                }
              }
            } else {
              struct Selection {
                std::size_t rank;
                const std::vector< bmc::BmcEvent >* startup;
                std::size_t a, b;
              };
              std::vector< Selection > selections;
              bool more_occurrences = false;
              for (const auto& startup : startups) {
                // The cache key is unordered; count matching entries anew so
                // candidate A/B orientation cannot swap the selected instances.
                auto count = [&](ar::Function* entry) {
                  return static_cast< std::size_t >(std::count_if(startup.begin(), startup.end(), [&](const auto& event) {
                    return event.kind == bmc::EvKind::Create &&
                           event.thread == reinterpret_cast< std::uint64_t >(entry);
                  }));
                };
                const auto count_a = count(fa), count_b = count(fb);
                more_occurrences |= count_a * count_b - (fa == fb ? count_a : 0) > 32;
                std::size_t rank = 0;
                for (std::size_t ia = 0; ia < count_a && rank < 32; ++ia) {
                  for (std::size_t ib = 0; ib < count_b && rank < 32; ++ib) {
                    if (fa == fb && ia == ib) continue;
                    selections.push_back({rank++, &startup, ia, ib});
                  }
                }
              }
              // Give every retained startup its first choice before spending
              // the same selection budget on another choice of an earlier one.
              std::stable_sort(selections.begin(), selections.end(), [](const auto& x, const auto& y) {
                return x.rank < y.rank;
              });
              facts.selected_worker_occurrences = true;
              std::size_t occurrence_attempts = 0;
              std::vector< bmc::LaterAccessState > later_states;
              if (pass == 7) later_states.resize(std::min< std::size_t >(32, selections.size()));
              unsigned frontier = 2;
              bool pending = true;
              do {
                occurrence_attempts = 0;
                pending = false;
                for (const auto& selection : selections) {
                  if (occurrence_attempts >= 32 || search_budget.expired() || search_budget.queries == 0 ||
                      (pass == 7 && search_budget.candidates == 0)) break;
                  if (pass == 7) {
                    later_state = &later_states[occurrence_attempts];
                    later_frontier = frontier;
                    ++occurrence_attempts;
                    if (!later_state->pending(frontier)) continue;
                    pending = true;
                  } else ++occurrence_attempts;
                  facts.startup = *selection.startup;
                  facts.worker_a_occurrence = selection.a;
                  facts.worker_b_occurrence = selection.b;
                  bmc::record(trace, "selected_activation_occurrences");
                  if ((bmc_confirmed = confirm())) {
                    confirmed_creations.clear();
                    for (const auto& event : *selection.startup)
                      if (event.kind == bmc::EvKind::Create) confirmed_creations.push_back(event.stmt);
                    break;
                  }
                }
                // Give every retained create selection the same access frontier
                // before one disjoint instance can consume all later requests.
              } while (pass == 7 && pending && !bmc_confirmed &&
                       !search_budget.expired() && search_budget.candidates != 0 &&
                       search_budget.queries != 0 && ++frontier <= bmc::MAX_PATH_EVENTS);
              if (!bmc_confirmed && (occurrence_attempts < selections.size() || more_occurrences)) {
                bmc::record(trace, occurrence_attempts >= 32 ? "activation_selection_budget" : "search_budget");
                candidate_result.merge(bmc::Result(bmc::Status::BudgetExceeded));
              }
            }
          } else bmc_confirmed = confirm();
          if (bmc_confirmed) {
            selected = &pr;
            confirmed_pair_id = static_cast< int >(candidate_id);
          }
        }
      }
      group_result.merge(candidate_result);
      if (diagnostics_enabled) {
        const auto& evidence = candidate_result.evidence;
        JsonDict counts;
        for (const auto& entry : diagnostics.counts) {
          counts.put(entry.first, entry.second);
          reason_counts[entry.first] += entry.second;
        }
        // Keep examples bounded; aggregate counters still cover every pair.
        if (candidate_id < 256) {
          JsonList examples;
          for (const auto& example : diagnostics.examples) {
            JsonDict d;
            d.put("reason", example.reason);
            std::ostringstream ar_statement;
            example.stmt->dump(ar_statement);
            d.put("ar", ar_statement.str());
            if (auto loc = source_location(example.stmt)) {
              d.put("file", loc.path().string());
              d.put("line", static_cast< int >(loc.line()));
              d.put("column", static_cast< int >(loc.column()));
            }
            examples.add(d);
          }
          JsonDict d;
          d.put("pair_id", candidate_id);
          d.put("result", candidate_result.summary());
          d.put("first_blocker", diagnostics.first_blocker);
          d.put("counts", counts);
          d.put("paths_a", diagnostics.paths_a);
          d.put("paths_b", diagnostics.paths_b);
          d.put("path_pairs", diagnostics.path_pairs);
          d.put("examples", examples);
          if (!evidence.model.empty()) {
            auto path_info = [](const std::vector< bmc::Counterexample::Step >& steps) {
              JsonList path;
              for (const auto& step : steps) {
                JsonDict item;
                std::ostringstream statement;
                step.stmt->dump(statement);
                item.put("ar", statement.str());
                item.put("clock", step.clock);
                if (auto loc = source_location(step.stmt)) {
                  item.put("file", loc.path().string());
                  item.put("line", static_cast< int >(loc.line()));
                  item.put("column", static_cast< int >(loc.column()));
                }
                path.add(item);
              }
              return path;
            };
            d.put("confirmed_path_a", path_info(evidence.confirmed_a));
            d.put("confirmed_path_b", path_info(evidence.confirmed_b));
            d.put("confirmed_startup", path_info(evidence.confirmed_startup));
            d.put("smt_model", evidence.model);
            d.put("replay", evidence.replay);
          }
          candidate_diagnostics.add(d);
        }
      }
    }

    file_confirmed |= bmc_confirmed;
    const bool legacy_deferred = (pass == 0 && search_budget.saw_interrupted) ||
                                 (pass == 1 && (group_small_activation || search_budget.saw_interrupted) && !file_confirmed);
    const bool occurrence_deferred = pass < 3 && bmc_enabled && group_worker_pairs && !file_confirmed;
    // Hold the last unconfirmed worker reports until every original pass has
    // finished. With no larger hint the final pass only emits these results.
    const bool hint_deferred = pass == 3 && bmc_enabled && group_worker_pairs && !file_confirmed;
    // New completion searches follow all legacy passes in this same file budget.
    const bool completion_deferred = pass < 5 && bmc_enabled && group_completion_pairs && group_result.needs_completion && !file_confirmed;
    if (group_worker_pairs && this->_ctx.posix_semaphore() && this->_ctx.posix_semaphore()->has_effects())
      semaphore_groups.insert(kv.first);
    const bool semaphore_deferred = pass < 6 && bmc_enabled &&
        semaphore_groups.count(kv.first) != 0 && !file_confirmed;
    // Keep every still-inconclusive report until the additional exact-access
    // pass, including main/worker pairs with joins only after a later target.
    const bool later_access_deferred = pass < 7 && bmc_enabled && !file_confirmed;
    // These final worker-pair passes construct timed main/provider executions;
    // failure of the old fixed-startup proof is not a safety fact. Other gates,
    // global coverage and the execution-permission rule remain unchanged.
    const bool provider_deferred = pass < 8 && bmc_enabled && group_worker_pairs && !file_confirmed;
    // Exact provider access pairs reuse the same shell scheduler only after
    // all first-access provider groups. No new budget/deadline is created.
    const bool provider_later_deferred = pass < 9 && bmc_enabled && group_worker_pairs && !file_confirmed;
    if (!bmc_confirmed && (legacy_deferred || occurrence_deferred || hint_deferred ||
                          completion_deferred || semaphore_deferred || later_access_deferred ||
                          provider_deferred || provider_later_deferred)) {
      deferred_results[kv.first] = group_result;
      if (legacy_deferred) {
        if (pass == 0) deferred_groups.insert(kv.first);
        else larger_activation_groups.insert(kv.first);
      } else if (hint_deferred) hinted_activation_groups.insert(kv.first);
      else if (occurrence_deferred) occurrence_groups.insert(kv.first);
      if (completion_deferred) completion_groups.insert(kv.first);
      if (later_access_deferred) later_access_groups.insert(kv.first);
      if (provider_deferred) provider_groups.insert(kv.first);
      if (provider_later_deferred) provider_later_groups.insert(kv.first);
      if (diagnostics_enabled) {
        if (pass == 0) deferred_candidates.emplace(kv.first, candidate_diagnostics);
        if (pass == 3) occurrence_candidates[kv.first] = candidate_diagnostics;
        deferred_counts[kv.first] = reason_counts;
      }
      continue;
    }
    completion_groups.erase(kv.first);
    semaphore_groups.erase(kv.first);
    later_access_groups.erase(kv.first);
    provider_groups.erase(kv.first);
    provider_later_groups.erase(kv.first);
    // Flat targets and the report anchor must refer to the confirmed pair.
    JsonDict info{
        {"memory_location", first.first_conflict_loc.empty()
                                ? std::string("unknown") : first.first_conflict_loc},
        {"access_a", side_info(this->_accesses[selected->i],
                               selected->conflicting_pts, joined_names[selected->i])},
        {"access_b", side_info(this->_accesses[selected->j],
                               selected->conflicting_pts, joined_names[selected->j])},
        {"conflicting_accesses", conflicting_accesses},
        {"num_pairs", static_cast< int >(pair_indices.size())},
        {"unique_lockset_intersection", "empty"},
    };
    info.put("thread_creations", creation_info(bmc_confirmed ? &confirmed_creations : nullptr));
    if (bmc_confirmed) {
      info.put("confirmed_pair_id", confirmed_pair_id);
      info.put("counterexample", group_result.evidence.replay);
    }
    info.put("bmc_result", group_result.summary());
    if (group_is_unknown) {
      info.put("verdict", std::string("unknown"));
      info.put("unknown_reason", std::string("mixed atomic/non-atomic accesses"));
    }
    if (diagnostics_enabled) {
      JsonDict diagnostics, counts;
      for (const auto& entry : reason_counts) {
        counts.put(entry.first, entry.second);
      }
      diagnostics.put("counts", counts);
      diagnostics.put("budget", JsonDict{{"deadline_ms", search_budget.deadline_ms},
                                        {"solver_timeout_ms", search_budget.solver_timeout_ms},
                                        {"remaining_candidates", search_budget.candidates},
                                        {"remaining_queries", search_budget.queries}});
      diagnostics.put("candidates", candidate_diagnostics);
      if (pass != 0) diagnostics.put("first_pass_candidates", deferred_candidates.at(kv.first));
      if (pass == 4 && startup_hint > 16 && !query_order.empty())
        diagnostics.put("previous_activation_candidates", occurrence_candidates.at(kv.first));
      diagnostics.put("omitted_candidates", pair_indices.size() > 256 ? pair_indices.size() - 256 : 0);
      info.put("bmc", diagnostics);
    }

    this->report_race_group(*selected, group_is_unknown, bmc_confirmed, info);
    // No terminal output: the aggregated info is the report. Downstream
    // tools (ikos-report, ikos-view) render it from the database.
  }
  }}

} // namespace analyzer
} // namespace ikos
