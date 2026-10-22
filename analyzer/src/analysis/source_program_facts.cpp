// Copyright and license: see context.hpp.
#include <ikos/analyzer/analysis/source_program_facts.hpp>
#include <ikos/analyzer/analysis/context.hpp>

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
  _globals.clear();
  _statement_count = 0;
  auto* bundle = _bundle;
  for (auto it = bundle->function_begin(); it != bundle->function_end(); ++it) {
    auto* f = *it;
    if (f->has_frontend()) _functions.emplace(f->frontend< llvm::Function >(), f);
    if (!f->is_definition()) continue;
    for (auto* block : *f->body()) for (auto* s : *block) {
      if (++_statement_count >= limit) return false;
      bool basic = isa< ar::CallBase >(s) || isa< ar::Load >(s) || isa< ar::Store >(s);
      if ((!basic && !isa< ar::Allocate >(s)) || !s->has_frontend()) continue;
      auto* native = s->frontend< llvm::Value >();
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

SourceProgramFacts& source_program_facts(Context& ctx) {
  if (!ctx._source_program_facts)
    ctx._source_program_facts = std::make_shared< SourceProgramFacts >(ctx.bundle);
  return *ctx._source_program_facts;
}
} // namespace analyzer
} // namespace ikos
