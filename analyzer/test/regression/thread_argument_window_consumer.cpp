// Copyright and license: see checker/data_race.hpp.
#include <ikos/analyzer/checker/thread_argument_window_consumer.hpp>
#include <ikos/core/domain/numeric/apron.hpp>
#include <ikos/core/domain/numeric/var_packing_domain.hpp>
#include <ikos/core/domain/machine_int/numeric_domain_adapter.hpp>
#include <ikos/core/domain/scalar/composite.hpp>
#include <ikos/core/domain/uninitialized/separate_domain.hpp>
#include <ikos/core/domain/nullity/separate_domain.hpp>
#include <ikos/core/example/scalar/variable_factory.hpp>
#include <ikos/core/example/memory_factory.hpp>
#include <cstdlib>
#include <iostream>

using namespace ikos::analyzer;
using namespace ikos::core;
static void require(bool ok, const char* message) {
  if (!ok) { std::cerr << message << '\n'; std::exit(EXIT_FAILURE); }
}

using V = example::scalar::VariableFactory::VariableRef;
using R = example::MemoryFactory::MemoryLocationRef;
using P = numeric::ApronDomain<numeric::apron::PolkaPolyhedra,ZNumber,V>;
using N = numeric::VarPackingDomain<ZNumber,V,P>;
using M = machine_int::NumericDomainAdapter<V,N>;
using D = scalar::CompositeDomain<V,R,uninitialized::SeparateDomain<V>,M,
                                 nullity::SeparateDomain<V>>;

static void test_native_reobservation(unsigned bits) {
  int family, object;
  const std::string worker = "worker";
  const ZNumber stride(16);
  example::scalar::VariableFactory factory;
  auto pointer = factory.get_pointer("access",bits,Unsigned);
  auto anchor = factory.get_int("entry.anchor",bits,Unsigned);
  auto source = factory.get_int("shared.source",bits,Unsigned);
  auto wide_offset = factory.get_int("window.offset",bits+2,Signed);
  auto wide_anchor = factory.get_int("window.anchor",bits+2,Signed);
  auto delta = factory.get_int("window.delta",bits+2,Signed);
  using I = machine_int::Interval;
  auto observe = [&](const ConcurrentGlobalEnv& shared) {
    // A fresh native state, not a reused interval/complete flag. Shared facts
    // flow through actual native VarPack scalar transfers before the production
    // residual query is evaluated on a copy.
    D native(uninitialized::SeparateDomain<V>::top(),M(N::top()),
             nullity::SeparateDomain<V>::top());
    native.int_assign(anchor,MachineInt(32,bits,Unsigned));
    native.int_set(source,shared.read_mutex_handoff(101,{},bits,Unsigned,0));
    native.int_apply(machine_int::BinaryOperator::Add,
                     pointer->offset_var(),anchor,source);
    auto query = native;
    return native_thread_argument_window<D,V>(query,pointer->offset_var(),anchor,
                                         wide_offset,wide_anchor,delta);
  };
  ConcurrentGlobalEnv shared;
  shared.initialize_mutex_value(101,I(MachineInt(0,bits,Unsigned)),0);
  shared.enable_mutex_values();
  shared.clear_dirty();
  auto first = observe(shared);
  require(first.singleton() && first.singleton()->is_zero(),
          "Initial native observation must reflect shared delta zero");
  ZNumber lower=first.lb().to_z_number(), upper=first.ub().to_z_number();
  ThreadArgumentWindowConsumerState state{
      &family,worker,stride,&object,true,true,true};
  ThreadArgumentWindowAccessView a{&family,&object,worker,lower,upper,4};
  ThreadArgumentWindowAccessView b{&family,&object,worker,lower,upper,4};
  require(state.disjoint(a,b,shared,true),
          "Actual initial native window must authorize this pair");

  shared.join_mutex_ongoing(101,{201},I(MachineInt(16,bits,Unsigned)),0);
  require(shared.is_dirty() && !state.disjoint(a,b,shared,true),
          "Real late board write withdraws the previously successful proof");
  auto after = observe(shared);
  require(after.lb().to_z_number()==0 && after.ub().to_z_number()==16,
          "Native reobservation must reflect the changed shared source");
  lower=after.lb().to_z_number(); upper=after.ub().to_z_number();
  shared.clear_dirty();
  require(!state.disjoint(a,b,shared,false),
          "Clearing dirty cannot finish an analysis");
  require(!state.disjoint(a,b,shared,true),
          "Changed native window crosses the neighbor even after completion");

  // A separate newly analyzed round can regain precision. Production currently
  // withdraws on late dirty; this unit does not claim automatic reconvergence.
  ConcurrentGlobalEnv independent_round;
  independent_round.initialize_mutex_value(101,I(MachineInt(4,bits,Unsigned)),0);
  independent_round.enable_mutex_values();
  independent_round.clear_dirty();
  auto fresh = observe(independent_round);
  require(fresh.singleton() && fresh.singleton()->to_z_number()==4,
          "Independent native analysis must observe its own stable source");
  const ZNumber fresh_lower=fresh.lb().to_z_number(),fresh_upper=fresh.ub().to_z_number();
  ThreadArgumentWindowAccessView fresh_a{
      &family,&object,worker,fresh_lower,fresh_upper,4};
  ThreadArgumentWindowAccessView fresh_b{
      &family,&object,worker,fresh_lower,fresh_upper,4};
  require(state.disjoint(fresh_a,fresh_b,independent_round,true),
          "A newly observed stable independent round can authorize proof");
}

static void test_consumer(unsigned bits) {
  // Object identities are deliberately opaque: the consumer does not dereference
  // them or decide allocation/type validity. The producer certifies those facts.
  int family, other_family, object, other_object;
  const std::string worker = "worker", wrong_worker = "other";
  const ZNumber stride(16), zero(0), twelve(12), thirteen(13), negative(-4);
  ThreadArgumentWindowConsumerState state{
      &family, worker, stride, &object, true, true, true};
  ThreadArgumentWindowAccessView a{&family, &object, worker, zero, zero, 4};
  ThreadArgumentWindowAccessView b{&family, &object, worker, zero, zero, 4};
  ConcurrentGlobalEnv shared;
  bool complete = true;
  auto excludes = [&]() { return state.disjoint(a, b, shared, complete); };
  require(excludes(), "Certified separated instance windows must exclude");
  complete = false;
  require(!excludes(), "An incomplete analysis cannot authorize window proof");
  complete = true;

  // This block checks the stable-snapshot gate only. The separate
  // test_native_reobservation exercises changed facts and native recomputation.
  using I = machine_int::Interval;
  shared.initialize_mutex_value(101, I(MachineInt(0, bits, Signed)), 0);
  shared.enable_mutex_values();
  shared.clear_dirty();
  require(excludes(), "Stable shared snapshot remains eligible");
  shared.join_mutex_ongoing(101, {201}, I(MachineInt(1, bits, Signed)), 0);
  require(shared.is_dirty() && !excludes(),
          "A late shared write withdraws an actual window exclusion");
  complete = false;
  shared.clear_dirty();
  require(!excludes(), "Replay must finish after dirty is cleared");
  complete = true;
  require(excludes(), "The stable-snapshot gate may reopen; this is not native recomputation");

  state.valid = false;
  require(!excludes(), "An invalid creator observation is monotone withdrawal");
  shared.clear_dirty();
  require(!excludes(), "Clearing shared dirty does not restore invalid observation");
  state.valid = true;
  state.create_seen = false;
  require(!excludes(), "Missing creator observation cannot authorize proof");
  state.create_seen = true;
  state.step_seen = false;
  require(!excludes(), "Missing loop-step observation cannot authorize proof");
  state.step_seen = true;

  b.family = &other_family;
  require(!excludes(), "Another activation family cannot borrow this theorem");
  b.family = &family;
  b.object = &other_object;
  require(!excludes(), "Another object cannot borrow this coordinate");
  b.object = &object;
  b.width = 0;
  require(!excludes(), "Unknown width remains may-overlapping");
  b.width = 4;
  auto wrong_thread = ThreadArgumentWindowAccessView{
      &family, &object, wrong_worker, zero, zero, 4};
  require(!state.disjoint(a, wrong_thread, shared, true),
          "Another root cannot borrow this worker's coordinate");
  auto contained = ThreadArgumentWindowAccessView{
      &family, &object, worker, zero, twelve, 4};
  require(state.disjoint(contained, b, shared, true),
          "A full joined within-cell window must retain disjointness");
  auto crossing = ThreadArgumentWindowAccessView{
      &family, &object, worker, zero, thirteen, 4};
  require(!state.disjoint(crossing, b, shared, true),
          "A joined window crossing a neighbor remains may-overlapping");
  auto invalid = ThreadArgumentWindowAccessView{
      &family, &object, worker, thirteen, twelve, 4};
  require(!state.disjoint(invalid, b, shared, true),
          "An inverted observation is never a proof");
  auto before_entry = ThreadArgumentWindowAccessView{
      &family, &object, worker, negative, negative, 4};
  require(state.disjoint(before_entry, b, shared, true),
          "Mathematical negative residuals are not unsigned-wrap aliases");

  // A recorded unresolved create remains an obligation after dirty is cleared.
  shared.record_unmodeled_thread_creation(nullptr);
  shared.clear_dirty();
  require(!excludes(), "Unmodeled child coverage withdraws a window proof");
}

int main() {
  test_consumer(32);
  test_consumer(64);
  test_native_reobservation(32);
  test_native_reobservation(64);
  std::cout << "PASS dual-ABI native window consumer and real-board invalidation\n";
}
