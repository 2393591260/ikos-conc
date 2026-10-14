#define BOOST_TEST_MODULE test_lockset_domain
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <vector>

#include <ikos/core/domain/lockset/lockset_domain.hpp>

using Domain = ikos::core::lockset::LocksetDomain;
using Polarity = ikos::core::lockset::CondPolarity;

BOOST_AUTO_TEST_CASE(top_is_unknown_and_entry_has_empty_history) {
  auto top = Domain::top();
  auto entry = Domain::entry();
  BOOST_CHECK(top.is_top());
  BOOST_CHECK(top.spawned_is_top());
  BOOST_CHECK(top.live_children_is_top());
  BOOST_CHECK(!entry.is_top());
  BOOST_CHECK(!entry.spawned_is_top());
  BOOST_CHECK(!entry.live_children_is_top());
  BOOST_CHECK(entry.get_spawned_threads().empty());
  BOOST_CHECK(entry.get_spawned_instances().empty());
  BOOST_CHECK(!entry.may_ever_acquired_mutex());
  BOOST_CHECK(entry.leq(top));
  BOOST_CHECK(!top.leq(entry));

  auto history = entry;
  history.add_spawned_thread("child");
  history.add_spawned_instance(17);
  history.add_live_child(17);
  history.add_signaled_cond(3);
  history.add_awaited_cond(4);
  BOOST_CHECK(history.leq(top));
  history.set_to_top();
  BOOST_CHECK(history.equals(top));
  BOOST_CHECK(history.spawned_is_top());

  auto fresh = top;
  fresh.add_heap_node_fresh(42);
  BOOST_CHECK(!fresh.is_top());
  BOOST_CHECK(!fresh.equals(top));
  BOOST_CHECK(fresh.leq(top));
}

BOOST_AUTO_TEST_CASE(may_instances_meet_unknown_is_identity) {
  auto finite = Domain::entry();
  finite.add_spawned_thread("child");
  finite.add_spawned_instance(17);
  finite.add_spawned_instance(19);
  auto unknown = finite;
  unknown.set_spawned_top();
  BOOST_CHECK(finite.meet(unknown).equals(finite));
  BOOST_CHECK(unknown.meet(finite).equals(finite));
  BOOST_CHECK(Domain::top().meet(finite).equals(finite));
  BOOST_CHECK(finite.meet(Domain::top()).equals(finite));

  auto other = Domain::entry();
  other.add_spawned_thread("child");
  other.add_spawned_instance(19);
  other.add_spawned_instance(23);
  auto both = finite.meet(other);
  BOOST_CHECK(both.get_spawned_instances() == std::unordered_set< std::uint64_t >{19});
  BOOST_CHECK(both.leq(finite));
  BOOST_CHECK(both.leq(other));
}

BOOST_AUTO_TEST_CASE(conditional_meet_keeps_distinct_implications) {
  auto left = Domain::top();
  left.add_cond_lock(1, 10);
  auto right = Domain::top();
  right.add_cond_lock(1, 20);
  auto both = left.meet(right);
  BOOST_CHECK(both.leq(left));
  BOOST_CHECK(both.leq(right));
  BOOST_CHECK(both.equals(right.meet(left)));
  BOOST_CHECK_EQUAL(both.cond_locks().size(), 2);
  BOOST_CHECK(both.meet(both).equals(both));
  BOOST_CHECK(both.join(left).equals(left));
  BOOST_CHECK(left.join(right).equals(Domain::top()));

  auto opposite = Domain::top();
  opposite.add_cond_lock(1, 10, Polarity::HELD_IF_NONZERO);
  auto two_predicates = left.meet(opposite);
  BOOST_CHECK_EQUAL(two_predicates.cond_locks().size(), 2);
  BOOST_CHECK(two_predicates.leq(left));
  BOOST_CHECK(two_predicates.leq(opposite));
  BOOST_CHECK(two_predicates.join(left).equals(left));

  both.remove_cond_lock(10);
  BOOST_CHECK(both.equals(right));
  // Transfer keeps its old binding semantics; meet is a separate operation.
  both.add_cond_lock(1, 30);
  BOOST_CHECK_EQUAL(both.cond_locks().size(), 1);
  BOOST_CHECK_EQUAL(both.cond_locks().begin()->second.lock_addr, 30);
}

BOOST_AUTO_TEST_CASE(finite_product_lattice_laws) {
  std::vector< Domain > states{Domain::top(), Domain::bottom(), Domain::entry()};
  for (unsigned mask = 0; mask < 32; ++mask) {
    auto state = Domain::entry();
    if (mask & 1) {
      state.add_mutex(10);
      state.add_mutex(10);
    }
    if (mask & 2) {
      state.add_spawned_thread("child");
      state.add_spawned_instance(17);
      state.add_live_child(17);
    }
    if (mask & 4) state.add_heap_node_fresh(42);
    if (mask & 8) state.add_cond_lock(1, 20);
    if (mask & 16) {
      state.add_signaled_cond(3);
      state.add_awaited_cond(4);
    }
    states.push_back(state);
  }
  auto other_cond = Domain::top();
  other_cond.add_cond_lock(1, 30);
  states.push_back(other_cond);
  auto unknown_spawn = Domain::entry();
  unknown_spawn.set_spawned_top();
  states.push_back(unknown_spawn);
  auto ownership = Domain::entry();
  ownership.add_read_lock(20);
  ownership.add_joined_thread(17);
  states.push_back(ownership);

  for (const auto& a : states) {
    BOOST_CHECK(a.leq(Domain::top()));
    BOOST_CHECK(Domain::bottom().leq(a));
    BOOST_CHECK_EQUAL(a.is_top(), a.equals(Domain::top()));
    BOOST_CHECK(a.join(a).equals(a));
    BOOST_CHECK(a.meet(a).equals(a));
    BOOST_CHECK(a.join(Domain::bottom()).equals(a));
    BOOST_CHECK(a.meet(Domain::top()).equals(a));
    for (const auto& b : states) {
      auto joined = a.join(b);
      auto met = a.meet(b);
      BOOST_CHECK(a.leq(joined) && b.leq(joined));
      BOOST_CHECK(met.leq(a) && met.leq(b));
      BOOST_CHECK(joined.equals(b.join(a)));
      BOOST_CHECK(met.equals(b.meet(a)));
      BOOST_CHECK_EQUAL(a.equals(b), a.leq(b) && b.leq(a));
      BOOST_CHECK(a.join(met).equals(a));
      BOOST_CHECK(a.meet(joined).equals(a));
      BOOST_CHECK(a.leq(a.widening(b)) && b.leq(a.widening(b)));
      BOOST_CHECK(a.narrowing(b).leq(a));
      for (const auto& c : states) {
        BOOST_CHECK(a.join(b).join(c).equals(a.join(b.join(c))));
        BOOST_CHECK(a.meet(b).meet(c).equals(a.meet(b.meet(c))));
        if (a.leq(b) && b.leq(c)) BOOST_CHECK(a.leq(c));
      }
    }
  }
}
