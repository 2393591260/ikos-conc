#pragma once

#include <set>
#include <cstdint>
#include <string>
#include <vector>
#include <ikos/ar/semantic/statement.hpp>
#include <ikos/analyzer/json/json.hpp>

namespace ikos { namespace analyzer { namespace bmc {

// None of these unsuccessful outcomes is an unbounded safety proof.
enum class Status { NotSearched, BoundedUnsat, SolverUnknown, Unsupported,
                    BudgetExceeded, Confirmed };

inline const char* status_name(Status status) {
  switch (status) {
    case Status::NotSearched: return "NOT_SEARCHED";
    case Status::BoundedUnsat: return "BOUNDED_UNSAT";
    case Status::SolverUnknown: return "SOLVER_UNKNOWN";
    case Status::Unsupported: return "UNSUPPORTED";
    case Status::BudgetExceeded: return "BUDGET_EXCEEDED";
    case Status::Confirmed: return "CONFIRMED";
  }
  ikos_unreachable("invalid BMC result");
}

struct Counterexample {
  struct Step { ar::Statement* stmt; std::string clock; };
  std::vector< Step > confirmed_a, confirmed_b, confirmed_startup;
  std::string model;
  JsonDict replay;
};

// Counts include attempted prefixes, including unsupported ones. Time is
// measured inside solver checks/export, not a claim about whole-pipeline cost.
struct Metrics {
  std::uint64_t path_queries = 0, prefix_events = 0, solver_queries = 0;
  std::uint64_t assertions = 0, solver_us = 0, evidence_us = 0;
  std::uint64_t ai_bounds = 0, ai_no_wrap = 0;
  // Positive pointer-store annotations, not unique functions or publications.
  std::uint64_t ai_single_execution = 0;

  void add(const Metrics& other) {
    path_queries += other.path_queries; prefix_events += other.prefix_events;
    solver_queries += other.solver_queries; assertions += other.assertions;
    solver_us += other.solver_us; evidence_us += other.evidence_us;
    ai_bounds += other.ai_bounds; ai_no_wrap += other.ai_no_wrap;
    ai_single_execution += other.ai_single_execution;
  }

  JsonDict json() const {
    return JsonDict{{"path_queries", path_queries}, {"prefix_events", prefix_events},
      {"solver_queries", solver_queries}, {"assertions", assertions},
      {"solver_us", solver_us}, {"evidence_us", evidence_us},
      {"ai_bounds", ai_bounds}, {"ai_no_wrap", ai_no_wrap},
      {"ai_single_execution", ai_single_execution}};
  }
};

struct Result {
  Status status;
  std::set< Status > observed;
  Counterexample evidence;
  Metrics metrics;
  bool needs_completion = false; // Capability hint, not a verdict premise.

  explicit Result(Status status = Status::Unsupported)
      : status(status), observed{status} {}

  bool confirmed() const { return status == Status::Confirmed; }

  // Preserve gaps in a failed search; only a concrete selected model dominates
  // them. This aggregation never turns unsupported branches into UNSAT.
  void merge(Result other) {
    metrics.add(other.metrics);
    needs_completion |= other.needs_completion;
    observed.insert(other.observed.begin(), other.observed.end());
    if (confirmed()) return;
    if (static_cast< unsigned >(other.status) > static_cast< unsigned >(status))
      status = other.status;
    if (other.confirmed()) evidence = std::move(other.evidence);
  }

  JsonDict summary() const {
    JsonList outcomes;
    for (auto outcome : observed) outcomes.add(status_name(outcome));
    return JsonDict{{"status", status_name(status)}, {"observed", outcomes}, {"metrics", metrics.json()},
                    {"validation_status", confirmed() ? "UNVALIDATED" : "NO_COUNTEREXAMPLE"}};
  }
};

} } }
