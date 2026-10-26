// Copyright and license: see checker/race_rules.hpp.
#include <ikos/analyzer/checker/race_rules.hpp>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

using namespace ikos::analyzer::race_rules;
static void require(bool valid, const char* message) {
  if (!valid) { std::cerr << message << '\n'; std::exit(EXIT_FAILURE); }
}
int main() {
  std::set< std::string > names;
  for (std::size_t n = 1; n < static_cast< std::size_t >(Id::Count); ++n) {
    auto id = static_cast< Id >(n);
    auto c = contract(id);
    require(names.insert(c.id).second, "Every rule needs a distinct identity");
    if (c.role != Role::PairExclusion)
      require(!permits_pair_exclusion(id), "Other permissions cannot exclude a pair");
    if (c.role != Role::EffectCoverage)
      require(!permits_effect_coverage(id), "Other permissions cannot cover an effect");
    if (c.role != Role::LibraryEffect)
      require(!permits_library_effect(id), "Other permissions cannot omit a library effect");
  }
  require(!permits_pair_exclusion(Id::DisabledPrivateInit), "Unsafe legacy rule stays disabled");
  require(!permits_pair_exclusion(Id::BmcSelectedExecution), "A BMC result cannot prove unbounded safety");
  require(!permits_effect_coverage(Id::RangeMutexEffects), "A range alone does not prove coverage");
  require(permits_pair_exclusion(Id::BucketOwnership), "Bucket pair proof is registered");
  require(permits_effect_coverage(Id::CoverageBucketOwnership), "Bucket coverage needs its distinct proof");
  require(!permits_effect_coverage(Id::BucketOwnership), "Bucket pair proof cannot cover an effect");
  require(!permits_pair_exclusion(Id::CoverageBucketOwnership), "Bucket coverage cannot exclude a pair");
  require(permits_range_premise(Id::RangeSubarray), "Range publication is registered");
  require(!permits_range_premise(Id::CoverageBucketOwnership), "Coverage cannot authorize range publication");
  require(!permits_range_premise(Id::BmcSelectedExecution), "Execution cannot authorize range publication");
  require(permits_pair_exclusion(Id::ThreadArgumentWindows), "Native windows may exclude certified pairs");
  require(!permits_effect_coverage(Id::ThreadArgumentWindows), "Native windows cannot cover missing effects");
  require(!permits_execution_premise(Id::ThreadArgumentWindows), "Native windows cannot authorize BMC executions");
  require(!permits_pair_exclusion(Id::None), "No proof retains the candidate");
  require(!permits_pair_exclusion(Id::Count), "Invalid identity grants no permission");
  require(dependency(Id::ArenaReservation) == Id::BinaryToken,
          "Arena retains its actual token producer dependency");
  require(dependency(Id::OnceInitialization) == Id::BinaryToken,
          "Once initialization retains its actual token dependency");
  require(permits_execution_premise(Id::BmcSelectedExecution), "BMC confirmation is registered as execution evidence");
  require(!permits_execution_premise(Id::CommonMutex), "Pair exclusion cannot authorize execution semantics");
  std::cout << "Race rule permissions passed\n";
}
