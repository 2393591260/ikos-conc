// Copyright and license: see analysis/closed_integer_fold.hpp.
#include <ikos/analyzer/analysis/closed_integer_fold.hpp>
#include <ikos/analyzer/analysis/source_call_binding.hpp>
#include <cstdlib>
#include <iostream>

using namespace ikos::analyzer;
namespace ar = ikos::ar;
static void require(bool ok, const char* message) {
  if (!ok) { std::cerr << message << '\n'; std::exit(EXIT_FAILURE); }
}
int main() {
  llvm::LLVMContext llvm_context;
  llvm::DataLayout layout("e-p:32:32");
  auto* two = llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_context), 2);
  auto* one = llvm::ConstantInt::get(two->getType(), 1);
  auto* maximum = llvm::ConstantInt::get(two->getType(), 0xffffffffULL);
  auto* add = llvm::BinaryOperator::CreateAdd(two, one);
  auto* other_add = llvm::BinaryOperator::CreateAdd(two, one);
  auto* c = closed_integer_fold(add, {two, one}, layout);
  require(c && c->getZExtValue() == 3, "literal addition");
  c = closed_integer_fold(add, {maximum, one}, layout);
  require(c && c->isZero(), "ordinary unsigned wrap preserves bit patterns");
  add->setHasNoUnsignedWrap(true);
  require(!closed_integer_fold(add, {maximum, one}, layout), "nuw overflow rejects");
  add->setHasNoUnsignedWrap(false); add->setHasNoSignedWrap(true);
  auto* signed_max = llvm::ConstantInt::get(two->getType(), 0x7fffffff);
  require(!closed_integer_fold(add, {signed_max, one}, layout), "nsw overflow rejects");
  require(!closed_integer_fold(add, {one}, layout), "arity mismatch rejects");
  require(!closed_integer_fold(add, {nullptr, one}, layout), "null operand rejects");
  auto* wide = llvm::ConstantInt::get(llvm::Type::getInt64Ty(llvm_context), 0x100000001ULL);
  auto* trunc = new llvm::TruncInst(wide, two->getType());
  require(!closed_integer_fold(trunc, {wide}, layout), "lossy truncation rejects");
  auto* small = llvm::ConstantInt::get(wide->getType(), 2);
  c = closed_integer_fold(trunc, {small}, layout);
  require(c && c->getZExtValue() == 2, "value-preserving truncation");
  auto* mul = llvm::BinaryOperator::CreateMul(two, one);
  require(!closed_integer_fold(mul, {two, one}, layout), "unsupported operation rejects");
  require(!closed_integer_fold(add, {llvm::UndefValue::get(two->getType()), one}, layout),
          "undef is not a constant fact");
  require(!closed_integer_fold(add, {llvm::PoisonValue::get(two->getType()), one}, layout),
          "poison is not a constant fact");

  ar::Context context;
  auto* bundle = ar::Bundle::create(context,
      ar::DataLayout::create(ar::LittleEndian, {32, 4, 4}), "");
  auto* function = ar::Function::create(bundle,
      ar::FunctionType::get(context, ar::VoidType::get(context), {}, false), "test", true);
  auto* value = ar::InternalVariable::create(function->body(), ar::IntegerType::ui32(context));
  value->set_frontend(add);
  require(source_integer_binding(value, add), "actual dynamic source binds");
  require(!source_integer_binding(value, other_add),
          "another expression with the same range does not bind");
  auto* wrong_width = ar::InternalVariable::create(function->body(), ar::IntegerType::ui64(context));
  wrong_width->set_frontend(add);
  require(!source_integer_binding(wrong_width, add), "source width mismatch rejects");
  auto* block = ar::BasicBlock::create(function->body());
  auto* slot = ar::LocalVariable::create(function,
      ar::PointerType::get(context, ar::IntegerType::ui32(context)), 4);
  auto* count = ar::IntegerConstant::get(context, ar::IntegerType::ui32(context), 1);
  auto allocation = ar::Allocate::create(slot, ar::IntegerType::ui32(context), count);
  auto* actual_allocation = allocation.get();
  block->push_back(std::move(allocation));
  require(unique_allocation_definition(actual_allocation), "one actual local allocation");
  block->push_back(ar::Allocate::create(slot, ar::IntegerType::ui32(context), count));
  require(!unique_allocation_definition(actual_allocation),
          "synthetic reallocation with the same result invalidates its lifetime");
  auto* loaded = ar::InternalVariable::create(function->body(), ar::IntegerType::ui32(context));
  auto read = ar::Load::create(loaded, slot, 4, false);
  auto* actual_load = read.get();
  block->push_back(std::move(read));
  require(unique_def(loaded) == actual_load, "actual load has one result definition");
  block->push_back(ar::Assignment::create(loaded, count));
  require(unique_def(loaded) != actual_load,
          "synthetic overwrite invalidates the actual load result");
  delete add; delete other_add; delete trunc; delete mul;
}
