// Copyright and license: see data_race.hpp.
#pragma once

#include <memory>
#include <vector>
#include <ikos/analyzer/json/json.hpp>

namespace ikos {
namespace analyzer {
class Context;
class BinaryTokenOwnership;
struct AccessRecord;

// Symbolic, inductively checked live-heap slices; consumed by ArenaReservation.
class SymbolicArenaReservation {
  class Impl;
  std::unique_ptr< Impl > _impl;
public:
  explicit SymbolicArenaReservation(Context&);
  ~SymbolicArenaReservation();
  bool excludes(const AccessRecord&, const AccessRecord&) const;
  JsonDict info(const AccessRecord&) const;
};

// Disjoint slices from a sealed, serialized finite counter transition system.
// A proof budget is a refusal boundary, never an unrolling-based SAFE result.
class ArenaReservation {
  class Impl;
  std::unique_ptr< Impl > _impl;

public:
  ArenaReservation(Context&, const std::vector< AccessRecord >&, const BinaryTokenOwnership&);
  ~ArenaReservation();
  bool excludes(const AccessRecord&, const AccessRecord&) const;
  JsonDict info(const AccessRecord&) const;
};
} // namespace analyzer
} // namespace ikos
