#pragma once

#include <set>
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

struct Result {
  Status status;
  std::set< Status > observed;
  Counterexample evidence;

  explicit Result(Status status = Status::Unsupported)
      : status(status), observed{status} {}

  bool confirmed() const { return status == Status::Confirmed; }

  // Preserve gaps in a failed search; only a concrete selected model dominates
  // them. This aggregation never turns unsupported branches into UNSAT.
  void merge(Result other) {
    observed.insert(other.observed.begin(), other.observed.end());
    if (confirmed()) return;
    if (static_cast< unsigned >(other.status) > static_cast< unsigned >(status))
      status = other.status;
    if (other.confirmed()) evidence = std::move(other.evidence);
  }

  JsonDict summary() const {
    JsonList outcomes;
    for (auto outcome : observed) outcomes.add(status_name(outcome));
    return JsonDict{{"status", status_name(status)}, {"observed", outcomes},
                    {"validation_status", confirmed() ? "UNVALIDATED" : "NO_COUNTEREXAMPLE"}};
  }
};

} } }
