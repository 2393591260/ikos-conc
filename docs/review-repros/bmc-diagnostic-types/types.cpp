#include <cassert>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <ikos/ar/semantic/context.hpp>
#include <ikos/ar/semantic/type.hpp>

using namespace ikos::ar;
std::string render(Type* type) { std::ostringstream out; type->dump(out); return out.str(); }
struct ThrowBuffer : std::streambuf {
  int overflow(int) override { throw std::runtime_error("test stream failure"); }
};
int main() {
  Context context;
  auto* i32 = IntegerType::si32(context);
  auto* node = StructType::create(context, false);
  node->set_layout({{ZNumber(0), PointerType::get(context, node)}, {ZNumber(8), i32}});
  assert(render(node) == "{0: {...}*, 8: si32}");
  auto* a = StructType::create(context, false);
  auto* b = StructType::create(context, true);
  a->set_layout({{ZNumber(0), PointerType::get(context, b)}});
  b->set_layout({{ZNumber(0), PointerType::get(context, a)}});
  assert(render(a) == "{0: <{0: {...}*}>*}");
  auto* leaf = StructType::create(context, {{ZNumber(0), i32}}, false);
  auto* repeated = StructType::create(context, {{ZNumber(0), leaf}, {ZNumber(4), leaf}}, false);
  assert(render(repeated) == "{0: {0: si32}, 4: {0: si32}}");
  ThrowBuffer buffer;
  std::ostream failed(&buffer); failed.exceptions(std::ios::badbit | std::ios::failbit);
  bool threw = false;
  try { node->dump(failed); } catch (...) { threw = true; }
  assert(threw && render(node) == "{0: {...}*, 8: si32}");
  auto work = [&] { for (unsigned i = 0; i < 100; ++i) assert(render(node) == "{0: {...}*, 8: si32}"); };
  std::thread first(work), second(work); first.join(); second.join();
}
