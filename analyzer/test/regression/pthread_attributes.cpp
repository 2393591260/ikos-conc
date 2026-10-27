// Copyright and license: see analysis/execution_engine/pthread_attributes.hpp.
#include <ikos/analyzer/analysis/execution_engine/pthread_attributes.hpp>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <ikos/core/domain/machine_int/interval.hpp>
#include <ikos/core/domain/scalar/composite.hpp>
#include <ikos/core/domain/uninitialized/separate_domain.hpp>
#include <ikos/core/domain/nullity/separate_domain.hpp>
#include <ikos/core/domain/lifetime/separate_domain.hpp>
#include <ikos/core/domain/memory/value.hpp>
#include <ikos/core/domain/exception/exception.hpp>
#include <ikos/core/domain/domain_product.hpp>
#include <ikos/core/domain/lockset/lockset_domain.hpp>
#include <cstdlib>
#include <iostream>
#include <string>
using namespace ikos::analyzer;
namespace ar = ikos::ar;
namespace core = ikos::core;
static void require(bool ok, const std::string& message) {
  if (!ok) { std::cerr << message << '\n'; std::exit(EXIT_FAILURE); }
}

struct Fixture {
  llvm::LLVMContext llvm_context;
  llvm::Module module{"attribute-contract", llvm_context};
  ar::Context context;
  ar::Bundle* bundle;
  ar::Function* main;
  ar::LocalVariable* local;
  ar::CallBase* init;
  ar::CallBase* destroy;
  unsigned bits, bytes;
  Fixture(unsigned bits, const std::string& mode) : bits(bits), bytes(bits == 32 ? 36 : 56) {
    module.setTargetTriple(bits == 32 ? "i386-pc-linux-gnu" : "x86_64-pc-linux-gnu");
    module.setDataLayout(bits == 32 ? "e-p:32:32-i64:32-n8:16:32-S128" : "e-p:64:64-i64:64-n8:16:32:64-S128");
    auto* i32 = llvm::Type::getInt32Ty(llvm_context);
    auto* word = llvm::IntegerType::get(llvm_context, bits);
    auto* tail = llvm::ArrayType::get(llvm::Type::getInt8Ty(llvm_context), bytes - bits / 8);
    auto* object = llvm::StructType::create(llvm_context, {word, tail}, "union.pthread_attr_t");
    auto* pointer = llvm::PointerType::getUnqual(object);
    auto* signature = llvm::FunctionType::get(i32, {pointer}, false);
    auto* init_fn = llvm::Function::Create(signature, llvm::GlobalValue::ExternalLinkage, "pthread_attr_init", module);
    auto* destroy_fn = llvm::Function::Create(signature, llvm::GlobalValue::ExternalLinkage, "pthread_attr_destroy", module);
    auto* native_main = llvm::Function::Create(llvm::FunctionType::get(i32, false), llvm::GlobalValue::ExternalLinkage, "main", module);
    auto* entry = llvm::BasicBlock::Create(llvm_context, "entry", native_main);
    auto* success = llvm::BasicBlock::Create(llvm_context, "success", native_main);
    auto* failed = llvm::BasicBlock::Create(llvm_context, "failed", native_main);
    llvm::IRBuilder<> ir(entry);
    auto* storage = ir.CreateAlloca(object); storage->setAlignment(llvm::Align(mode == "native-align" ? 1 : bits / 8));
    if (mode == "main-recursion") ir.CreateCall(native_main);
    if (mode == "other-calls-main") {
      auto* helper = llvm::Function::Create(native_main->getFunctionType(), llvm::GlobalValue::ExternalLinkage, "helper", module);
      llvm::IRBuilder<> h(llvm::BasicBlock::Create(llvm_context, "entry", helper)); h.CreateRet(h.CreateCall(native_main));
    }
    if (mode == "frameaddress") {
      auto* f = llvm::Intrinsic::getDeclaration(&module, llvm::Intrinsic::frameaddress, {ir.getInt8PtrTy()});
      ir.CreateCall(f, {ir.getInt32(0)});
    }
    if (mode == "returns-twice") init_fn->addFnAttr(llvm::Attribute::ReturnsTwice);
    if (mode == "module-asm") module.setModuleInlineAsm("nop");
    if (mode == "nested-forged-constant") {
      auto* forged = llvm::ConstantExpr::getIntToPtr(llvm::ConstantInt::get(word, 4096), ir.getInt8PtrTy());
      auto* array_type = llvm::ArrayType::get(ir.getInt8PtrTy(), 1);
      new llvm::GlobalVariable(module, array_type, false, llvm::GlobalValue::ExternalLinkage,
                               llvm::ConstantArray::get(array_type, {forged}), "forged");
    }
    if (mode == "missing-lifetime") ir.CreateLifetimeStart(storage, ir.getInt64(bytes));
    auto* native_init = ir.CreateCall(init_fn, {storage});
    auto* cmp = llvm::cast< llvm::ICmpInst >(ir.CreateICmpEQ(native_init, ir.getInt32(0)));
    ir.CreateCondBr(cmp, success, failed);
    ir.SetInsertPoint(success);
    // Real native-only lifecycle calls: deliberately absent from imported AR.
    // The builder must inspect native users, not trust the partial AR index.
    if (mode == "native-only-joinable-setter" || mode == "native-only-detached-setter") {
      auto* setter_type = llvm::FunctionType::get(i32, {pointer, i32}, false);
      auto* setter = llvm::Function::Create(setter_type, llvm::GlobalValue::ExternalLinkage,
                                            "pthread_attr_setdetachstate", module);
      ir.CreateCall(setter, {storage, ir.getInt32(mode == "native-only-detached-setter" ? 1 : 0)});
    }
    if (mode == "native-only-destroy") ir.CreateCall(destroy_fn, {storage});
    auto* native_destroy = ir.CreateCall(destroy_fn, {storage});
    ir.CreateRet(ir.getInt32(0));
    ir.SetInsertPoint(failed); ir.CreateRet(ir.getInt32(1));
    if (mode == "native-bypass") {
      auto* bypass = llvm::BasicBlock::Create(llvm_context, "bypass", native_main, entry);
      llvm::IRBuilder<> b(bypass);
      auto* choose = llvm::Function::Create(llvm::FunctionType::get(b.getInt1Ty(), false), llvm::GlobalValue::ExternalLinkage, "choose", module);
      b.CreateCondBr(b.CreateCall(choose), entry, success);
      // This also moves allocation out of the actual entry; either premise must refuse.
    }

    bundle = ar::Bundle::create(context, ar::DataLayout::create(ar::LittleEndian, {bits, bits / 8, bits / 8}), "");
    auto* ar_word = bits == 32 ? ar::IntegerType::ui32(context) : ar::IntegerType::ui64(context);
    auto* ar_tail = ar::ArrayType::get(context, ar::IntegerType::ui8(context), ar::ZNumber(bytes - bits / 8));
    auto* ar_object = ar::StructType::create(context, {{ar::ZNumber(0), ar_word}, {ar::ZNumber(bits / 8), ar_tail}}, false);
    auto* ar_pointer = ar::PointerType::get(context, ar_object);
    auto* result_type = mode == "unsigned-result" ? ar::IntegerType::ui32(context) : ar::IntegerType::si32(context);
    auto* ar_signature = ar::FunctionType::get(context, result_type, {ar_pointer}, false);
    auto* ai = ar::Function::create(bundle, ar_signature, "pthread_attr_init", false); ai->set_frontend(init_fn);
    auto* ad = ar::Function::create(bundle, ar_signature, "pthread_attr_destroy", false); ad->set_frontend(destroy_fn);
    main = ar::Function::create(bundle, ar::FunctionType::get(context, result_type, {}, false), "main", true);
    main->set_frontend(native_main);
    auto* ae = ar::BasicBlock::create(main->body());
    auto* as = ar::BasicBlock::create(main->body());
    auto* af = ar::BasicBlock::create(main->body());
    main->body()->set_entry_block(ae);
    ae->add_successor(as); ae->add_successor(af);
    if (mode == "ar-entry-cycle") as->add_successor(ae);
    local = ar::LocalVariable::create(main, ar_pointer, mode == "ar-align" ? 1 : bits / 8); local->set_frontend(storage);
    auto* one = ar::IntegerConstant::get(context, ar_word, 1);
    auto allocation = ar::Allocate::create(local, ar_object, one);
    allocation->set_frontend(storage);
    (mode == "late-allocation" ? as : ae)->push_back(std::move(allocation));
    auto* r = ar::InternalVariable::create(main->body(), result_type); r->set_frontend(native_init);
    auto init_owner = ar::Call::create(r, ar::FunctionPointerConstant::get(context, ai), {local});
    if (mode != "missing-init-map") init_owner->set_frontend(native_init);
    init = init_owner.get(); ae->push_back(std::move(init_owner));
    auto* zero = ar::IntegerConstant::get(context, result_type, 0);
    ar::Value* guarded_result = r;
    if (mode == "duplicate-result") ae->push_back(ar::Assignment::create(r, zero));
    if (mode == "result-copy" || mode == "clobbered-copy" || mode == "forged-copy-annotation") {
      auto* copy = ar::InternalVariable::create(main->body(), result_type); copy->set_frontend(native_init);
      auto assignment = ar::Assignment::create(copy, mode == "forged-copy-annotation" ? static_cast< ar::Value* >(zero) : r);
      assignment->set_frontend(native_init); ae->push_back(std::move(assignment));
      if (mode == "clobbered-copy") ae->push_back(ar::Assignment::create(copy, zero));
      guarded_result = copy;
    }
    auto guard = ar::Comparison::create(ar::Comparison::SIEQ, guarded_result, zero); guard->set_frontend(cmp);
    auto destroy_owner = ar::Call::create(nullptr, ar::FunctionPointerConstant::get(context, ad), {local});
    destroy_owner->set_frontend(native_destroy); destroy = destroy_owner.get();
    if (mode == "consumer-before-guard") {
      as->push_back(std::move(destroy_owner)); as->push_back(std::move(guard));
    } else if (mode == "ar-bypass") {
      ae->push_back(std::move(destroy_owner)); as->push_back(std::move(guard));
    } else { as->push_back(std::move(guard)); as->push_back(std::move(destroy_owner)); }
    if (mode == "extra-ar-use") as->push_back(ar::Store::create(local, ar::UndefinedConstant::get(context, ar_object), bits / 8, false));
    if (mode == "duplicate-call-map") {
      auto extra = ar::Call::create(nullptr, ar::FunctionPointerConstant::get(context, ad), {local});
      extra->set_frontend(native_destroy); as->push_back(std::move(extra));
    }
    if (mode == "ar-main-exposure") {
      auto* copy = ar::InternalVariable::create(main->body(), ar::PointerType::get(context, main->type()));
      as->push_back(ar::Assignment::create(copy, ar::FunctionPointerConstant::get(context, main)));
    }
    as->push_back(ar::ReturnValue::create(zero)); af->push_back(ar::ReturnValue::create(zero));
  }
};

static void test_certificates(unsigned bits) {
  for (const std::string mode : {"positive", "result-copy", "duplicate-result", "clobbered-copy",
       "forged-copy-annotation", "consumer-before-guard", "ar-bypass", "native-bypass",
       "main-recursion", "other-calls-main", "ar-main-exposure", "frameaddress", "returns-twice",
       "module-asm", "nested-forged-constant", "missing-lifetime", "late-allocation", "ar-entry-cycle",
       "missing-init-map", "extra-ar-use", "duplicate-call-map", "native-align", "ar-align", "unsigned-result",
       "native-only-joinable-setter", "native-only-detached-setter", "native-only-destroy"}) {
    Fixture f(bits, mode); SourceProgramFacts facts(f.bundle);
    auto effects = build_closed_pthread_attribute_effects(f.bundle, f.main, facts);
    const bool expected = mode == "positive" || mode == "result-copy";
    require((effects.size() == 2) == expected && (expected || effects.empty()), mode);
    if (expected) {
      require(effects.at(f.init).storage == f.local && effects.at(f.destroy).bytes == f.bytes,
              "Actual allocation/whole extent must bind both effects");
    }
    if (mode == "positive") {
      // Find the completed proof's boundary, then stop one work item earlier.
      // This reaches closure/CFG work, rather than only failing the initial index.
      std::size_t low = 0, high = 200000;
      while (low < high) {
        const auto middle = low + (high - low) / 2;
        SourceProgramFacts bounded(f.bundle);
        if (build_closed_pthread_attribute_effects(f.bundle, f.main, bounded, middle).size() == 2)
          high = middle;
        else low = middle + 1;
      }
      require(low > 8 && low < 200000, "Finite closure proof has a positive work boundary");
      SourceProgramFacts before_boundary(f.bundle);
      require(build_closed_pthread_attribute_effects(f.bundle, f.main, before_boundary, low - 1).empty(),
              "A proof interrupted at its last work item cannot publish effects");
    }
    for (std::size_t limit : {0u, 1u, 8u}) {
      SourceProgramFacts bounded(f.bundle);
      require(build_closed_pthread_attribute_effects(f.bundle, f.main, bounded, limit).empty(),
              "Exhausted complete-program/call/use/CFG work never grants a prefix certificate");
    }
  }
}

using U = core::uninitialized::SeparateDomain< Variable* >;
using N = core::nullity::SeparateDomain< Variable* >;
using I = core::machine_int::IntervalDomain< Variable* >;
using S = core::scalar::CompositeDomain< Variable*, MemoryLocation*, U, I, N >;
using L = core::lifetime::SeparateDomain< MemoryLocation* >;
using M = core::memory::ValueDomain< Variable*, MemoryLocation*, VariableFactory*, S, L >;
using D = core::exception::ExceptionDomain< M >;

static void test_current_ranges(unsigned bits) {
  Fixture f(bits, "positive");
  VariableFactory variables(f.bundle); MemoryFactory memory;
  LiteralFactory literals(variables, f.bundle->data_layout());
  const auto& pointer = literals.get_scalar(f.local);
  auto* location = memory.get_local(f.local);
  auto* size = variables.get_alloc_size(location);
  auto* other_local = ar::LocalVariable::create(f.main, f.local->type(), bits / 8);
  auto* other_location = memory.get_local(other_local);
  auto top_memory = [&]() { return M(&variables, S(U::top(), I::top(), N::top()), L::top()); };
  auto base = top_memory();
  base.pointer_assign(pointer.var(), location, core::Nullity::non_null());
  base.lifetime_assign_allocated(location);
  base.int_assign(size, core::MachineInt(f.bytes, bits, core::Unsigned));
  auto bottom = top_memory(); bottom.set_to_bottom();
  D good(base, bottom, bottom);
  ClosedPthreadAttributeEffect effect{f.local, f.bytes, bits / 8};
  auto matches = [&](const D& state) {
    return closed_pthread_attribute_range(effect, pointer, state, memory, variables);
  };
  auto before = good;
  require(matches(good) && good.equals(before), "Accepted range query is pure");
  for (unsigned mode = 0; mode != 12; ++mode) {
    auto bad = good;
    switch (mode) {
      case 0: bad.set_normal_flow_to_bottom(); break;
      case 1: bad.normal().uninit_assign_maybe(pointer.var()); break;
      case 2: bad.normal().nullity_set(pointer.var(), core::Nullity::top()); break;
      case 3: bad.normal().pointer_forget(pointer.var()); break;
      case 4: bad.normal().pointer_assign(pointer.var(), pointer.var(), core::MachineInt(1, bits, core::Unsigned)); break;
      case 5: bad.normal().int_forget(size); break;
      case 6: bad.normal().int_assign(size, core::MachineInt(f.bytes - 1, bits, core::Unsigned)); break;
      case 7: bad.normal().lifetime_assign_deallocated(location); break;
      case 8: bad.normal().lifetime_forget(location); break;
      case 9: bad.normal().pointer_assign(pointer.var(), other_location, core::Nullity::non_null()); break;
      case 10: {
        auto other = good;
        other.normal().pointer_assign(pointer.var(), other_location, core::Nullity::non_null());
        bad.join_with(other); break;
      }
      case 11: {
        auto other = good;
        other.normal().pointer_assign(pointer.var(), pointer.var(), core::MachineInt(1, bits, core::Unsigned));
        bad.join_with(other); break;
      }
    }
    before = bad;
    require(!matches(bad) && bad.equals(before), "Broken current range is refused without refinement");
  }
  require(matches(good), "Refusal does not mutate immutable certificate or another state's range");
  // Seed first byte, last byte (including any ABI padding), and another local.
  auto* byte_type = ar::IntegerType::ui8(f.context);
  auto* byte_pointer = ar::PointerType::get(f.context, byte_type);
  auto variable = [&](ar::Type* type) {
    return variables.get_internal(ar::InternalVariable::create(f.main->body(), type));
  };
  auto* last = variable(byte_pointer);
  auto* neighbor = variable(byte_pointer);
  auto* read = variable(byte_type);
  auto& state = good.normal();
  state.pointer_assign(last, pointer.var(), core::MachineInt(f.bytes - 1, bits, core::Unsigned));
  state.pointer_assign(neighbor, other_location, core::Nullity::non_null());
  state.lifetime_assign_allocated(other_location);
  const auto byte = core::MachineInt(1, bits, core::Unsigned);
  const auto seed = ScalarLit::machine_int(core::MachineInt(42, 8, core::Unsigned));
  state.mem_write(pointer.var(), seed, byte); state.mem_write(last, seed, byte); state.mem_write(neighbor, seed, byte);
  core::ConcurrentGlobalEnv concurrent;
  havoc_closed_pthread_attribute_bytes(effect, pointer, good, memory, concurrent, bits);
  state.mem_read(ScalarLit::machine_int_var(read), pointer.var(), byte);
  require(state.int_to_interval(read).is_top(), "The first attribute byte is forgotten");
  state.mem_read(ScalarLit::machine_int_var(read), last, byte);
  require(state.int_to_interval(read).is_top(), "The last attribute byte and padding are forgotten");
  state.mem_read(ScalarLit::machine_int_var(read), neighbor, byte);
  auto retained = state.int_to_interval(read).singleton();
  require(retained && retained->to_z_number() == 42, "An unrelated private local is preserved");

  // Cross-component fallback chronology, using the actual persistent debt API.
  // The ordinary external fallback records this reason. We supply that step
  // explicitly here: this is not a NumericalEngine/DataRaceChecker harness.
  core::ConcurrentGlobalEnv debt_env;
  auto rejected = good;
  rejected.normal().nullity_set(pointer.var(), core::Nullity::top());
  require(!matches(rejected), "The earlier transfer range must really fail");
  debt_env.record_unmodeled_effect(f.init, "external call effects not covered");
  const auto debt_before = debt_env.unmodeled_effects();
  require(debt_before.size() == 1 && debt_before.front().first == f.init,
          "Fallback retains a real call-scoped obligation in ConcurrentGlobalEnv");
  require(matches(good), "A later precise visit can independently match");
  havoc_closed_pthread_attribute_bytes(effect, pointer, good, memory, debt_env, bits);
  require(debt_env.unmodeled_effects() == debt_before,
          "A later successful attribute effect cannot erase earlier fallback debt");
  debt_env.clear_dirty();
  require(debt_env.unmodeled_effects() == debt_before,
          "Global iteration dirty reset cannot discharge an effect obligation");

  // A stale published pointer in the byte footprint must become unusable at
  // the real summary consumer, even when its numeric memory is already TOP.
  using PV = core::ConcurrentGlobalEnv::PointerValue;
  auto* pointer_type = ar::PointerType::get(f.context, byte_type);
  auto* slot_type = ar::PointerType::get(f.context, pointer_type);
  auto* slot = ar::InternalVariable::create(f.main->body(), slot_type);
  auto* other_slot = ar::InternalVariable::create(f.main->body(), slot_type);
  auto* null_value = ar::NullConstant::get(f.context, pointer_type);
  auto writer = ar::Store::create(slot, null_value, bits / 8, false);
  auto other_writer = ar::Store::create(other_slot, null_value, bits / 8, false);
  core::ConcurrentGlobalEnv publication;
  publication.join_publication_pointer(writer.get(), PV::null(bits, core::Unsigned));
  publication.join_publication_pointer(other_writer.get(), PV::null(bits, core::Unsigned));
  auto value = PV::null(bits, core::Unsigned);
  require(publication.get_publication_pointer(writer.get(), location, value),
          "The overlapping publication summary must be genuinely available before havoc");
  require(publication.get_publication_pointer(other_writer.get(), other_location, value),
          "The unrelated publication summary must be genuinely available before havoc");
  havoc_closed_pthread_attribute_bytes(effect, pointer, good, memory, publication, bits);
  require(!publication.get_publication_pointer(writer.get(), location, value),
          "Attribute byte havoc invalidates an overlapping published pointer source");
  require(publication.get_publication_pointer(other_writer.get(), other_location, value),
          "Attribute byte havoc preserves an unrelated publication source");

  // Exercise the applicable mutex-value invalidation interface independently;
  // real certificate closure would normally preclude such a stale typed cell.
  core::ConcurrentGlobalEnv typed_values;
  typed_values.enable_mutex_values();
  const auto address = core::IndexableTraits< MemoryLocation* >::index(location);
  typed_values.initialize_mutex_value(address,
      core::machine_int::Interval(core::MachineInt(9, 32, core::Signed)));
  require(typed_values.has_mutex_value_cell(address) && typed_values.mutex_values_available(),
          "A real typed summary must exist before testing its invalidation");
  havoc_closed_pthread_attribute_bytes(effect, pointer, good, memory, typed_values, bits);
  require(!typed_values.mutex_values_available(),
          "Opaque attribute bytes invalidate any stale typed mutex-value summary");

  // The production helper receives the data component only. Keep an actual
  // product around it so every concurrency digest is compared, not a mock.
  using LS = core::lockset::LocksetDomain;
  auto locks = LS::entry();
  locks.add_mutex(101); locks.add_read_lock(102); locks.add_cond_lock(103, 104);
  locks.add_joined_thread(201); locks.add_spawned_thread("worker");
  locks.add_spawned_instance(202); locks.add_live_child(202);
  locks.add_heap_node_fresh(301); locks.add_signaled_cond(401); locks.add_awaited_cond(402);
  core::DomainProduct2< D, LS > product(good, locks);
  const auto concurrency_before = product.second();
  core::ConcurrentGlobalEnv thread_env;
  auto* worker = ar::Function::create(f.bundle, f.main->type(), "attribute_test_worker", true);
  const auto sid = thread_env.register_once_thread(worker, 501, true, core::ZNumber(0));
  thread_env.set_join_summary(worker, {201});
  const auto functions_before = thread_env.get_all_thread_functions();
  const auto instances_before = thread_env.get_func_instances(worker->name());
  const auto joins_before = thread_env.get_join_summary(worker);
  const bool unique_before = thread_env.is_thread_unique(worker->name());
  const bool static_before = thread_env.thread_instances_are_static(worker->name());
  require(sid != 0 && !instances_before.empty(), "The global lifecycle test needs a real registered instance");
  havoc_closed_pthread_attribute_bytes(effect, pointer, product.first(), memory, thread_env, bits);
  require(product.second().equals(concurrency_before),
          "Attribute havoc preserves mutex/read/conditional locks and joined/spawned/live/fresh histories");
  require(thread_env.get_all_thread_functions() == functions_before &&
          thread_env.get_func_instances(worker->name()) == instances_before &&
          thread_env.get_join_summary(worker) == joins_before &&
          thread_env.is_thread_unique(worker->name()) == unique_before &&
          thread_env.thread_instances_are_static(worker->name()) == static_before,
          "Attribute havoc grants no root, completion, multiplicity or static-instance fact");
}
int main() {
  for (unsigned bits : {32u, 64u}) { test_certificates(bits); test_current_ranges(bits); }
  std::cout << "PASS private pthread attribute AR tampering and native range controls\n";
}
