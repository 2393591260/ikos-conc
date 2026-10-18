// Copyright and license: see context.hpp.
#pragma once

#include <map>

namespace ikos {
namespace analyzer {

// MUST predicates over all reachable observations, owned by one Context/run.
// The owning query inventory fixes each key's subject, width and scope.
// This class publishes range premises, not effect coverage or race freedom.
template < typename Key >
class AllAccessFacts {
private:
  std::map< Key, bool > _observations;
  bool _stable = false;

public:
  AllAccessFacts() = default;
  AllAccessFacts(const AllAccessFacts&) = delete;
  AllAccessFacts& operator=(const AllAccessFacts&) = delete;

  void observe(Key key, bool valid) {
    // A replay after publication invalidates the snapshot, even if it agrees.
    _stable = false;
    auto added = _observations.emplace(key, valid);
    if (!added.second) {
      added.first->second &= valid;
    }
  }

  // Only the driver may finish, after all replay and shared-summary flushing.
  void finish(bool stable) { _stable = stable; }

  bool proves(Key key) const {
    if (!_stable) {
      return false;
    }
    auto found = _observations.find(key);
    return found != _observations.end() && found->second;
  }
};

} // namespace analyzer
} // namespace ikos
