#include <cassert>
#include <ikos/core/domain/concurrent_global_env.hpp>

int main() {
  using namespace ikos::core;
  for (unsigned width : {32u, 64u}) {
    ConcurrentGlobalEnv env;
    assert(env.read_global_interference(1, width, Signed).is_bottom());
    env.record_privatized_write(17, 1,
        machine_int::Interval(MachineInt(1, width, Signed)));
    env.record_privatized_write(0, 1,
        machine_int::Interval(MachineInt(0, width, Signed)));
    auto pending = env.read_global_interference(1, width, Signed);
    assert(pending.lb().is_zero() && pending.ub().to_z_number() == ZNumber(1));
    assert(env.read_global_interference(2, width, Signed).is_bottom());
    env.flush_privatized(17);
    env.flush_unlocked_partition();
    auto published = env.read_global_interference(1, width, Signed);
    assert(published == pending);
    env.record_privatized_write(23, 1,
        machine_int::Interval(MachineInt(-2, width, Signed)));
    auto mixed = env.read_global_interference(1, width, Signed);
    assert(mixed.lb().to_z_number() == ZNumber(-2));
    assert(mixed.ub().to_z_number() == ZNumber(1));
  }
}
