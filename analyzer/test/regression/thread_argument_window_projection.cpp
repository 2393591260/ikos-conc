// Copyright and license: see checker/data_race.hpp.
#include <iostream>
#include <stdexcept>
#include <ikos/core/domain/numeric/apron.hpp>
#include <ikos/core/domain/numeric/var_packing_domain.hpp>
#include <ikos/core/domain/machine_int/numeric_domain_adapter.hpp>
#include <ikos/core/domain/scalar/composite.hpp>
#include <ikos/core/domain/uninitialized/separate_domain.hpp>
#include <ikos/core/domain/nullity/separate_domain.hpp>
#include <ikos/core/example/scalar/variable_factory.hpp>
#include <ikos/core/example/memory_factory.hpp>
using namespace ikos::core;
using V=example::scalar::VariableFactory::VariableRef;
using R=example::MemoryFactory::MemoryLocationRef;
using P=numeric::ApronDomain<numeric::apron::PolkaPolyhedra,ZNumber,V>;
using N=numeric::VarPackingDomain<ZNumber,V,P>;
using M=machine_int::NumericDomainAdapter<V,N>;
using D=scalar::CompositeDomain<V,R,uninitialized::SeparateDomain<V>,M,nullity::SeparateDomain<V>>;
int main(){
 unsigned checks=0;
 for(unsigned bits:{32u,64u}) {
  example::scalar::VariableFactory f;
  auto p=f.get_pointer("p",bits,Unsigned), a=f.get_int("anchor",bits,Unsigned);
  for(auto u:{Uninitialized::top(),Uninitialized::initialized(),Uninitialized::uninitialized()})
  for(auto n:{Nullity::top(),Nullity::null(),Nullity::non_null()})
  for(int offset:{-1,0,16}) {
   D before(uninitialized::SeparateDomain<V>::top(), M(N::top()),nullity::SeparateDomain<V>::top());
   before.uninit_refine(p,u);
   before.nullity_refine(p,n);
   if(offset>=0) before.int_assign(p->offset_var(),MachineInt(offset,bits,Unsigned));
   auto after=before;
   after.int_assign(a,p->offset_var());
   after.uninit_assign_maybe(a);
   after.uninit_refine(a,Uninitialized::initialized());
   if(!after.uninit_to_uninitialized(p).equals(u))throw std::runtime_error("formal initialization was refined");
   if(!after.nullity_to_nullity(p).equals(n))throw std::runtime_error("formal nullity was refined");
   after.scalar_forget(a);
   if(!before.equals(after))throw std::runtime_error("snapshot projection changed old state");
   ++checks;
  }
 }
 std::cout<<"PASS "<<checks<<" snapshot projection/initialization checks\n";
}
