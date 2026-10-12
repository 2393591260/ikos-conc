#include <cassert>
#include <ikos/ar/semantic/context.hpp>
#include <ikos/analyzer/analysis/execution_engine/protected_values.hpp>
#include <ikos/core/domain/concurrent_global_env.hpp>
#include <ikos/core/domain/lockset/lockset_domain.hpp>

int main() {
  using namespace ikos;
  using namespace ikos::core;
  auto initial = lockset::LocksetDomain::top();
  initial.initialize_mutex_history();
  auto acquired = initial;
  acquired.add_mutex(17);
  acquired.remove_mutex(17);
  auto joined = initial;
  joined.join_with(acquired);
  assert(joined.held_mutexes().empty() && joined.may_ever_acquired_mutex());
  assert(initial.leq(joined) && acquired.leq(joined));
  auto contradictory = initial;
  auto held = acquired;
  held.add_mutex(17);
  contradictory.meet_with(held);
  assert(contradictory.is_bottom());
  assert(lockset::LocksetDomain::top().may_ever_acquired_mutex());

  for (unsigned width : {32u, 64u}) {
    ar::Context context;
    auto* bundle = ar::Bundle::create(context,
        ar::DataLayout::create(ar::LittleEndian, {width, width / 8, width / 8}), "test");
    auto* integer = ar::IntegerType::get(context, width, Signed);
    auto* pointer = ar::PointerType::get(context, integer);
    auto* function = ar::Function::create(bundle,
        ar::FunctionType::get(context, ar::VoidType::get(context), {pointer}, false),
        "output", true);
    auto* block = ar::BasicBlock::create(function->body());
    function->body()->set_entry_block(block);
    auto* local = ar::LocalVariable::create(function, pointer, width / 8);
    auto variable = [&](ar::Type* type) { return ar::InternalVariable::create(function->body(), type); };
    auto* copy = variable(pointer);
    block->push_back(ar::Assignment::create(copy, local));
    auto* cast = variable(pointer);
    block->push_back(ar::UnaryOperation::create(ar::UnaryOperation::Bitcast, cast, copy));
    auto* shifted = variable(pointer);
    block->push_back(ar::PointerShift::create(shifted, cast,
        {{MachineInt(1, width, Unsigned), variable(integer)}}));
    assert(analyzer::local_output_base(shifted) == local);
    // A formal also has an implicit entry definition, even if AR assigns it once.
    block->push_back(ar::Assignment::create(function->param(0), local));
    assert(analyzer::local_output_base(function->param(0)) == nullptr);
    auto* indirect = variable(pointer);
    auto* pointer_slot = ar::LocalVariable::create(function, ar::PointerType::get(context, pointer), width / 8);
    block->push_back(ar::Load::create(indirect, pointer_slot, width / 8, false));
    assert(analyzer::local_output_base(indirect) == nullptr);
    auto* ambiguous = variable(pointer);
    block->push_back(ar::Assignment::create(ambiguous, local));
    block->push_back(ar::Assignment::create(ambiguous, copy));
    assert(analyzer::local_output_base(ambiguous) == nullptr);
    assert(analyzer::local_output_base(variable(integer)) == nullptr);

    ConcurrentGlobalEnv env;
    auto interval = [&](int value) { return machine_int::Interval(MachineInt(value, width, Signed)); };
    env.initialize_mutex_value(1, interval(1));
    env.enable_mutex_values();
    env.join_mutex_ongoing(1, {17, 23}, interval(0));
    env.join_mutex_release(1, {17}, interval(0));
    env.join_mutex_release(1, {}, interval(1));
    // The inner release is invisible to a reader still holding the outer lock.
    assert(env.read_mutex_handoff(1, {17}, width, Signed) == interval(1));
    assert(env.read_mutex_ongoing(1, {17}, width, Signed).is_bottom());
    env.clear_dirty();
    // A late unprotected writer matters even when its value was already known.
    env.join_mutex_ongoing(1, {}, interval(0));
    assert(env.is_dirty());
    assert(env.read_mutex_ongoing(1, {17}, width, Signed) == interval(0));
    env.invalidate_mutex_values();
    assert(!env.mutex_values_available());
    assert(env.read_mutex_handoff(1, {17}, width, Signed).is_top());
  }
}
