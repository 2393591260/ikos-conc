#include <cassert>
#include <ikos/core/domain/concurrent_global_env.hpp>

int main() {
  using namespace ikos::core;
  ConcurrentGlobalEnv env;
  int keys[2]; // Opaque identities only; the environment never dereferences them.
  auto* a = reinterpret_cast<ikos::analyzer::MemoryLocation*>(&keys[0]);
  auto* b = reinterpret_cast<ikos::analyzer::MemoryLocation*>(&keys[1]);
  using Targets = PointsToSet<ikos::analyzer::MemoryLocation*>;
  assert(!env.is_escaped(a));
  env.record_possible_escape(Targets::bottom());
  assert(!env.is_dirty());
  env.record_possible_escape(Targets::empty());
  assert(!env.is_dirty());
  env.record_possible_escape(Targets{a});
  assert(env.is_dirty() && env.is_escaped(a) && !env.is_escaped(b));
  env.clear_dirty();
  env.record_possible_escape(Targets{a});
  assert(!env.is_dirty());
  env.record_possible_escape(Targets::top());
  assert(env.is_dirty() && env.is_escaped(b));
  env.clear();
  assert(!env.is_dirty() && !env.is_escaped(a) && !env.is_escaped(b));
}
