// Copyright and license: see context.hpp.
#include <ikos/analyzer/analysis/source_program_facts.hpp>
#include <ikos/analyzer/analysis/context.hpp>
#include <ikos/analyzer/support/cast.hpp>

#include <algorithm>
#include <functional>

namespace ikos {
namespace analyzer {
SourceProgramFacts::SourceProgramFacts(ar::Bundle* bundle) : _bundle(bundle) {}

bool SourceProgramFacts::index(std::size_t limit) {
  if (_indexed) return _statement_count < limit;
  if (_statement_count >= limit) return false;
  // A later unbounded consumer may finish an earlier once-limited scan.
  _memory_calls = {};
  _memory_calls_allocations = {};
  _functions.clear();
  _definitions.clear();
  _globals.clear();
  _statement_count = 0;
  auto* bundle = _bundle;
  for (auto it = bundle->function_begin(); it != bundle->function_end(); ++it) {
    auto* f = *it;
    if (f->has_frontend()) _functions.emplace(f->frontend< llvm::Function >(), f);
    if (!f->is_definition()) continue;
    auto* source_function = f->has_frontend() ? f->frontend< llvm::Function >() : nullptr;
    if (source_function) _definitions.insert(source_function);
    else {
      _memory_calls.complete = false;
      _memory_calls_allocations.complete = false;
    }
    for (auto* block : *f->body()) for (auto* s : *block) {
      if (++_statement_count >= limit) return false;
      bool basic = isa< ar::CallBase >(s) || isa< ar::Load >(s) || isa< ar::Store >(s);
      if (!basic && !isa< ar::Allocate >(s)) continue;
      auto* native = s->has_frontend() ? s->frontend< llvm::Value >() : nullptr;
      auto* instruction = llvm::dyn_cast_or_null< llvm::Instruction >(native);
      if (!instruction || instruction->getFunction() != source_function) {
        if (basic) _memory_calls.complete = false;
        _memory_calls_allocations.complete = false;
      }
      if (!native) continue;
      if (basic && !_memory_calls.statements.emplace(native, s).second)
        _memory_calls.unique = false;
      if (!_memory_calls_allocations.statements.emplace(native, s).second)
        _memory_calls_allocations.unique = false;
    }
  }
  for (auto it = bundle->global_begin(); it != bundle->global_end(); ++it)
    if ((*it)->has_frontend()) _globals.emplace((*it)->frontend< llvm::GlobalVariable >(), *it);
  _indexed = true;
  return true;
}

const llvm::DominatorTree& SourceProgramFacts::dominators(llvm::Function* f) {
  auto& tree = _dominators[f];
  if (!tree) tree = std::make_unique< llvm::DominatorTree >(*f);
  return *tree;
}

const std::vector< ar::Function* >* SourceProgramFacts::once_callbacks(ar::CallBase* c) const {
  auto it = _once_callbacks.find(c);
  return it == _once_callbacks.end() ? nullptr : &it->second;
}

void SourceProgramFacts::remember_once_callbacks(ar::CallBase* c, std::vector< ar::Function* > targets) {
  _once_callbacks.emplace(c, std::move(targets));
}

// Preserve the original finite, nonoverlapping integer-leaf subset.
const std::vector< SourceProgramFacts::IntegerCell >&
SourceProgramFacts::integer_cells(ar::GlobalVariable* global) {
  auto found = _integer_cells.find(global);
  if (found != _integer_cells.end()) return found->second;
  auto& cells = _integer_cells[global];
  if (!global->is_definition() || global->is_thread_local()) return cells;
  const auto& dl = global->bundle()->data_layout();
  auto extent = dl.alloc_size_in_bytes(global->type()->pointee());
  if (extent <= 0 || !extent.fits< std::uint64_t >() ||
      extent >= (core::ZNumber(1) << dl.pointers.bit_width)) return cells;
  std::function< bool(ar::Type*, core::ZNumber, unsigned) > visit =
      [&](ar::Type* type, core::ZNumber offset, unsigned depth) {
    if (depth > 8 || cells.size() >= 64) return false;
    if (auto* integer = dyn_cast< ar::IntegerType >(type)) {
      auto bytes = dl.store_size_in_bytes(type);
      if (offset < 0 || offset + bytes > extent ||
          !offset.fits< std::uint64_t >()) return false;
      cells.push_back({offset.to< std::uint64_t >(), integer});
      return true;
    }
    if (auto* structure = dyn_cast< ar::StructType >(type)) {
      for (const auto& field : structure->layout())
        if (!visit(field.type, offset + field.offset, depth + 1)) return false;
      return true;
    }
    if (auto* array = dyn_cast< ar::ArrayType >(type)) {
      if (array->num_elements() > 64) return false;
      auto stride = dl.alloc_size_in_bytes(array->element_type());
      for (core::ZNumber i(0); i < array->num_elements(); ++i)
        if (!visit(array->element_type(), offset + i * stride, depth + 1)) return false;
      return true;
    }
    return false;
  };
  if (!visit(global->type()->pointee(), core::ZNumber(0), 0)) {
    cells.clear();
    return cells;
  }
  std::sort(cells.begin(), cells.end(), [](const auto& a, const auto& b) {
    return a.offset < b.offset;
  });
  core::ZNumber end(0);
  for (const auto& cell : cells) {
    if (core::ZNumber(cell.offset) < end) {
      cells.clear();
      return cells;
    }
    end = core::ZNumber(cell.offset) + dl.store_size_in_bytes(cell.type);
  }
  return cells;
}

SourceProgramFacts& source_program_facts(Context& ctx) {
  if (!ctx._source_program_facts)
    ctx._source_program_facts = std::make_shared< SourceProgramFacts >(ctx.bundle);
  return *ctx._source_program_facts;
}
} // namespace analyzer
} // namespace ikos
