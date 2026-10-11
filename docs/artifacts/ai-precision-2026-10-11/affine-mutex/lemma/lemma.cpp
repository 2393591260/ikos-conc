#include <z3++.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  z3::context c;
  auto check = [&](const char* name, unsigned bits, unsigned period,
                   unsigned width_a, unsigned width_b, unsigned shift,
                   z3::check_result expected) {
    auto a = c.bv_const("a", bits), b = c.bv_const("b", bits);
    auto x = c.bv_const("byte_a", bits), y = c.bv_const("byte_b", bits);
    z3::solver s(c);
    s.add(z3::urem(a,c.bv_val(period,bits)) ==
          z3::urem(b+c.bv_val(shift,bits),c.bv_val(period,bits)));
    s.add(z3::ult(x,c.bv_val(width_a,bits)) && z3::ult(y,c.bv_val(width_b,bits)));
    s.add(a+x == b+y); // actual overlapping byte, including address wrap
    s.add(a != b);
    auto result = s.check();
    if (result != expected) throw std::runtime_error(name);
    std::ofstream(std::string(argv[1])+"/"+name+".smt2") << s.to_smt2();
    std::cout << name << " " << result << "\n";
  };
  check("congruent_32",32,4,4,4,0,z3::unsat);
  check("congruent_64",64,8,4,8,0,z3::unsat);
  check("unequal_widths",32,4,1,4,0,z3::unsat);
  check("different_residue",32,4,4,4,1,z3::sat);
  check("width_exceeds_modulus",32,4,8,4,0,z3::sat);
  check("missing_pointer_modulus",4,3,3,3,0,z3::sat);
}
