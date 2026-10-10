#include <cassert>
#include <ikos/core/domain/concurrent_global_env.hpp>

int main() {
  using namespace ikos::core;
  ConcurrentGlobalEnv env;
  ikos::analyzer::AbsoluteZeroMemoryLocation first;
  ikos::analyzer::ArgvMemoryLocation second;
  auto* a = &first;
  auto* b = &second;
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
