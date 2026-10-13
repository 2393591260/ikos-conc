#pragma once

#include <map>
#include <string>
#include <vector>

#include <ikos/ar/semantic/statement.hpp>
#include <ikos/analyzer/json/json.hpp>

namespace ikos {
namespace analyzer {
namespace bmc {

// Counts describe observed search outcomes, not independently removable gates.
struct Diagnostics {
  struct Example {
    std::string reason;
    ar::Statement* stmt;
  };
  std::map< std::string, std::size_t > counts;
  std::vector< Example > examples;
  std::string first_blocker;
  std::size_t paths_a = 0, paths_b = 0, path_pairs = 0;

  void record(const std::string& reason, ar::Statement* stmt = nullptr) {
    ++counts[reason];
    if (first_blocker.empty() && reason != "sat" && reason != "integer_operation" &&
        reason != "sc_read_from" && reason != "ai_candidate_bounds" && reason != "ai_no_wrap" &&
        reason != "selected_activation_occurrences" && reason != "startup_create_offset" &&
        reason != "ai_single_execution") {
      first_blocker = reason;
    }
    if (stmt == nullptr || examples.size() >= 16) {
      return;
    }
    for (const auto& e : examples) {
      if (e.reason == reason && e.stmt == stmt) {
        return;
      }
    }
    examples.push_back({reason, stmt});
  }
};

inline void record(Diagnostics* diagnostics, const std::string& reason,
                   ar::Statement* stmt = nullptr) {
  if (diagnostics != nullptr) {
    diagnostics->record(reason, stmt);
  }
}

} // namespace bmc
} // namespace analyzer
} // namespace ikos
