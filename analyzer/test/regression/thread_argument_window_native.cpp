// Copyright and license: see checker/data_race.hpp.
#include <iostream>
#include <stdexcept>
#include <ikos/core/domain/numeric/apron.hpp>
#include <ikos/core/domain/numeric/var_packing_domain.hpp>
#include <ikos/core/domain/machine_int/numeric_domain_adapter.hpp>
#include <ikos/core/example/machine_int/variable_factory.hpp>
using namespace ikos::core;
using V=example::machine_int::VariableFactory::VariableRef;
using P=numeric::ApronDomain<numeric::apron::PolkaPolyhedra,ZNumber,V>;
using N=numeric::VarPackingDomain<ZNumber,V,P>;
using M=machine_int::NumericDomainAdapter<V,N>;
using I=machine_int::Interval;
using B=machine_int::BinaryOperator;
using U=machine_int::UnaryOperator;
void need(bool b,const char* s){if(!b)throw std::runtime_error(s);}
int main(){
 unsigned checks=0;
 for(unsigned width:{32u,64u}){
  example::machine_int::VariableFactory f;
  auto anchor=f.get("anchor",width,Unsigned),p=f.get("p",width,Unsigned);
  auto a=f.get("wide.anchor",width+2,Signed),q=f.get("wide.p",width+2,Signed);
  auto delta=f.get("delta",width+2,Signed);
  ZNumber maximum=(ZNumber(1)<<width)-1;
  auto seed=[&](){M s(N::top());s.set(anchor,I(MachineInt(32,width,Unsigned),MachineInt(maximum-32,width,Unsigned)));s.assign(p,anchor);return s;};
  auto residual=[&](M s){s.apply(U::Cast,a,anchor);s.apply(U::Cast,q,p);s.apply(B::Sub,delta,q,a);return s.to_interval(delta);};
  for(int displacement:{0,4,12,-4}){
   auto s=seed();
   if(displacement>=0)s.apply(B::Add,p,p,MachineInt(displacement,width,Unsigned));
   else s.apply(B::Sub,p,p,MachineInt(-displacement,width,Unsigned));
   auto range=residual(s);need(range.singleton()&&range.singleton()->to_z_number()==ZNumber(displacement),"native relative offset lost");checks++;
  }
  auto left=seed(),right=seed();right.apply(B::Add,p,p,MachineInt(8,width,Unsigned));left.join_with(right);
  auto joined=residual(left);need(joined.lb().to_z_number()==0&&joined.ub().to_z_number()==8,"joined window lost");checks++;
  auto wide_left=seed(),wide_right=seed();wide_right.apply(B::Add,p,p,MachineInt(8,width,Unsigned));
  wide_left.widen_with(wide_right);
  auto widened=residual(wide_left);
  need(!widened.is_bottom()&&widened.lb().to_z_number()<=0&&widened.ub().to_z_number()>=8,"widening dropped possible residuals");checks++;
  auto unknown=seed();unknown.forget(p);need(!residual(unknown).singleton(),"rebound pointer falsely exact");checks++;
  auto wrapped=seed();wrapped.set(anchor,I(MachineInt(maximum,width,Unsigned)));wrapped.assign(p,anchor);
  wrapped.apply(B::Add,p,p,MachineInt(4,width,Unsigned));
  auto wrap=residual(wrapped);need(!wrap.singleton()||wrap.singleton()->to_z_number()!=4,"modular delta mistaken for mathematical delta");checks++;
 }
 std::cout<<"PASS "<<checks<<" native Polka machine-width anchor checks\n";
}
