#include <cassert>
#include <ikos/core/domain/concurrent_global_env.hpp>

int main() {
  using namespace ikos::core;
  ikos::analyzer::AbsoluteZeroMemoryLocation key;
  ikos::analyzer::ArgvMemoryLocation target;
  {
    ConcurrentGlobalEnv env;
    assert(!env.is_synthetic_handle_slot(&key));
    env.record_synthetic_handle_slot(&key, 17);
    assert(env.is_dirty() && env.is_synthetic_handle_slot(&key));
    env.clear_dirty();
    env.record_synthetic_handle_slot(&key, 17);
    assert(!env.is_dirty());
    assert(env.synthetic_handle_slot_ids(&key).count(17));
    env.record_synthetic_handle_slot(&key, 19);
    assert(env.is_dirty() && env.synthetic_handle_slot_ids(&key).size()==2);
    env.reset_spawn_counts();
    assert(env.is_synthetic_handle_slot(&key));
    env.clear();
    assert(!env.is_synthetic_handle_slot(&key));
    assert(env.synthetic_handle_slot_ids(&key).empty());
  }
  using Value = ConcurrentGlobalEnv::PointerValue;
  for (unsigned width : {32u, 64u}) {
    for (bool heap : {false, true}) {
      ConcurrentGlobalEnv env;
      auto put = [&](const Value& v) {
        if (heap) env.join_heap_pointer({17, true, ZNumber(0)}, v);
        else env.join_global_pointer(&key, v);
      };
      auto get = [&] {
        auto v = Value::top(width, Unsigned);
        assert(heap ? env.get_heap_pointer({17, true, ZNumber(0)}, v) : env.get_global_pointer(&key, v));
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
    ConcurrentGlobalEnv env;
    ikos::analyzer::AbsoluteZeroMemoryLocation first;
    ikos::analyzer::ArgvMemoryLocation second;
    auto p = [&](ikos::analyzer::MemoryLocation* target) {
      return Value(Uninitialized::initialized(), Nullity::non_null(),
                   PointsToSet<ikos::analyzer::MemoryLocation*>{target},
                   machine_int::Interval(MachineInt(0, width, Unsigned)));
    };
    auto get = [&](ConcurrentGlobalEnv::HeapPointerKey key) {
      auto value = Value::bottom(width, Unsigned);
      assert(env.get_heap_pointer(key, value));
      return value;
    };
    env.join_heap_pointer({17, true, ZNumber(0)}, p(&first));
    env.join_heap_pointer({17, true, ZNumber(8)}, p(&second));
    assert(get({17, true, ZNumber(0)}).points_to().size() == 1);
    assert(get({17, false, ZNumber(0)}).points_to().size() == 2);
    env.join_heap_pointer({17, false, ZNumber(0)}, p(&second));
    assert(get({17, true, ZNumber(0)}).points_to().size() == 2);
    assert(get({17, true, ZNumber(16)}).points_to().contains(&second));
    env.join_heap_pointer({18, true, ZNumber(0)}, p(&first));
    assert(get({18, true, ZNumber(0)}).points_to().size() == 1);
    ZNumber large(4294967296ULL);
    env.join_heap_pointer({17, true, large}, p(&first));
    assert(get({17, true, large}).points_to().size() == 2);
    env.join_heap_pointer({18, true, large}, p(&second));
    assert(get({18, true, large}).points_to().contains(&second));
    assert(get({18, true, large}).points_to().size() == 1);
    assert(get({18, true, ZNumber(0)}).points_to().contains(&first));
    assert(get({18, true, ZNumber(0)}).points_to().size() == 1);
    env.join_heap_pointer({4294967313ULL, true, ZNumber(0)}, p(&first));
    assert(get({4294967313ULL, true, ZNumber(0)}).points_to().size() == 1);
    for (bool raw_first : {false, true}) {
      ConcurrentGlobalEnv bytes;
      auto raw = [&] {
        bytes.record_pointer_write(17, machine_int::Interval(MachineInt(1, width, Unsigned)),
                                   ZNumber(1), false);
      };
      if (raw_first) raw();
      bytes.join_heap_pointer({17, true, ZNumber(0)}, p(&first));
      if (!raw_first) raw();
      auto value = Value::bottom(width, Unsigned);
      assert(bytes.get_heap_pointer({17, true, ZNumber(0)}, value) && value.is_top());
      bytes.clear_dirty(); raw();
      assert(!bytes.is_dirty());
      bytes.reset_spawn_counts();
      assert(bytes.get_heap_pointer({17, true, ZNumber(0)}, value) && value.is_top());
      bytes.clear();
      assert(!bytes.get_heap_pointer({17, true, ZNumber(0)}, value));
    }
    {
      ConcurrentGlobalEnv bytes;
      auto value = Value::bottom(width, Unsigned);
      bytes.join_heap_pointer({17, true, ZNumber(0)}, p(&first));
      // Adjacent raw fields retain the pointer; shifted full pointer stores do not.
      bytes.record_pointer_write(17, machine_int::Interval(MachineInt(width / 8, width, Unsigned)),
                                 ZNumber(1), false);
      assert(bytes.get_heap_pointer({17, true, ZNumber(0)}, value) && !value.is_top());
      bytes.record_pointer_write(17, machine_int::Interval(MachineInt(0, width, Unsigned)),
                                 ZNumber(width / 8), true);
      assert(bytes.get_heap_pointer({17, true, ZNumber(0)}, value) && !value.is_top());
      bytes.record_pointer_write(17, machine_int::Interval(MachineInt(1, width, Unsigned)),
                                 ZNumber(width / 8), true);
      assert(bytes.get_heap_pointer({17, true, ZNumber(0)}, value) && value.is_top());
    }
    {
      ConcurrentGlobalEnv bytes;
      const auto base = IndexableTraits<ikos::analyzer::MemoryLocation*>::index(&key);
      bytes.record_pointer_write(base, machine_int::Interval(MachineInt(0, width, Unsigned)),
                                 ZNumber(1), false);
      bytes.join_global_pointer(&key, p(&first));
      auto value = Value::bottom(width, Unsigned);
      assert(bytes.get_global_pointer(&key, value) && value.is_top());
      bytes.clear();
      bytes.join_heap_pointer({17, true, ZNumber(0)}, p(&first));
      bytes.record_pointer_write(17, machine_int::Interval::top(width, Unsigned), ZNumber(1), false);
      assert(bytes.get_heap_pointer({17, true, ZNumber(0)}, value) && value.is_top());
    }
  }
}
