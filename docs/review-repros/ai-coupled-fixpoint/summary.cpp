#include <cassert>
#include <ikos/ar/semantic/context.hpp>
#include <ikos/ar/semantic/statement.hpp>
#include <ikos/core/domain/concurrent_global_env.hpp>

int main() {
  using namespace ikos;
  using namespace core;
  for (unsigned width : {32u, 64u}) {
    ar::Context ctx;
    auto* bundle = ar::Bundle::create(ctx,
        ar::DataLayout::create(ar::LittleEndian, {width, width/8, width/8}), "test");
    auto* type = ar::FunctionType::get(ctx, ar::VoidType::get(ctx), {}, false);
    auto* function = ar::Function::create(bundle, type, "child", true);
    auto call = ar::Call::create(nullptr, function, {});
    auto call2 = ar::Call::create(nullptr, function, {});
    analyzer::AbsoluteZeroMemoryLocation key;
    analyzer::ArgvMemoryLocation target;
    using Pts = PointsToSet<analyzer::MemoryLocation*>;
    using Interval = machine_int::Interval;
    auto at = [&](unsigned n) { return Interval(MachineInt(n, width, Unsigned)); };
    ConcurrentGlobalEnv env;
    env.set_join_summary(function, {1,2});
    assert(env.is_dirty());
    env.clear_dirty(); env.set_join_summary(function, {1,2});
    assert(!env.is_dirty());
    env.set_join_summary(function, {1});
    assert(env.is_dirty() && env.get_join_summary(function).count(2) == 0);
    env.clear_dirty(); env.record_spawn_arg(function, Pts{&key}, at(0));
    assert(env.is_dirty() && env.is_escaped(&key));
    env.clear_dirty(); env.record_spawn_arg(function, Pts{&key}, at(0));
    assert(!env.is_dirty());
    env.record_spawn_arg(function, Pts{&key}, at(4));
    Pts pts = Pts::empty(); auto offset = Interval::bottom(width, Unsigned);
    assert(env.is_dirty() && env.get_spawn_arg(function, pts, offset) &&
           offset.ub().to_z_number() == ZNumber(4));
    env.clear_dirty(); env.record_spawn_arg(function, Pts{&target}, at(0));
    assert(env.is_dirty() && env.is_escaped(&target));
    env.clear_dirty(); env.record_spawn_arg(function, Pts::top(), Interval::top(width, Unsigned));
    assert(env.is_dirty() && env.get_spawn_arg(function, pts, offset) && pts.is_top());
    analyzer::AbsoluteZeroMemoryLocation other;
    assert(env.is_escaped(&other));
    env.clear_dirty(); env.record_spawn_arg(function, Pts{&key}, at(0));
    assert(!env.is_dirty());
    env.record_spawn_joined(function, call.get(), false, {1,2});
    assert(env.is_dirty());
    env.clear_dirty(); env.reset_spawn_counts();
    bool top; std::unordered_set<std::uint64_t> joined;
    assert(env.get_spawn_joined(function, top, joined) && !top && joined.size() == 2);
    assert(!env.is_dirty());
    env.record_spawn_joined(function, call.get(), false, {1});
    assert(env.is_dirty() && env.get_spawn_joined(function, top, joined) && joined == std::unordered_set<std::uint64_t>{1});
    env.clear_dirty(); env.record_spawn_joined(function, call.get(), false, {1,2});
    assert(!env.is_dirty());
    env.record_spawn_joined(function, call2.get(), false, {2});
    assert(env.is_dirty() && env.get_spawn_joined(function, top, joined) && joined.empty());
    env.clear_dirty(); env.record_spawn_joined(function, call.get(), true, {});
    assert(env.is_dirty() && env.get_spawn_joined(function, top, joined) && top && joined.empty());
    env.clear_dirty(); env.record_privatized_write(77, 88, at(3));
    env.flush_all_privatized_locks();
    assert(env.is_dirty());
    env.clear_dirty(); env.flush_all_privatized_locks();
    assert(!env.is_dirty());
    env.clear();
    assert(!env.get_spawn_joined(function, top, joined));
  }
}
