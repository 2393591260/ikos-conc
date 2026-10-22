// Copyright and license: see context.hpp.
#pragma once

#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <vector>
#include <llvm/IR/Dominators.h>
#include <ikos/ar/semantic/bundle.hpp>
#include <ikos/ar/semantic/statement.hpp>

namespace ikos {
namespace analyzer {
class Context;

// Immutable input identities, not a certificate that a consumer's premises hold.
class SourceProgramFacts {
public:
  struct StatementIndex {
    std::map< llvm::Value*, ar::Statement* > statements;
    bool unique = true;
  };

private:
  ar::Bundle* _bundle;
  bool _indexed = false;
  StatementIndex _memory_calls, _memory_calls_allocations;
  std::map< llvm::Function*, ar::Function* > _functions;
  std::map< llvm::GlobalVariable*, ar::GlobalVariable* > _globals;
  std::size_t _statement_count = 0;
  std::map< llvm::Function*, std::unique_ptr< llvm::DominatorTree > > _dominators;
  std::map< ar::CallBase*, std::vector< ar::Function* > > _once_callbacks;

public:
  explicit SourceProgramFacts(ar::Bundle*);
  // A bounded query refuses at the same statement as its original scan.
  bool index(std::size_t limit = std::numeric_limits< std::size_t >::max());
  const StatementIndex& memory_calls() const { return _memory_calls; }
  const StatementIndex& memory_calls_allocations() const { return _memory_calls_allocations; }
  const std::map< llvm::Function*, ar::Function* >& functions() const { return _functions; }
  const std::map< llvm::GlobalVariable*, ar::GlobalVariable* >& globals() const { return _globals; }
  // Preserve each consumer's original logical work bound despite cached scans.
  std::size_t statement_count() const { return _statement_count; }
  const llvm::DominatorTree& dominators(llvm::Function*);
  const std::vector< ar::Function* >* once_callbacks(ar::CallBase*) const;
  void remember_once_callbacks(ar::CallBase*, std::vector< ar::Function* >);
};

// One cache per Context, used only after frontend passes and option reconciliation.
SourceProgramFacts& source_program_facts(Context&);
} // namespace analyzer
} // namespace ikos
