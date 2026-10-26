#include <ikos/analyzer/analysis/pair_exclusion.hpp>

#include <algorithm>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

using namespace ikos::analyzer::pair_exclusion;
using ikos::core::MachineInt;
using ikos::core::Signed;
using ikos::core::Unsigned;
using ikos::core::Signedness;
using ikos::core::machine_int::Interval;
using ikos::core::machine_int::Congruence;

void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

AccessFacts facts(unsigned bits, Signedness sign, ZNumber lo, ZNumber hi,
                  ZNumber modulus, ZNumber residue, std::uint64_t width) {
  return {Interval(MachineInt(lo, bits, sign), MachineInt(hi, bits, sign)),
          Congruence(modulus, residue, bits, sign), width};
}

AccessFacts small(unsigned bits, Signedness sign, int lo, int hi,
                  int modulus, int residue, std::uint64_t width) {
  return facts(bits, sign, ZNumber(lo), ZNumber(hi), ZNumber(modulus),
               ZNumber(residue), width);
}

const char* name(Status status) {
  switch (status) {
    case Status::Unsat: return "UNSAT";
    case Status::Sat: return "SAT";
    case Status::Unknown: return "UNKNOWN";
    case Status::InvalidFacts: return "INVALID_FACTS";
  }
  return "INVALID_ENUM";
}

void observation(const char* test, unsigned bits, const AccessFacts& a,
                  const AccessFacts& b, Status expected, unsigned timeout = 1000) {
  auto result = check(a, b, bits, timeout);
  require(result.status == expected, test);
  bool native = native_excludes(a, b, bits);
  require(!native || result.status == Status::Unsat, "native false exclusion");
  std::cout << "{\"test\":\"" << test << "\",\"bits\":" << bits
            << ",\"status\":\"" << name(result.status)
            << "\",\"native_excludes\":" << (native ? "true" : "false")
            << ",\"elapsed_us\":" << result.elapsed_us;
  if (result.model) {
    const auto& m = *result.model;
    require(model_satisfies(a, b, bits, m), "SAT model recheck");
    std::cout << ",\"model\":{\"start_a\":\"" << m.start_a
              << "\",\"start_b\":\"" << m.start_b << "\",\"byte_a\":\""
              << m.byte_a << "\",\"byte_b\":\"" << m.byte_b
              << "\",\"address\":\"" << m.address << "\"}";
  }
  std::cout << "}\n";
}

// Independent exhaustive concrete enumeration for tiny address spaces.
bool concrete_contains(const AccessFacts& facts, int value) {
  int modulus = facts.congruence.modulus().to< int >();
  int residue = facts.congruence.residue().to< int >();
  return modulus == 0 ? value == residue : (value - residue) % modulus == 0;
}

bool concrete_overlap(const AccessFacts& a, const AccessFacts& b, unsigned bits) {
  int space = 1 << bits;
  for (int x = a.offset.lb().to_z_number().to< int >();
       x <= a.offset.ub().to_z_number().to< int >(); ++x) {
    if (!concrete_contains(a, x)) continue;
    for (int y = b.offset.lb().to_z_number().to< int >();
         y <= b.offset.ub().to_z_number().to< int >(); ++y) {
      if (!concrete_contains(b, y)) continue;
      for (std::uint64_t u = 0; u < a.width; ++u)
        for (std::uint64_t v = 0; v < b.width; ++v)
          if (((x + int(u)) % space + space) % space ==
              ((y + int(v)) % space + space) % space) return true;
    }
  }
  return false;
}

int main() {
  for (unsigned bits : {32u, 64u}) {
    auto even = small(bits, Unsigned, 0, 56, 8, 0, 4);
    auto odd = small(bits, Unsigned, 4, 60, 8, 4, 4);
    auto weak_even = small(bits, Unsigned, 0, 56, 1, 0, 4);
    auto weak_odd = small(bits, Unsigned, 4, 60, 1, 0, 4);
    observation("weak-ai-parity", bits, weak_even, weak_odd, Status::Sat);
    observation("refined-ai-parity", bits, even, odd, Status::Unsat);
    observation("same-parity", bits, even, even, Status::Sat);
    auto wide = even;
    wide.width = 8;
    observation("mixed-width-overlap", bits, wide, odd, Status::Sat);
    observation("unknown-second-index", bits, even, weak_odd, Status::Sat);
    observation("signed-negative-wrap", bits,
                small(bits, Signed, -2, -2, 0, -2, 4),
                small(bits, Unsigned, 0, 0, 0, 0, 1), Status::Sat);
    auto near_wrap = facts(bits, Unsigned, (ZNumber(1) << bits) - 2,
                           (ZNumber(1) << bits) - 2, ZNumber(0),
                           (ZNumber(1) << bits) - 2, 4);
    observation("unsigned-wrap", bits, near_wrap,
                small(bits, Unsigned, 0, 0, 0, 0, 1), Status::Sat);
    observation("interval-refines-native-baseline", bits,
                small(bits, Unsigned, 0, 3, 1, 0, 4),
                small(bits, Unsigned, 12, 15, 1, 0, 4), Status::Unsat);
    observation("overlapping-hulls-need-both-facts", bits,
                small(bits, Unsigned, 0, 16, 16, 0, 4),
                small(bits, Unsigned, 8, 12, 4, 0, 4), Status::Unsat);
    auto empty = even;
    empty.offset = Interval::bottom(bits, Unsigned);
    observation("bottom-interval", bits, empty, odd, Status::InvalidFacts);
    empty = even;
    empty.congruence = Congruence::bottom(bits, Unsigned);
    observation("bottom-congruence", bits, empty, odd, Status::InvalidFacts);
    empty = small(bits, Unsigned, 0, 0, 2, 1, 4);
    observation("empty-fact-intersection", bits, empty, odd, Status::InvalidFacts);
    empty = even;
    empty.congruence = Congruence::top(bits, Signed);
    observation("signedness-mismatch", bits, empty, odd, Status::InvalidFacts);
    empty = even;
    empty.width = 0;
    observation("unknown-width", bits, empty, odd, Status::Unknown);
    observation("exhausted-budget", bits, weak_even, weak_odd, Status::Unknown, 0);

    auto saved = check(even, odd, bits, 1000, true);
    require(saved.status == Status::Unsat && !saved.smtlib.empty(), "query export");
    z3::context ctx;
    z3::solver independent(ctx);
    independent.from_string(saved.smtlib.c_str());
    require(independent.check() == z3::unsat, "exported query replay");
    auto limited = check(even, odd, bits, 1);
    require(limited.status == Status::Unknown || limited.status == Status::Unsat,
            "one-ms solver budget returned a false model");
    std::cout << "{\"test\":\"one-ms-solver-budget\",\"bits\":" << bits
              << ",\"status\":\"" << name(limited.status)
              << "\",\"elapsed_us\":" << limited.elapsed_us << "}\n";
  }
  observation("modulus-must-include-address-space", 4,
              small(4, Unsigned, 15, 15, 3, 0, 2),
              small(4, Unsigned, 0, 0, 3, 0, 1), Status::Sat);

  std::mt19937 rng(0x1c052027);
  unsigned valid = 0, invalid = 0, native = 0, sat = 0, unsat = 0;
  for (unsigned i = 0; i < 256; ++i) {
    unsigned bits = 3 + rng() % 3;
    auto generate = [&]() {
      Signedness sign = rng() % 2 ? Signed : Unsigned;
      int shift = sign == Signed ? -(1 << (bits - 1)) : 0;
      int lo = int(rng() % (1 << bits)) + shift;
      int hi = int(rng() % (1 << bits)) + shift;
      if (lo > hi) std::swap(lo, hi);
      return small(bits, sign, lo, hi, int(rng() % 10),
                   int(rng() % (1 << bits)) + shift, 1 + rng() % 9);
    };
    auto a = generate(), b = generate();
    auto result = check(a, b, bits, 1000);
    bool baseline = native_excludes(a, b, bits);
    if (!invalid_facts(a, bits).empty() || !invalid_facts(b, bits).empty()) {
      require(result.status == Status::InvalidFacts && !baseline, "invalid random facts");
      ++invalid;
      continue;
    }
    bool overlap = concrete_overlap(a, b, bits);
    require(result.status == (overlap ? Status::Sat : Status::Unsat),
            "query disagrees with exhaustive byte enumeration");
    require(!baseline || !overlap, "native disagrees with concrete enumeration");
    if (overlap) {
      require(result.model && model_satisfies(a, b, bits, *result.model),
              "random model invalid");
      ++sat;
    } else ++unsat;
    ++valid;
    native += baseline;
  }
  std::cout << "{\"test\":\"tiny-exhaustive-enumeration\",\"generated\":256,"
            << "\"valid\":" << valid << ",\"invalid\":" << invalid
            << ",\"sat\":" << sat << ",\"unsat\":" << unsat
            << ",\"native_excluded\":" << native << "}\n";

}
