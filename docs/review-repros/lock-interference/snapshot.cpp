// Published lock values must survive source type/order differences.
#include <algorithm>
#include <array>
#include <cassert>
#include <ikos/core/domain/concurrent_global_env.hpp>
int main() {
  using namespace ikos::core;
  using machine_int::Interval;
  std::array<unsigned, 3> order{{0, 1, 2}};
  do {
    ConcurrentGlobalEnv env;
    const std::array<Interval, 3> values{{Interval(MachineInt(255, 8, Unsigned)),
                                         Interval(MachineInt(-1, 8, Signed)),
                                         Interval(MachineInt(3, 16, Signed))}};
    for (unsigned i : order) {
      env.record_privatized_write(i + 1, 42, values[i]);
      env.flush_privatized(i + 1);
    }
    auto snapshot = env.snapshot_global_interference();
    const auto& value = snapshot.at(42);
    assert(value.bit_width() == 16 && value.sign() == Signed);
    assert(value.contains(MachineInt(255, 16, Signed)));
    assert(value.contains(MachineInt(-1, 16, Signed)));
    assert(value.contains(MachineInt(3, 16, Signed)));
  } while (std::next_permutation(order.begin(), order.end()));
}
