#include <cassert>
#include <ikos/core/domain/lifetime/separate_domain.hpp>
#include <ikos/core/domain/machine_int/interval.hpp>
#include <ikos/core/domain/memory/value.hpp>
#include <ikos/core/domain/nullity/separate_domain.hpp>
#include <ikos/core/domain/scalar/composite.hpp>
#include <ikos/core/domain/uninitialized/separate_domain.hpp>
#include <ikos/core/example/memory_factory.hpp>
#include <ikos/core/example/scalar/variable_factory.hpp>
#include <iostream>
#include <map>
#include <sstream>
namespace c = ikos::core;
using VF = c::example::scalar::VariableFactory;
using V = VF::VariableRef;
using MF = c::example::MemoryFactory;
using M = MF::MemoryLocationRef;
struct Cell {
  M base;
  c::MachineInt offset, size;
};
static std::map< V, Cell > cells;
struct Factory {
  VF variables;
  unsigned bits;
};
namespace ikos {
namespace core {
namespace memory {
template <>
struct CellVariableTraits< V, M > {
  static bool is_cell(V v) { return cells.count(v) != 0; }
  static M base(V v) { return cells.at(v).base; }
  static const MachineInt& offset(V v) { return cells.at(v).offset; }
  static const MachineInt& size(V v) { return cells.at(v).size; }
};
template <>
struct CellFactoryTraits< V, M, Factory* > {
  static V cell(Factory* f,
                M base,
                const MachineInt& offset,
                const MachineInt& size,
                Signedness sign) {
    std::ostringstream name;
    name << "cell." << base->index() << "." << offset << "." << size << "."
         << sign;
    V v = f->variables.get_dynamic(name.str(),
                                   size.to< unsigned >() * 8,
                                   sign,
                                   f->bits,
                                   Unsigned);
    cells.emplace(v, Cell{base, offset, size});
    return v;
  }
};
} // namespace memory
} // namespace core
} // namespace ikos
using U = c::uninitialized::SeparateDomain< V >;
using I = c::machine_int::IntervalDomain< V >;
using N = c::nullity::SeparateDomain< V >;
using S = c::scalar::CompositeDomain< V, M, U, I, N >;
using L = c::lifetime::SeparateDomain< M >;
using D = c::memory::ValueDomain< V, M, Factory*, S, L >;
using Lit = c::Literal< V, M >;
int main(int argc, char**) {
  unsigned checks = 0, failures = 0;
  for (unsigned bits : {32u, 64u}) {
    cells.clear();
    Factory f;
    f.bits = bits;
    MF objects;
    auto a = objects.get("a"), b = objects.get("b"), x = objects.get("x"),
         y = objects.get("y");
    V pa = f.variables.get_pointer("pa", bits, c::Unsigned),
      pb = f.variables.get_pointer("pb", bits, c::Unsigned);
    V px = f.variables.get_pointer("px", bits, c::Unsigned),
      py = f.variables.get_pointer("py", bits, c::Unsigned),
      out = f.variables.get_pointer("out", bits, c::Unsigned),
      unknown = f.variables.get_pointer("unknown", bits, c::Unsigned);
    V length = f.variables.get_int("length", bits, c::Unsigned);
    auto size = c::MachineInt(bits / 8, bits, c::Unsigned),
         one = c::MachineInt(1, bits, c::Unsigned),
         zero = c::MachineInt(0, bits, c::Unsigned);
    auto init = [&]() {
      D d(&f, S(U::top(), I::top(), N::top()), L::top());
      d.pointer_assign(pa, a, c::Nullity::non_null());
      d.pointer_assign(pb, b, c::Nullity::non_null());
      d.pointer_assign(px, x, c::Nullity::non_null());
      d.pointer_assign(py, y, c::Nullity::non_null());
      d.mem_uninitialize_reachable(pa);
      d.mem_uninitialize_reachable(pb);
      return d;
    };
    auto check = [&](bool correct) {
      ++checks;
      if (!correct) {
        ++failures;
        std::cout << "failed check " << checks << " (" << bits << " bit)\n";
      }
    };
    auto top = [&](D& d, V p) {
      d.mem_read(Lit::pointer_var(out), p, size);
      check(!d.is_bottom() && d.pointer_to_points_to(out).is_top());
    };
    auto exact = [&](D& d, V p, M target) {
      d.mem_read(Lit::pointer_var(out), p, size);
      check(!d.is_bottom() &&
            d.pointer_to_points_to(out) == c::PointsToSet< M >{target});
    };
    auto null = [&](D& d, V p) {
      d.mem_read(Lit::pointer_var(out), p, size);
      check(!d.is_bottom() && d.nullity_is_null(out));
    };
    {
      auto d = init();
      d.mem_write(pa,
                  Lit::machine_int(c::MachineInt(4096, bits, c::Unsigned)),
                  size);
      top(d, pa);
    }
    // Constant integer bytes can become a pointer without any cast operation.
    {
      auto d = init();
      d.mem_write(pa,
                  Lit::machine_int(c::MachineInt(4096, bits, c::Unsigned)),
                  size);
      d.mem_copy(pb, pa, Lit::machine_int(size));
      top(d, pb);
    }
    // A typed pointer write can splice an older pointer at a byte offset.
    {
      auto d = init();
      d.mem_write(pa, Lit::pointer_var(px), size);
      d.pointer_assign(pb, pa, one);
      d.mem_write(pb, Lit::pointer_var(py), size);
      top(d, pa);
    }
    {
      auto d = init();
      d.mem_write(pa, Lit::machine_int(zero), size);
      null(d, pa);
    }
    {
      auto d = init();
      d.mem_write(pa, Lit::pointer_var(px), size);
      exact(d, pa, x);
    }
    {
      auto d = init();
      d.mem_write(pa, Lit::pointer_var(px), size);
      d.mem_write(pb, Lit::pointer_var(py), size);
      d.mem_copy(pb, pa, Lit::machine_int(size));
      exact(d, pb, x);
    }
    {
      auto d = init();
      d.mem_write(pa, Lit::pointer_var(px), size);
      d.mem_write(pa, Lit::machine_int(c::MachineInt(0, 8, c::Unsigned)), one);
      top(d, pa);
    }
    {
      auto d = init();
      d.mem_write(pa, Lit::pointer_var(px), size);
      d.mem_set(pa,
                Lit::machine_int(c::MachineInt(0, 8, c::Unsigned)),
                Lit::machine_int(one));
      top(d, pa);
    }
    {
      auto d = init();
      d.mem_write(pa, Lit::pointer_var(px), size);
      d.mem_set(pa,
                Lit::machine_int(c::MachineInt(0, 8, c::Unsigned)),
                Lit::machine_int(size));
      null(d, pa);
    }
    {
      auto d = init();
      d.mem_write(pa, Lit::pointer_var(px), size);
      d.mem_write(pb, Lit::pointer_var(py), size);
      d.mem_copy(pb, pa, Lit::machine_int(one));
      top(d, pb);
    }
    {
      auto d = init();
      d.mem_write(pa, Lit::pointer_var(px), size);
      d.mem_write(pb, Lit::pointer_var(py), size);
      d.int_assign_nondet(length);
      d.int_set(length, c::machine_int::Interval(zero, size));
      d.mem_copy(pb, pa, Lit::machine_int_var(length));
      top(d, pb);
    }
    {
      auto d = init();
      d.mem_write(pa, Lit::pointer_var(px), size);
      d.pointer_assign_nondet(unknown);
      d.mem_set(unknown,
                Lit::machine_int(c::MachineInt(0, 8, c::Unsigned)),
                Lit::machine_int(one));
      top(d, pa);
    }
    {
      auto d = init();
      d.mem_write(pa, Lit::pointer_var(px), size);
      d.pointer_assign_nondet(unknown);
      d.mem_copy(unknown, pa, Lit::machine_int(one));
      top(d, pa);
    }
    {
      auto d = init();
      d.mem_write(pa, Lit::pointer_var(px), size);
      d.pointer_assign_nondet(unknown);
      d.mem_write(unknown,
                  Lit::machine_int(c::MachineInt(1, 8, c::Unsigned)),
                  one);
      top(d, pa);
    }
    {
      auto d = init();
      d.mem_write(pa, Lit::pointer_var(px), size);
      d.mem_set(pa,
                Lit::machine_int(c::MachineInt(255, 8, c::Unsigned)),
                Lit::machine_int(zero));
      exact(d, pa, x);
    }
    {
      auto d = init();
      d.mem_write(pa, Lit::pointer_var(px), size);
      d.mem_write(pb, Lit::pointer_var(py), size);
      d.mem_copy(pa, pb, Lit::machine_int(zero));
      exact(d, pa, x);
    }
  }
  std::cout << checks << " semantic checks; " << failures << " failures\n";
  return failures && argc == 1 ? 1 : 0;
}
