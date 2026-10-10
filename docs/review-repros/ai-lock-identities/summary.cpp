#include <cassert>
#include <ikos/analyzer/analysis/execution_engine/concurrent_semantics.hpp>
int main() {
  using namespace ikos;
  using core::ZNumber;
  using analyzer::offset_lock_key;
  assert(offset_lock_key(1, ZNumber(0)) != 0);
  assert(offset_lock_key(1, ZNumber(0)) != offset_lock_key(1, ZNumber(8)));
  assert(offset_lock_key(1, ZNumber(8)) != offset_lock_key(2, ZNumber(8)));
  auto max = offset_lock_key(0xFFFFFFFFULL, ZNumber(0xFFFFFFFFULL));
  assert(max != 0);
  assert(analyzer::lock_key_base(max) == 0xFFFFFFFFULL);
  assert(analyzer::lock_key_instance(max) == 0xFFFFFFFFULL);
  assert(offset_lock_key(1, ZNumber(4294967296ULL)) == 0);
  assert(offset_lock_key(1, ZNumber(4294967312ULL)) == 0);
  assert(offset_lock_key(1, ZNumber(-1)) == 0);
  assert(offset_lock_key(4294967296ULL, ZNumber(0)) == 0);
  assert(offset_lock_key(0, ZNumber(0)) == 0);
  assert(offset_lock_key(1, ZNumber::from_string("18446744073709551616")) == 0);
}
