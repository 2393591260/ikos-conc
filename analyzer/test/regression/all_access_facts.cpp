// Copyright and license: see analysis/all_access_facts.hpp.
#include <ikos/analyzer/analysis/all_access_facts.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <type_traits>
#include <vector>

using Facts = ikos::analyzer::AllAccessFacts< int >;

static void require(bool condition, const char* reason) {
  if (!condition) {
    std::cerr << reason << '\n';
    std::exit(EXIT_FAILURE);
  }
}

int main() {
  static_assert(!std::is_copy_constructible< Facts >::value,
                "Observations cannot be copied into a fresh analysis");
  Facts facts;
  facts.finish(true);
  require(!facts.proves(1), "An absent observation is not a proof");
  facts.observe(1, true);
  require(!facts.proves(1), "A collecting analysis cannot publish a fact");
  facts.finish(false);
  require(!facts.proves(1), "Unstable shared summaries cannot publish a fact");
  facts.finish(true);
  require(facts.proves(1), "Stable complete positive observations prove the key");
  require(!facts.proves(2), "A different program point needs its own evidence");

  facts.observe(2, true);
  require(!facts.proves(1), "Any later replay invalidates the published snapshot");
  facts.finish(true);
  require(facts.proves(1) && facts.proves(2), "Each observed key remains available");
  facts.observe(1, false);
  facts.finish(true);
  require(!facts.proves(1) && facts.proves(2), "A failed context revokes only its key");
  facts.observe(1, true);
  facts.finish(true);
  require(!facts.proves(1), "A later positive context cannot repair a failed one");

  // Context/replay order must not hide one failing reachable observation.
  std::vector< int > observations{0, 1, 1};
  do {
    Facts reordered;
    for (int valid : observations) {
      reordered.observe(1, valid != 0);
    }
    reordered.finish(true);
    require(!reordered.proves(1), "All contexts combine by AND in any order");
  } while (std::next_permutation(observations.begin(), observations.end()));

  Facts separate_run;
  separate_run.finish(true);
  require(!separate_run.proves(2), "A fresh run cannot inherit another run's facts");
  std::cout << "All-access fact lifecycle passed\n";
}
