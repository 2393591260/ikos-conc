// Copyright and license: see analysis/source_program_facts.hpp.
#include <ikos/analyzer/analysis/source_program_facts.hpp>
#include <ikos/analyzer/analysis/source_call_binding.hpp>
#include <llvm/IR/IRBuilder.h>
#include <cstdlib>
#include <iostream>

using namespace ikos::analyzer;
namespace ar = ikos::ar;
static void require(bool ok, const char* message) {
  if (!ok) { std::cerr << message << '\n'; std::exit(EXIT_FAILURE); }
}
int main() {
  llvm::LLVMContext native_context;
  llvm::Module module("facts", native_context);
  auto* native_type = llvm::FunctionType::get(llvm::Type::getVoidTy(native_context), false);
  auto* native = llvm::Function::Create(native_type, llvm::GlobalValue::ExternalLinkage, "root", module);
  llvm::IRBuilder<> ir(llvm::BasicBlock::Create(native_context, "entry", native));
  auto* slot = ir.CreateAlloca(ir.getInt32Ty());
  auto* read = ir.CreateLoad(ir.getInt32Ty(), slot);
  auto* later = ir.CreateLoad(ir.getInt32Ty(), slot);
  ir.CreateRetVoid();

  ar::Context context;
  auto* bundle = ar::Bundle::create(context, ar::DataLayout::create(ar::LittleEndian, {32, 4, 4}), "");
  auto* f = ar::Function::create(bundle,
      ar::FunctionType::get(context, ar::VoidType::get(context), {}, false), "root", true);
  f->set_frontend(native);
  auto* block = ar::BasicBlock::create(f->body());
  auto* local = ar::LocalVariable::create(f, ar::PointerType::get(context, ar::IntegerType::ui32(context)), 4);
  auto* one = ar::IntegerConstant::get(context, ar::IntegerType::ui32(context), 1);
  auto* value = ar::InternalVariable::create(f->body(), ar::IntegerType::ui32(context));
  auto allocation = ar::Allocate::create(local, ar::IntegerType::ui32(context), one);
  allocation->set_frontend(read); // CLS must ignore this CLSA duplicate.
  block->push_back(std::move(allocation));
  auto load = ar::Load::create(value, local, 4, false);
  auto* actual = load.get(); load->set_frontend(read); block->push_back(std::move(load));
  block->push_back(ar::Load::create(value, local, 4, false)); // No source is invented.
  SourceProgramFacts facts(bundle);
  require(facts.memory_calls().statements.empty(), "early refusal does no indexing");
  require(facts.index(), "unbounded index builds");
  require(facts.statement_count() == 3, "all statement categories spend once budget");
  require(facts.memory_calls().unique && !facts.memory_calls_allocations().unique,
          "duplicate policies stay separate");
  require(facts.memory_calls().statements.size() == 1 &&
          facts.memory_calls().statements.at(read) == actual, "missing frontend stays missing");
  require(!facts.index(3) && facts.index(4), "cached scan preserves exact decrement boundary");
  require(facts.functions().at(native) == f, "actual function identity binds");
  const auto& dt = facts.dominators(native);
  require(&dt == &facts.dominators(native), "dominance is computed once");
  require(dt.dominates(read, later) && !dt.dominates(later, read),
          "same-block dominance retains instruction order");

  auto first = ar::Call::create(nullptr, f, {});
  auto second = ar::Call::create(nullptr, f, {});
  require(!facts.once_callbacks(first.get()), "unqueried and cached-empty differ");
  facts.remember_once_callbacks(first.get(), {});
  auto fallback = *facts.once_callbacks(first.get());
  fallback.push_back(f);
  require(facts.once_callbacks(first.get())->empty(), "local fallback cannot grant cached seal");
  facts.remember_once_callbacks(second.get(), {f});
  require(facts.once_callbacks(first.get())->empty() &&
          facts.once_callbacks(second.get())->size() == 1,
          "different actual queries do not share selected callbacks");

  // Rejected scans remain bounded, but a different consumer may finish the index.
  SourceProgramFacts bounded(bundle);
  require(!bounded.index(2) && bounded.statement_count() == 2, "bounded scan stops at refusal");
  require(!bounded.index(2) && bounded.statement_count() == 2, "repeat refusal does not rescan");
  require(bounded.index() && bounded.statement_count() == 3, "later full scan may finish");
  auto duplicate = ar::Load::create(value, local, 4, false);
  duplicate->set_frontend(read); block->push_back(std::move(duplicate));
  SourceProgramFacts duplicates(bundle); // New immutable snapshot after the test mutation.
  require(duplicates.index() && !duplicates.memory_calls().unique,
          "two loads mapped to one source invalidate CLS too");
  require(facts.memory_calls().unique, "separate snapshots never overwrite each other");

  for (unsigned n = 4; n < 16383; ++n) block->push_back(ar::Assignment::create(value, one));
  SourceProgramFacts below_boundary(bundle);
  require(below_boundary.index(16384) && below_boundary.statement_count() == 16383,
          "once may continue after scanning 16383 statements");
  block->push_back(ar::Assignment::create(value, one));
  SourceProgramFacts boundary(bundle);
  require(!boundary.index(16384) && boundary.statement_count() == 16384,
          "once rejects its 16384th statement including non-indexed assignments");
  auto* wide_bundle = ar::Bundle::create(context, ar::DataLayout::create(ar::LittleEndian, {64, 8, 8}), "");
  SourceProgramFacts wide(wide_bundle);
  require(wide.index() && wide.statement_count() == 0 && !wide.once_callbacks(second.get()),
          "separate ABI/input snapshots carry no identities or once results");

  require(ordinary_source_attributes(llvm::AttributeSet()), "empty ordinary attribute set");
  require(ordinary_source_attributes(llvm::AttributeSet::get(native_context,
          {llvm::Attribute::get(native_context, llvm::Attribute::NoUndef)})), "existing whitelist");
  require(!ordinary_source_attributes(llvm::AttributeSet::get(native_context,
          {llvm::Attribute::get(native_context, llvm::Attribute::SExt)})), "unsupported enum rejects");
  require(!ordinary_source_attributes(llvm::AttributeSet::get(native_context,
          {llvm::Attribute::get(native_context, "custom")})), "string attribute rejects");
}
