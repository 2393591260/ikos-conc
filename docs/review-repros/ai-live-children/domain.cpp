#include <cassert>
#include <ikos/core/domain/lockset/lockset_domain.hpp>
using D = ikos::core::lockset::LocksetDomain;
int main() {
  auto unknown = D::top();
  auto empty = unknown;
  empty.initialize_live_children();
  assert(unknown.live_children_is_top());
  assert(!empty.live_children_is_top());
  assert(empty.leq(unknown) && !unknown.leq(empty));
  auto a = empty, b = empty;
  a.add_live_child(1); a.add_spawned_instance(1);
  b.add_live_child(2); b.add_spawned_instance(2);
  auto joined = a; joined.join_with(b);
  assert(joined.get_live_children().size() == 2);
  auto widened = a; widened.widen_with(b);
  assert(widened.equals(joined));
  auto met = a; met.meet_with(b);
  assert(met.get_live_children().empty());
  auto refined = unknown; refined.meet_with(a);
  assert(refined.get_live_children() == a.get_live_children());
  refined = a; refined.meet_with(unknown);
  assert(refined.get_live_children() == a.get_live_children());
  joined.join_with(unknown);
  assert(joined.live_children_is_top());
  joined.remove_live_child(1);
  assert(joined.live_children_is_top());
  auto done = a; done.remove_live_child(1);
  assert(done.get_live_children().empty());
  assert(done.get_spawned_instances().count(1));
  auto merge = done; merge.join_with(empty);
  assert(merge.get_live_children().empty());
  auto missing = done; missing.join_with(a);
  assert(missing.get_live_children().count(1));
  auto bottom = D::bottom(); bottom.join_with(a);
  assert(bottom.equals(a));
  auto reset = a; reset.set_to_top();
  assert(reset.live_children_is_top());
  auto unresolved = a; unresolved.set_spawned_top();
  assert(unresolved.live_children_is_top());
}
