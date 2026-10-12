#include <cassert>
#include <vector>
#include <ikos/analyzer/analysis/execution_engine/concurrent_semantics.hpp>
int main() {
  using namespace ikos;
  using core::ZNumber;
  using analyzer::offset_lock_key;
  assert(offset_lock_key(1, ZNumber(0)) != 0);
  assert(offset_lock_key(1, ZNumber(0)) != offset_lock_key(1, ZNumber(8)));
  assert(offset_lock_key(1, ZNumber(8)) != offset_lock_key(2, ZNumber(8)));
  auto max = offset_lock_key(0xFFFFFFFFULL, ZNumber(0xFFFFFFFFULL));
  assert(max != 0);
  assert(analyzer::lock_key_base(max) == 0xFFFFFFFFULL);
  assert(analyzer::lock_key_instance(max) == 0xFFFFFFFFULL);
  assert(offset_lock_key(1, ZNumber(4294967296ULL)) == 0);
  assert(offset_lock_key(1, ZNumber(4294967312ULL)) == 0);
  assert(offset_lock_key(1, ZNumber(-1)) == 0);
  assert(offset_lock_key(4294967296ULL, ZNumber(0)) == 0);
  assert(offset_lock_key(0, ZNumber(0)) == 0);
  assert(offset_lock_key(1, ZNumber::from_string("18446744073709551616")) == 0);

  using core::lockset::LocksetDomain;
  auto none = LocksetDomain::top();
  std::vector< LocksetDomain > states{none, LocksetDomain::bottom()};
  auto mutex = none;
  mutex.add_mutex(17);
  assert(mutex.leq(none) && !none.leq(mutex));
  auto recursive = mutex;
  recursive.add_mutex(17);
  assert(recursive.leq(mutex) && !mutex.leq(recursive));
  auto empty = mutex;
  empty.remove_mutex(17);
  assert(empty.equals(none) && empty.leq(none) && none.leq(empty));
  auto read = none;
  read.add_read_lock(17);
  auto joined = none;
  joined.add_joined_thread(17);
  auto conditional = none;
  conditional.add_cond_lock(17, 23);
  auto fresh = none;
  fresh.add_heap_node_fresh(17);
  for (const auto& must : {mutex, recursive, read, joined, conditional, fresh}) {
    assert(must.leq(none) && !none.leq(must));
    states.push_back(must);
  }
  auto spawned = none;
  spawned.add_spawned_instance(17);
  assert(none.leq(spawned) && !spawned.leq(none));
  states.push_back(spawned);
  states.push_back(empty);
  for (const auto& a : states) for (const auto& b : states) {
    auto upper = a;
    upper.join_with(b);
    assert(a.leq(upper) && b.leq(upper));
    auto lower = a;
    lower.meet_with(b);
    assert(lower.leq(a) && lower.leq(b));
    assert(!a.equals(b) || (a.leq(b) && b.leq(a)));
  }
}
