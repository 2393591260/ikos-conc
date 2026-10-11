#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include <boost/optional.hpp>
#include <z3++.h>

#include <ikos/core/value/machine_int/congruence.hpp>
#include <ikos/core/value/machine_int/interval.hpp>

namespace ikos {
namespace analyzer {
namespace pair_exclusion {

using core::ZNumber;

// The caller must establish a common concrete non-TLS object, complete access
// coverage, and stable over-approximating facts for each access independently.
// No path bound, thread-local equality, or shared-load value is assumed here.
struct AccessFacts {
  core::machine_int::Interval offset;
  core::machine_int::Congruence congruence;
  std::uint64_t width; // 0 means unknown, never an empty access.
};

enum class Status { Unsat, Sat, Unknown, InvalidFacts };

struct Model {
  ZNumber start_a, start_b;
  ZNumber byte_a, byte_b;
  ZNumber address; // Relative byte address modulo 2^pointer_bits.
};

struct Result {
  Status status = Status::Unknown;
  boost::optional< Model > model;
  std::string reason;
  std::string smtlib;
  std::uint64_t elapsed_us = 0;
};

inline bool contains(const AccessFacts& facts, const ZNumber& n) {
  if (facts.offset.is_bottom() || facts.congruence.is_bottom()) return false;
  if (n < facts.offset.lb().to_z_number() ||
      n > facts.offset.ub().to_z_number()) return false;
  const auto& m = facts.congruence.modulus();
  const auto& r = facts.congruence.residue();
  return m == 0 ? n == r : core::mod(n - r, m) == 0;
}

// Reject inconsistent evidence instead of using an empty fact conjunction as
// a vacuous SAFE proof. Reachability bottom belongs to the caller's contract.
inline std::string invalid_facts(const AccessFacts& facts,
                                 std::uint64_t pointer_bits) {
  if (facts.offset.is_bottom() || facts.congruence.is_bottom())
    return "bottom input requires a separate reachability proof";
  if (facts.offset.bit_width() != pointer_bits ||
      facts.congruence.bit_width() != pointer_bits ||
      facts.offset.sign() != facts.congruence.sign())
    return "offset/congruence type mismatch";
  const auto& m = facts.congruence.modulus();
  const auto& r = facts.congruence.residue();
  ZNumber lo = facts.offset.lb().to_z_number();
  ZNumber hi = facts.offset.ub().to_z_number();
  if ((m == 0 && (r < lo || r > hi)) ||
      (m != 0 && lo + core::mod(r - lo, m) > hi))
    return "interval/congruence intersection is empty";
  return {};
}

inline bool model_satisfies(const AccessFacts& a, const AccessFacts& b,
                            std::uint64_t pointer_bits, const Model& model) {
  if (pointer_bits == 0 || pointer_bits > 64 || a.width == 0 || b.width == 0 ||
      !invalid_facts(a, pointer_bits).empty() ||
      !invalid_facts(b, pointer_bits).empty()) return false;
  const ZNumber space = ZNumber(1) << pointer_bits;
  return contains(a, model.start_a) && contains(b, model.start_b) &&
         model.byte_a >= 0 && model.byte_a < ZNumber(a.width) &&
         model.byte_b >= 0 && model.byte_b < ZNumber(b.width) &&
         core::mod(model.start_a + model.byte_a, space) == model.address &&
         core::mod(model.start_b + model.byte_b, space) == model.address;
}

// A solver-free sufficient condition for the ablation. If bytes overlap,
// b-a = byte_a-byte_b modulo the address space. Its residue modulo gcd(M,ma,mb)
// must occur in [-(width_b-1), width_a-1]. Ignoring offset bounds enlarges states.
inline bool native_excludes(const AccessFacts& a, const AccessFacts& b,
                             std::uint64_t pointer_bits) {
  if (pointer_bits == 0 || pointer_bits > 64 || a.width == 0 || b.width == 0 ||
      !invalid_facts(a, pointer_bits).empty() ||
      !invalid_facts(b, pointer_bits).empty()) return false;
  ZNumber period = core::gcd(ZNumber(1) << pointer_bits,
                            core::gcd(a.congruence.modulus(),
                                      b.congruence.modulus()));
  ZNumber lo = -(ZNumber(b.width) - 1);
  ZNumber hi = ZNumber(a.width) - 1;
  ZNumber residue = b.congruence.residue() - a.congruence.residue();
  return lo + core::mod(residue - lo, period) > hi;
}

// SAT is only an abstract conflicting pair, not a concrete execution witness.
// UNSAT excludes this pair iff the caller's coverage contract holds. UNKNOWN,
// unsupported widths, and invalid facts all retain the original may candidate.
inline Result check(const AccessFacts& a, const AccessFacts& b,
                    std::uint64_t pointer_bits, unsigned timeout_ms = 20,
                    bool save_query = false) {
  auto started = std::chrono::steady_clock::now();
  Result result;
  auto finish = [&]() {
    result.elapsed_us = std::chrono::duration_cast< std::chrono::microseconds >(
                            std::chrono::steady_clock::now() - started).count();
    return result;
  };
  if (pointer_bits == 0 || pointer_bits > 64 || a.width == 0 || b.width == 0) {
    result.reason = "unsupported pointer bits or unknown access width";
    return finish();
  }
  result.reason = invalid_facts(a, pointer_bits);
  if (result.reason.empty()) result.reason = invalid_facts(b, pointer_bits);
  if (!result.reason.empty()) {
    result.status = Status::InvalidFacts;
    return finish();
  }
  // Z3 interprets timeout=0 as unlimited; this API interprets it as no budget.
  if (timeout_ms == 0) {
    result.reason = "query budget exhausted";
    return finish();
  }
  try {
    z3::context ctx;
    z3::solver solver(ctx);
    solver.set("timeout", timeout_ms);
    auto integer = [&](const ZNumber& n) { return ctx.int_val(n.str().c_str()); };
    auto start_a = ctx.int_const("start_a"), start_b = ctx.int_const("start_b");
    auto byte_a = ctx.int_const("byte_a"), byte_b = ctx.int_const("byte_b");
    auto add_facts = [&](const AccessFacts& facts, const z3::expr& start) {
      solver.add(integer(facts.offset.lb().to_z_number()) <= start &&
                 start <= integer(facts.offset.ub().to_z_number()));
      if (facts.congruence.modulus() == 0) {
        solver.add(start == integer(facts.congruence.residue()));
      } else if (!facts.congruence.is_top()) {
        solver.add(z3::mod(start - integer(facts.congruence.residue()),
                           integer(facts.congruence.modulus())) == 0);
      }
    };
    add_facts(a, start_a);
    add_facts(b, start_b);
    solver.add(0 <= byte_a && byte_a < integer(ZNumber(a.width)));
    solver.add(0 <= byte_b && byte_b < integer(ZNumber(b.width)));
    auto space = integer(ZNumber(1) << pointer_bits);
    auto address = z3::mod(start_a + byte_a, space);
    solver.add(address == z3::mod(start_b + byte_b, space));
    if (save_query) result.smtlib = solver.to_smt2();
    auto status = solver.check();
    if (status == z3::unsat) {
      result.status = Status::Unsat;
    } else if (status == z3::sat) {
      const auto model = solver.get_model();
      auto value = [&](const z3::expr& expression) {
        std::string text;
        if (!model.eval(expression, true).is_numeral(text))
          throw z3::exception("non-integer offset model");
        return ZNumber::from_string(text);
      };
      Model candidate{value(start_a), value(start_b), value(byte_a),
                      value(byte_b), value(address)};
      if (model_satisfies(a, b, pointer_bits, candidate)) {
        result.status = Status::Sat;
        result.model = std::move(candidate);
      } else {
        result.reason = "solver model failed arithmetic recheck";
      }
    } else {
      result.reason = solver.reason_unknown();
    }
  } catch (const z3::exception& error) {
    result.status = Status::Unknown;
    result.reason = error.msg();
  }
  return finish();
}

} // namespace pair_exclusion
} // namespace analyzer
} // namespace ikos
