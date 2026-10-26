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

static void test_integer_layouts() {
  ar::Context context;
  for (unsigned bits : {32u, 64u}) {
    auto* bundle = ar::Bundle::create(context,
        ar::DataLayout::create(ar::LittleEndian, {bits, bits / 8, bits / 8}), "");
    SourceProgramFacts facts(bundle);
    auto global = [&](ar::Type* type, const char* name, bool definition = true,
                      bool local = false) {
      return ar::GlobalVariable::create(bundle, ar::PointerType::get(context, type),
                                        name, definition, 0, local);
    };
    auto* word = ar::IntegerType::ui32(context);
    auto* wide = ar::IntegerType::ui64(context);
    auto* pair = ar::ArrayType::get(context, word, ar::ZNumber(2));
    auto* padded = ar::StructType::create(context,
        {{ar::ZNumber(0), ar::IntegerType::ui8(context)},
         {ar::ZNumber(8), wide}, {ar::ZNumber(16), pair}}, false);
    auto* object = global(padded, "padded");
    const auto& cells = facts.integer_cells(object);
    require(cells.size() == 4 && cells[0].offset == 0 && cells[1].offset == 8 &&
            cells[2].offset == 16 && cells[3].offset == 20 &&
            cells[0].type->bit_width() == 8 && cells[1].type == wide &&
            cells[2].type == word && cells[3].type == word,
            "Nested integer leaves retain byte offsets, widths and padding");
    require(&cells == &facts.integer_cells(object), "Repeated transfers share one layout");

    auto* boundary = global(ar::ArrayType::get(context, word, ar::ZNumber(64)), "boundary");
    const auto& full = facts.integer_cells(boundary);
    require(full.size() == 64 && full.back().offset == 252,
            "The original 64-leaf boundary is inclusive");
    auto* oversized = global(ar::StructType::create(context,
        {{ar::ZNumber(0), word},
         {ar::ZNumber(4), ar::ArrayType::get(context, word, ar::ZNumber(65))}}, false), "oversized");
    const auto& refused = facts.integer_cells(oversized);
    require(refused.empty() && &refused == &facts.integer_cells(oversized),
            "A rejected layout caches no supported prefix");
    auto* mixed = global(ar::StructType::create(context,
        {{ar::ZNumber(0), word}, {ar::ZNumber(8), ar::PointerType::get(context, word)}}, false), "mixed");
    require(facts.integer_cells(mixed).empty() && facts.integer_cells(mixed).empty(),
            "An unsupported pointer leaf cannot leak its preceding integer cell");
    require(facts.integer_cells(global(word, "declaration", false)).empty() &&
            facts.integer_cells(global(word, "tls", true, true)).empty(),
            "Definitions and thread-local storage retain the old admission checks");
    SourceProgramFacts other(bundle);
    const auto& independent = other.integer_cells(object);
    require(&independent != &cells && independent.size() == cells.size() &&
            cells.data() == facts.integer_cells(object).data(),
            "Context snapshots own their cache and unrelated inserts preserve references");
  }
}

static void test_source_call_abi_attributes() {
  llvm::LLVMContext context;
  llvm::Module module("typed-source-call-abi", context);
  auto* wrapper = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), false),
      llvm::GlobalValue::ExternalLinkage, "wrapper", module);
  llvm::IRBuilder<> ir(llvm::BasicBlock::Create(context, "entry", wrapper));
  for (unsigned bits : {1u, 8u, 16u, 32u, 64u}) {
    auto* type = llvm::IntegerType::get(context, bits);
    auto* function = llvm::Function::Create(
        llvm::FunctionType::get(type, {type}, false),
        llvm::GlobalValue::ExternalLinkage, "scalar", module);
    auto* call = ir.CreateCall(function, {llvm::ConstantInt::get(type, 0)});
    auto reset = [&]() {
      call->setAttributes(llvm::AttributeList());
      function->setAttributes(llvm::AttributeList());
    };
    require(ordinary_source_call_abi_attributes(call, function),
            "ordinary scalar invocation needs no extension attribute");
    call->addParamAttr(0, llvm::Attribute::NoUndef);
    require(ordinary_source_call_abi_attributes(call, function),
            "ordinary attribute presence need not be identical");
    reset();
    for (auto extension : {llvm::Attribute::SExt, llvm::Attribute::ZExt}) {
      const bool supported = bits == 8 || bits == 16 ||
                             (bits == 1 && extension == llvm::Attribute::ZExt);
      for (bool returned : {false, true}) {
        reset();
        if (returned) call->addRetAttr(extension);
        else call->addParamAttr(0, extension);
        require(!ordinary_source_call_abi_attributes(call, function),
                "call-only extension is rejected");
        if (returned) function->addRetAttr(extension);
        else function->addParamAttr(0, extension);
        require(ordinary_source_call_abi_attributes(call, function) == supported,
                "paired narrow integer extension follows typed C policy");
        require(ordinary_source_function_abi_attributes(function) == supported,
                "runtime function entry has the same typed policy");
        call->setAttributes(llvm::AttributeList());
        require(!ordinary_source_call_abi_attributes(call, function),
                "declaration-only extension cannot hide behind LLVM fallback");
      }
    }
    reset();
    call->addParamAttr(0, llvm::Attribute::InReg);
    function->addParamAttr(0, llvm::Attribute::InReg);
    require(!ordinary_source_call_site_abi_attributes(call) &&
            !ordinary_source_call_abi_attributes(call, function) &&
            !ordinary_source_function_abi_attributes(function),
            "matching inreg is unsupported, including an indirect actual call");
    reset();
    call->addRetAttr(llvm::Attribute::InReg);
    function->addRetAttr(llvm::Attribute::InReg);
    require(!ordinary_source_call_abi_attributes(call, function),
            "matching return inreg is unsupported");
    reset();
    call->addParamAttr(0, llvm::Attribute::SExt);
    call->addParamAttr(0, llvm::Attribute::ZExt);
    function->addParamAttr(0, llvm::Attribute::SExt);
    function->addParamAttr(0, llvm::Attribute::ZExt);
    require(!ordinary_source_call_abi_attributes(call, function),
            "simultaneous extension kinds are rejected");
    reset();
    call->addRetAttr(llvm::Attribute::get(context, "unknown-value-abi"));
    require(!ordinary_source_call_abi_attributes(call, function),
            "unknown string value attribute is rejected");
    reset();
  }
  auto* pointer = llvm::Type::getInt8PtrTy(context);
  auto* function = llvm::Function::Create(
      llvm::FunctionType::get(pointer, {pointer}, false),
      llvm::GlobalValue::ExternalLinkage, "pointer", module);
  auto* call = ir.CreateCall(function, {llvm::ConstantPointerNull::get(pointer)});
  call->addParamAttr(0, llvm::Attribute::ZExt);
  function->addParamAttr(0, llvm::Attribute::ZExt);
  require(!ordinary_source_call_abi_attributes(call, function),
          "extensions never confer pointer ABI support");
  ir.CreateRetVoid();
}

int main() {
  test_source_call_abi_attributes();
  test_integer_layouts();
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
