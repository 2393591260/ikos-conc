// Copyright and license: see core/domain/concurrent_global_env.hpp.
#include <ikos/core/domain/concurrent_global_env.hpp>

#include <cstdlib>
#include <iostream>
#include <unordered_set>

namespace ar = ikos::ar;
using ikos::core::ConcurrentGlobalEnv;
using Joined = std::unordered_set< std::uint64_t >;

static void require(bool condition, const char* reason) {
  if (!condition) {
    std::cerr << reason << '\n';
    std::exit(EXIT_FAILURE);
  }
}

static void test_mutex_field_values() {
  using ikos::core::MachineInt;
  using ikos::core::Signed;
  using Interval = ikos::core::machine_int::Interval;
  const auto value = [](std::int64_t number, std::uint64_t width = 32) {
    return Interval(MachineInt(number, width, Signed));
  };
  constexpr std::uint64_t base = 101, mu = 201, nu = 202;
  ConcurrentGlobalEnv fields;
  fields.initialize_mutex_value(base, value(3), 4);
  fields.initialize_mutex_value(base, value(9), 8);
  // Width is part of the board key. Actual program byte-overlap rejection is
  // the caller's closure obligation, not an alias claim made by this board.
  fields.initialize_mutex_value(base, value(4294967297LL, 64), 4);
  fields.enable_mutex_values();
  require(fields.mutex_values_available() &&
              fields.has_mutex_value_cell(base, 4, 32) &&
              fields.has_mutex_value_cell(base, 8, 32) &&
              fields.has_mutex_value_cell(base, 4, 64) &&
              !fields.has_mutex_value_cell(base, 4, 16),
          "Aggregate cells keep exact offset and width identities");
  require(fields.read_mutex_handoff(base, {mu}, 32, Signed, 4).equals(value(3)),
          "A first acquire retains a nonzero initial field value");
  fields.join_mutex_ongoing(base, {mu}, value(1), 4);
  fields.join_mutex_release(base, {mu}, value(2), 4);
  require(fields.is_dirty() &&
              fields.read_mutex_ongoing(base, {nu}, 32, Signed, 4).equals(value(1)) &&
              fields.read_mutex_handoff(base, {}, 32, Signed, 4).equals(
                  Interval(MachineInt(1, 32, Signed), MachineInt(3, 32, Signed))),
          "Handoff joins initial, ongoing, and released alternatives");
  require(fields.read_mutex_ongoing(base, {mu}, 32, Signed, 4).is_bottom(),
          "An actually held mutex excludes concurrent protected writes");
  require(fields.read_mutex_handoff(base, {}, 32, Signed, 8).equals(value(9)) &&
              fields.read_mutex_handoff(base, {}, 64, Signed, 4).equals(
                  value(4294967297LL, 64)) &&
              fields.read_mutex_ongoing(base, {}, 32, Signed, 8).is_bottom(),
          "A write cannot pollute another offset or another width");
  fields.clear_dirty();
  fields.join_mutex_ongoing(base, {mu}, value(1), 4);
  fields.join_mutex_release(base, {mu}, value(2), 4);
  require(!fields.is_dirty(), "Unchanged ongoing and release replays are stable");

  ConcurrentGlobalEnv locks;
  locks.initialize_mutex_value(base, value(0), 4);
  locks.enable_mutex_values();
  locks.join_mutex_ongoing(base, {mu}, value(1), 4);
  locks.join_mutex_ongoing(base, {mu, nu}, value(1), 4);
  require(locks.mutex_field_protected(base, 4, 32, {mu}) &&
              !locks.mutex_field_protected(base, 4, 32, {nu}),
          "All writers and the reader must share the same actual mutex");
  locks.clear_dirty();
  locks.join_mutex_ongoing(base, {nu}, value(1), 4);
  require(locks.is_dirty() &&
              !locks.mutex_field_protected(base, 4, 32, {mu, nu}),
          "A same-value late lock partition invalidates a pairwise-only gate");
  locks.clear_dirty();
  locks.join_mutex_ongoing(base, {nu}, value(1), 4);
  locks.join_mutex_ongoing(base, {mu}, value(1), 4);
  require(!locks.is_dirty() &&
              !locks.mutex_field_protected(base, 4, 32, {mu, nu}),
          "Replay neither dirties nor restores a revoked common-lock gate");

  ConcurrentGlobalEnv unlocked;
  unlocked.initialize_mutex_value(base, value(0), 4);
  unlocked.enable_mutex_values();
  unlocked.join_mutex_ongoing(base, {mu}, value(1), 4);
  require(unlocked.mutex_field_protected(base, 4, 32, {mu}),
          "A protected writer initially grants the field gate");
  unlocked.clear_dirty();
  unlocked.join_mutex_ongoing(base, {}, value(1), 4);
  require(unlocked.is_dirty() &&
              !unlocked.mutex_field_protected(base, 4, 32, {mu}) &&
              unlocked.read_mutex_ongoing(base, {mu}, 32, Signed, 4).equals(value(1)),
          "An unlocked same-value writer revokes the gate and stays observable");
  unlocked.clear_dirty();
  unlocked.invalidate_mutex_values();
  require(unlocked.is_dirty() && !unlocked.mutex_values_available() &&
              unlocked.read_mutex_ongoing(base, {mu}, 32, Signed, 4).is_top() &&
              unlocked.read_mutex_handoff(base, {mu}, 32, Signed, 4).is_top(),
          "Invalidation removes usable field feedback and forces TOP");
  unlocked.clear_dirty();
  unlocked.invalidate_mutex_values();
  require(!unlocked.is_dirty() && !unlocked.mutex_values_available(),
          "Repeated invalidation is stable and cannot restore availability");
}

static void test_mutex_field_equalities() {
  using ikos::core::MachineInt;
  using ikos::core::Signed;
  using Interval = ikos::core::machine_int::Interval;
  constexpr std::uint64_t base = 101, mu = 201, nu = 202;
  ConcurrentGlobalEnv env;
  const auto zero = Interval(MachineInt(0, 32, Signed));
  const auto range = Interval(MachineInt(0, 32, Signed), MachineInt(2, 32, Signed));
  env.initialize_mutex_value(base, zero, 0);
  env.initialize_mutex_value(base, zero, 4);
  env.initialize_mutex_equality(base, 0, 4, 32);
  env.initialize_mutex_equality(base, 0, 4, 64);
  env.enable_mutex_values();
  require(env.mutex_equalities(base).size() == 1 &&
              env.mutex_equality_available(base, 0, 4, 32, mu),
          "Only registered complete cells form an initial equality candidate");
  env.join_mutex_ongoing(base, {mu}, range, 0);
  env.join_mutex_ongoing(base, {mu}, range, 4);
  env.clear_dirty();
  env.join_mutex_equality_release(base, 0, 4, 32, mu, true);
  require(!env.is_dirty() && env.mutex_equality_available(base, 0, 4, 32, mu) &&
              !env.mutex_equality_available(base, 0, 4, 32, nu),
          "Both fields require the same actual mutex across all writer partitions");
  env.join_mutex_equality_release(base, 0, 4, 32, nu, false);
  require(env.is_dirty() && env.mutex_equality_available(base, 0, 4, 32, mu),
          "An unrelated nested release cannot revoke the protecting mutex");
  env.clear_dirty();
  env.join_mutex_equality_release(base, 0, 4, 32, mu, false);
  require(env.is_dirty() && !env.mutex_equality_available(base, 0, 4, 32, mu) &&
              env.read_mutex_handoff(base, {}, 32, Signed, 0).equals(range) &&
              env.read_mutex_handoff(base, {}, 32, Signed, 4).equals(range),
          "Loss of joint equality dirties even when both marginals are unchanged");
  env.clear_dirty();
  env.join_mutex_equality_release(base, 0, 4, 32, mu, true);
  env.join_mutex_equality_release(base, 0, 4, 32, mu, false);
  require(!env.is_dirty() && !env.mutex_equality_available(base, 0, 4, 32, mu),
          "MUST intersection is stable and cannot resurrect a revoked equality");
  env.clear();
  require(env.mutex_equalities(base).empty(), "Reset discards joint templates");

  env.initialize_mutex_value(base, zero, 0);
  env.initialize_mutex_value(base, zero, 4);
  env.initialize_mutex_equality(base, 0, 4, 32);
  env.enable_mutex_values();
  env.join_mutex_ongoing(base, {mu}, range, 0);
  env.join_mutex_ongoing(base, {mu}, range, 4);
  env.clear_dirty();
  env.join_mutex_ongoing(base, {}, range, 4);
  require(env.is_dirty() && !env.mutex_equality_available(base, 0, 4, 32, mu),
          "A late same-marginal unlocked writer revokes joint protection");
  env.invalidate_mutex_values();
  require(!env.mutex_equality_available(base, 0, 4, 32, mu),
          "Unknown effects also disable the joint channel");
}

static void test_interference_selection() {
  using ikos::core::MachineInt;
  using ikos::core::Signed;
  using Interval = ikos::core::machine_int::Interval;
  // Exhaust the small lockset space against the direct semantic definition.
  const auto locks = [](unsigned mask) {
    Joined result;
    for (unsigned bit = 0; bit < 3; ++bit)
      if (mask & (1u << bit)) result.insert(201 + bit);
    return result;
  };
  for (unsigned writers = 0; writers < 256; ++writers) {
    ConcurrentGlobalEnv env;
    const auto initial = Interval(MachineInt(-1, 32, Signed));
    env.initialize_mutex_value(101, initial);
    env.enable_mutex_values();
    for (unsigned mask = 0; mask < 8; ++mask) {
      if (!(writers & (1u << mask))) continue;
      // TOP first also exercises absorbing joins followed by finite values.
      auto value = mask == 0 ? Interval::top(32, Signed)
                             : Interval(MachineInt(mask, 32, Signed));
      env.join_mutex_ongoing(101, locks(mask), value);
      env.join_mutex_release(101, locks(mask), value);
    }
    for (unsigned reader = 0; reader < 8; ++reader) {
      auto ongoing = Interval::bottom(32, Signed);
      unsigned common = reader;
      for (unsigned mask = 0; mask < 8; ++mask) {
        if (!(writers & (1u << mask))) continue;
        common &= mask;
        if (!(reader & mask)) {
          auto value = mask == 0 ? Interval::top(32, Signed)
                                 : Interval(MachineInt(mask, 32, Signed));
          ongoing.join_with(value);
        }
      }
      auto handoff = initial;
      handoff.join_with(ongoing);
      require(env.mutex_field_protected(101, 0, 32, locks(reader)) == (common != 0) &&
              env.read_mutex_ongoing(101, locks(reader), 32, Signed).equals(ongoing) &&
              env.read_mutex_handoff(101, locks(reader), 32, Signed).equals(handoff),
              "Short-circuits preserve every writer partition and completed handoff");
    }
  }
}

int main() {
  test_mutex_field_values();
  test_mutex_field_equalities();
  test_interference_selection();
  ar::Context context;
  auto* bundle =
      ar::Bundle::create(context,
                         ar::DataLayout::create(ar::LittleEndian, {32, 4, 4}),
                         "");
  auto* type =
      ar::FunctionType::get(context, ar::VoidType::get(context), {}, false);
  auto* first = ar::Function::create(bundle, type, "first", true);
  auto* later = ar::Function::create(bundle, type, "later", true);
  ikos::analyzer::UnnamedShadowVariable
      pointer(ar::PointerType::get(context, ar::VoidType::get(context)), 0);
  ConcurrentGlobalEnv env;

  require(!env.is_dirty(), "A fresh blackboard is clean");
  env.mark_glob_flown(nullptr);
  require(!env.is_glob_flown(nullptr) && !env.is_dirty(),
          "Null global-flow provenance is ignored");
  require(!env.is_glob_flown(&pointer), "Provenance starts absent");
  env.mark_glob_flown(&pointer);
  require(env.is_glob_flown(&pointer) && env.is_dirty(),
          "New global-flow provenance invalidates the shared fixpoint");
  env.clear_dirty();
  env.mark_glob_flown(&pointer);
  require(!env.is_dirty(), "Repeated global-flow provenance is unchanged");

  require(env.get_join_summary(first).empty(),
          "An absent join summary is empty");
  env.set_join_summary(first, {});
  require(env.get_join_summary(first).empty() && !env.is_dirty(),
          "First-seen empty joins agree with an absent summary");
  env.set_join_summary(nullptr, {1});
  require(env.get_join_summary(nullptr).empty() && !env.is_dirty(),
          "Null join-summary roots are ignored");

  env.set_join_summary(first, {1});
  require(env.is_dirty() && env.get_join_summary(first) == Joined{1},
          "New nonempty MUST-join facts invalidate the fixpoint");
  env.clear_dirty();
  env.set_join_summary(first, {1});
  require(!env.is_dirty(), "An unchanged MUST-join summary is stable");
  env.set_join_summary(first, {1, 2});
  require(env.is_dirty() && env.get_join_summary(first) == Joined({1, 2}),
          "Growing MUST-join facts invalidate the fixpoint");
  env.clear_dirty();
  env.set_join_summary(first, {2});
  require(env.is_dirty() && env.get_join_summary(first) == Joined{2},
          "Replay replaces shrinking MUST-join facts rather than retaining "
          "them");
  env.clear_dirty();
  env.set_join_summary(first, {3});
  require(env.is_dirty() && env.get_join_summary(first) == Joined{3},
          "Same-size MUST-join replacement invalidates the fixpoint");

  // A later unchanged root cannot make an earlier replay's change stable.
  env.set_join_summary(later, {});
  env.set_join_summary(first, {3});
  env.mark_glob_flown(&pointer);
  env.mark_glob_flown(nullptr);
  require(env.is_dirty(), "Invalidation remains sticky across unchanged roots");

  env.clear_dirty();
  env.set_join_summary(first, {});
  require(env.is_dirty() && env.get_join_summary(first).empty(),
          "Losing every MUST-join fact invalidates the fixpoint");
  env.clear_dirty();
  env.set_join_summary(first, {});
  env.set_join_summary(later, {});
  require(!env.is_dirty(), "Repeated empty summaries are stable");
  ConcurrentGlobalEnv once;
  require(!once.same_closed_once("first", "later"), "Absent once identities cannot exclude");
  auto sid = once.register_once_thread(first, 7, true, ikos::core::ZNumber(0), true);
  require(sid && once.is_dirty(), "A sealed once identity invalidates discovery");
  require(once.register_once_thread(later, 7, true, ikos::core::ZNumber(0), true) == sid &&
          once.same_closed_once("first", "later"), "Alternatives share one sealed instance");
  require(once.site_func(sid) == nullptr, "Alternative callbacks have no representative function");
  once.clear_dirty();
  once.register_once_thread(first, 7, true, ikos::core::ZNumber(0), true);
  once.register_once_thread(later, 7, true, ikos::core::ZNumber(0), true);
  require(!once.is_dirty() && once.site_func(sid) == nullptr,
          "Callback replay is idempotent and cannot restore a representative");
  once.register_once_thread(later, 8, true, ikos::core::ZNumber(0), true);
  require(once.is_dirty() && !once.same_closed_once("first", "later"),
          "A later extra control revokes the singleton exclusion");
  ConcurrentGlobalEnv unsealed;
  unsealed.register_once_thread(first, 7, true, ikos::core::ZNumber(0));
  unsealed.register_once_thread(later, 7, true, ikos::core::ZNumber(0));
  require(!unsealed.same_closed_once("first", "later"), "Address equality alone grants no seal");
  unsealed.register_once_thread(first, 7, false, ikos::core::ZNumber(0), true);
  require(!unsealed.same_closed_once("first", "later"), "An ambiguous control never seals");
  std::cout << "Concurrent blackboard invalidation passed\n";
}
