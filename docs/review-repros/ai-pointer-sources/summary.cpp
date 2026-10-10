#include <cassert>
#include <ikos/core/domain/concurrent_global_env.hpp>

int main() {
  using namespace ikos::core;
  ikos::analyzer::AbsoluteZeroMemoryLocation key;
  ikos::analyzer::ArgvMemoryLocation target;
  using Value = ConcurrentGlobalEnv::PointerValue;
  for (unsigned width : {32u, 64u}) {
    for (bool heap : {false, true}) {
      ConcurrentGlobalEnv env;
      auto put = [&](const Value& v) {
        if (heap) env.join_heap_pointer(17, v);
        else env.join_global_pointer(&key, v);
      };
      auto get = [&] {
        auto v = Value::top(width, Unsigned);
        assert(heap ? env.get_heap_pointer(17, v) : env.get_global_pointer(&key, v));
        return v;
      };
      auto pointer = [&](unsigned offset) {
        return Value(Uninitialized::initialized(), Nullity::non_null(),
                     PointsToSet<ikos::analyzer::MemoryLocation*>{&target},
                     machine_int::Interval(MachineInt(offset, width, Unsigned)));
      };
      put(pointer(0));
      assert(env.is_dirty());
      env.clear_dirty(); put(pointer(0));
      assert(!env.is_dirty());
      put(pointer(4));
      assert(env.is_dirty() && get().offset().ub().to_z_number() == ZNumber(4));
      env.clear_dirty(); put(Value::null(width, Unsigned));
      assert(env.is_dirty() && get().nullity().is_top());
      env.clear_dirty(); env.set_global_iteration(100); put(pointer(8));
      assert(env.is_dirty() && get().offset().is_top());
      env.clear_dirty(); put(Value::top(width, Unsigned));
      assert(env.is_dirty() && get().is_top());
      env.clear_dirty(); put(pointer(16));
      assert(!env.is_dirty() && get().is_top());
    }
  }
}
