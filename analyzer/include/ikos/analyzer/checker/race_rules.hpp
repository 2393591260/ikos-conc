// Copyright and license: see data_race.hpp.
#pragma once

#include <cstddef>

namespace ikos {
namespace analyzer {
namespace race_rules {

// Permission registry for existing mechanisms, not a proof checker.
// Distinct permissions: a pair proof never discharges an effect obligation.
// Per-run opt-in gates remain at each original predicate invocation.
enum class Role { None, PairExclusion, EffectCoverage, LibraryEffect, RangePremise, ExecutionPremise };
enum class Stage { None, Transfer, Preflight, FinalPairs, FinalCoverage, BoundedSearch };
enum class Id {
  None,
  SameUniqueStatement,
  ReadRead,
  SameUniqueThread,
  InactiveChild,
  JoinedRight,
  JoinedLeft,
  CreateBefore,
  UnreachablePoints,
  DisjointPoints,
  AtomicAtomic,
  InitPublication,
  DisabledPrivateInit,
  DisjointBytes,
  CommonMutex,
  CorrespondingMutexExact,
  CorrespondingMutexCongruence,
  OffsetCongruence,
  OffsetSmt,
  PrivateHeap,
  BinaryToken,
  ArenaReservation,
  OnceInitialization,
  MutexHandover,
  ThreadLifecycle,
  GuardedPublication,
  ScProtocol,
  SelfPublication,
  FirstPublication,
  RegionOwnership,
  BucketOwnership,
  ReadonlyHeap,
  ArraySlices,
  CoverageReadonlyHeap,
  CoverageBucketOwnership,
  EffectSemaphore,
  EffectConstantOutput,
  EffectScalarScan,
  EffectTlsSlot,
  EffectTlsDestructor,
  EffectPthreadInitialization,
  EffectSyncArguments,
  EffectRuntimeStdio,
  EffectImmutableArguments,
  EffectLifetimeBoundary,
  EffectInitializationArguments,
  EffectTlsCreateOutput,
  RangeMutexEffects,
  RangeSubarray,
  RangeNativeHandles,
  BmcLocalInteger,
  BmcLocalControl,
  BmcSelectedExecution,
  Count
};

// Identity metadata does not choose execution order or establish premises.
struct Contract {
  const char* id;
  Role role;
  Stage stage;
  bool enabled;
};

inline Contract contract(Id id) {
  switch (id) {
    case Id::None: return {"none-v1", Role::None, Stage::None, false};
    case Id::SameUniqueStatement: return {"same-unique-statement-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::ReadRead: return {"read-read-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::SameUniqueThread: return {"same-unique-thread-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::InactiveChild: return {"inactive-child-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::JoinedRight: return {"joined-right-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::JoinedLeft: return {"joined-left-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::CreateBefore: return {"create-before-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::UnreachablePoints: return {"unreachable-points-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::DisjointPoints: return {"disjoint-points-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::AtomicAtomic: return {"atomic-atomic-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::InitPublication: return {"init-publication-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::DisabledPrivateInit: return {"disabled-private-init-v1", Role::PairExclusion, Stage::FinalPairs, false};
    case Id::DisjointBytes: return {"disjoint-bytes-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::CommonMutex: return {"common-mutex-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::CorrespondingMutexExact: return {"corresponding-mutex-exact-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::CorrespondingMutexCongruence: return {"corresponding-mutex-congruence-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::OffsetCongruence: return {"offset-congruence-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::OffsetSmt: return {"offset-smt-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::PrivateHeap: return {"private-heap-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::BinaryToken: return {"binary-token-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::ArenaReservation: return {"arena-reservation-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::OnceInitialization: return {"once-initialization-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::MutexHandover: return {"mutex-handover-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::ThreadLifecycle: return {"thread-lifecycle-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::GuardedPublication: return {"guarded-publication-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::ScProtocol: return {"sc-protocol-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::SelfPublication: return {"self-publication-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::FirstPublication: return {"first-publication-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::RegionOwnership: return {"region-ownership-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::BucketOwnership: return {"bucket-ownership-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::ReadonlyHeap: return {"readonly-heap-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::ArraySlices: return {"array-slices-v1", Role::PairExclusion, Stage::FinalPairs, true};
    case Id::CoverageReadonlyHeap: return {"coverage-readonly-heap-v1", Role::EffectCoverage, Stage::FinalCoverage, true};
    case Id::CoverageBucketOwnership: return {"coverage-bucket-ownership-v1", Role::EffectCoverage, Stage::FinalCoverage, true};
    case Id::EffectSemaphore: return {"effect-semaphore-v1", Role::LibraryEffect, Stage::Transfer, true};
    case Id::EffectConstantOutput: return {"effect-constant-output-v1", Role::LibraryEffect, Stage::Transfer, true};
    case Id::EffectScalarScan: return {"effect-scalar-scan-v1", Role::LibraryEffect, Stage::Transfer, true};
    case Id::EffectTlsSlot: return {"effect-tls-slot-v1", Role::LibraryEffect, Stage::Transfer, true};
    case Id::EffectTlsDestructor: return {"effect-tls-destructor-v1", Role::LibraryEffect, Stage::Transfer, true};
    case Id::EffectPthreadInitialization: return {"effect-pthread-initialization-v1", Role::LibraryEffect, Stage::Transfer, true};
    case Id::EffectSyncArguments: return {"effect-sync-arguments-v1", Role::LibraryEffect, Stage::Transfer, true};
    case Id::EffectRuntimeStdio: return {"effect-runtime-stdio-v1", Role::LibraryEffect, Stage::Transfer, true};
    case Id::EffectImmutableArguments: return {"effect-immutable-arguments-v1", Role::LibraryEffect, Stage::Transfer, true};
    case Id::EffectLifetimeBoundary: return {"effect-lifetime-boundary-v1", Role::LibraryEffect, Stage::Transfer, true};
    case Id::EffectInitializationArguments: return {"effect-initialization-arguments-v1", Role::LibraryEffect, Stage::Transfer, true};
    case Id::EffectTlsCreateOutput: return {"effect-tls-create-output-v1", Role::LibraryEffect, Stage::Transfer, true};
    case Id::RangeMutexEffects: return {"range-mutex-effects-v1", Role::RangePremise, Stage::Preflight, true};
    case Id::RangeSubarray: return {"range-subarray-v1", Role::RangePremise, Stage::Preflight, true};
    case Id::RangeNativeHandles: return {"range-native-handles-v1", Role::RangePremise, Stage::Preflight, true};
    case Id::BmcLocalInteger: return {"bmc-local-integer-v1", Role::ExecutionPremise, Stage::BoundedSearch, true};
    case Id::BmcLocalControl: return {"bmc-local-control-v1", Role::ExecutionPremise, Stage::BoundedSearch, true};
    case Id::BmcSelectedExecution: return {"bmc-selected-execution-v1", Role::ExecutionPremise, Stage::BoundedSearch, true};
    case Id::Count: break;
  }
  return {"invalid", Role::None, Stage::None, false};
}

inline Id dependency(Id id) {
  switch (id) {
    case Id::ArenaReservation:
    case Id::OnceInitialization: return Id::BinaryToken;
    default: return Id::None;
  }
}

inline bool permits_pair_exclusion(Id id) {
  const auto c = contract(id);
  return c.enabled && c.role == Role::PairExclusion;
}

inline bool permits_effect_coverage(Id id) {
  const auto c = contract(id);
  return c.enabled && c.role == Role::EffectCoverage;
}

inline bool permits_execution_premise(Id id) {
  const auto c = contract(id);
  return c.enabled && c.role == Role::ExecutionPremise;
}

inline bool permits_range_premise(Id id) {
  const auto c = contract(id);
  return c.enabled && c.role == Role::RangePremise;
}

inline bool permits_library_effect(Id id) {
  const auto c = contract(id);
  return c.enabled && c.role == Role::LibraryEffect;
}

} // namespace race_rules
} // namespace analyzer
} // namespace ikos
