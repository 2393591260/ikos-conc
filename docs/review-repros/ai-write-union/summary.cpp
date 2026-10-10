#include <cassert>
#include <ikos/core/domain/concurrent_global_env.hpp>

int main() {
  using namespace ikos::core;
  for (unsigned width : {32u, 64u}) {
    for (std::uint64_t lock : {0u, 17u}) {
      for (bool reverse : {false, true}) {
        ConcurrentGlobalEnv env;
        auto zero = machine_int::Interval(MachineInt(0, width, Signed));
        auto one = machine_int::Interval(MachineInt(1, width, Signed));
        env.record_privatized_write(lock, 1, reverse ? zero : one);
        env.record_privatized_write(lock, 1, reverse ? one : zero);
        env.record_privatized_write(lock, 2, one);
        assert(env.flush_privatized(lock));
        auto board = env.snapshot_global_interference();
        assert(board.at(1).lb().to_z_number() == ZNumber(0));
        assert(board.at(1).ub().to_z_number() == ZNumber(1));
        assert(board.at(2).lb().to_z_number() == ZNumber(1));
        assert(board.at(2).ub().to_z_number() == ZNumber(1));
        env.clear_dirty();
        assert(!env.flush_privatized(lock));
        assert(!env.is_dirty());
      }
    }
  }
  ConcurrentGlobalEnv mixed;
  mixed.record_privatized_write(0, 1,
      machine_int::Interval(MachineInt(255, 8, Unsigned)));
  mixed.record_privatized_write(0, 1,
      machine_int::Interval(MachineInt(-1, 8, Signed)));
  mixed.record_privatized_write(0, 1,
      machine_int::Interval(MachineInt(1, 16, Signed)));
  assert(mixed.flush_unlocked_partition());
  auto values = mixed.read_global(1);
  assert(values.lb().to_z_number() == ZNumber(-1));
  assert(values.ub().to_z_number() == ZNumber(255));
}
