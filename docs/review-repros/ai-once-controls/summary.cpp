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
    auto first = env.register_once_thread(function, 17, true, ZNumber(0));
    assert(first != 0 && env.is_dirty() && env.is_thread_unique("child"));
    env.clear_dirty();
    assert(env.register_once_thread(function, 17, true, ZNumber(0)) == first);
    assert(!env.is_dirty());
    auto second = env.register_once_thread(function, 17, true, ZNumber(4));
    assert(second != first && env.is_dirty() && !env.is_thread_unique("child"));
    env.clear_dirty();
    assert(env.register_once_thread(function, 17, false, ZNumber(4)) == 0);
    assert(env.is_dirty() && env.get_func_instances("child").size() == 3);
    env.clear_dirty();
    env.record_unmodeled_effect(call.get(), "callback missing");
    assert(env.is_dirty() && env.unmodeled_effects().size() == 1);
    env.clear_dirty();
    env.record_unmodeled_effect(call.get(), "callback missing");
    assert(!env.is_dirty());
    env.record_unmodeled_effect(call2.get(), "control unknown");
    assert(env.is_dirty() && env.unmodeled_effects().size() == 2);
    auto* alternative = ar::Function::create(bundle, type, "alternative", true);
    env.clear_dirty();
    assert(env.register_once_thread(alternative, 17, true, ZNumber(0)) == first);
    assert(env.is_dirty() && env.get_func_instances("alternative").count(first) == 1);
    assert(env.site_func(first) == nullptr);
    assert(env.register_once_thread(alternative, 17, true, ZNumber(4)) == second);
    assert(!env.is_thread_unique("alternative"));
    env.clear();
    assert(env.unmodeled_effects().empty());
  }
}
