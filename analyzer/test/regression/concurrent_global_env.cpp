// Copyright and license: see core/domain/concurrent_global_env.hpp.
#include <ikos/core/domain/concurrent_global_env.hpp>

#include <cstdlib>
#include <iostream>
#include <unordered_set>

namespace ar = ikos::ar;
using ikos::core::ConcurrentGlobalEnv;
using Joined = std::unordered_set< std::uint64_t >;

static void require(bool condition, const char* reason) {
  if (!condition) {
    std::cerr << reason << '\n';
    std::exit(EXIT_FAILURE);
  }
}

int main() {
  ar::Context context;
  auto* bundle =
      ar::Bundle::create(context,
                         ar::DataLayout::create(ar::LittleEndian, {32, 4, 4}),
                         "");
  auto* type =
      ar::FunctionType::get(context, ar::VoidType::get(context), {}, false);
  auto* first = ar::Function::create(bundle, type, "first", true);
  auto* later = ar::Function::create(bundle, type, "later", true);
  ikos::analyzer::UnnamedShadowVariable
      pointer(ar::PointerType::get(context, ar::VoidType::get(context)), 0);
  ConcurrentGlobalEnv env;

  require(!env.is_dirty(), "A fresh blackboard is clean");
  env.mark_glob_flown(nullptr);
  require(!env.is_glob_flown(nullptr) && !env.is_dirty(),
          "Null global-flow provenance is ignored");
  require(!env.is_glob_flown(&pointer), "Provenance starts absent");
  env.mark_glob_flown(&pointer);
  require(env.is_glob_flown(&pointer) && env.is_dirty(),
          "New global-flow provenance invalidates the shared fixpoint");
  env.clear_dirty();
  env.mark_glob_flown(&pointer);
  require(!env.is_dirty(), "Repeated global-flow provenance is unchanged");

  require(env.get_join_summary(first).empty(),
          "An absent join summary is empty");
  env.set_join_summary(first, {});
  require(env.get_join_summary(first).empty() && !env.is_dirty(),
          "First-seen empty joins agree with an absent summary");
  env.set_join_summary(nullptr, {1});
  require(env.get_join_summary(nullptr).empty() && !env.is_dirty(),
          "Null join-summary roots are ignored");

  env.set_join_summary(first, {1});
  require(env.is_dirty() && env.get_join_summary(first) == Joined{1},
          "New nonempty MUST-join facts invalidate the fixpoint");
  env.clear_dirty();
  env.set_join_summary(first, {1});
  require(!env.is_dirty(), "An unchanged MUST-join summary is stable");
  env.set_join_summary(first, {1, 2});
  require(env.is_dirty() && env.get_join_summary(first) == Joined({1, 2}),
          "Growing MUST-join facts invalidate the fixpoint");
  env.clear_dirty();
  env.set_join_summary(first, {2});
  require(env.is_dirty() && env.get_join_summary(first) == Joined{2},
          "Replay replaces shrinking MUST-join facts rather than retaining "
          "them");
  env.clear_dirty();
  env.set_join_summary(first, {3});
  require(env.is_dirty() && env.get_join_summary(first) == Joined{3},
          "Same-size MUST-join replacement invalidates the fixpoint");

  // A later unchanged root cannot make an earlier replay's change stable.
  env.set_join_summary(later, {});
  env.set_join_summary(first, {3});
  env.mark_glob_flown(&pointer);
  env.mark_glob_flown(nullptr);
  require(env.is_dirty(), "Invalidation remains sticky across unchanged roots");

  env.clear_dirty();
  env.set_join_summary(first, {});
  require(env.is_dirty() && env.get_join_summary(first).empty(),
          "Losing every MUST-join fact invalidates the fixpoint");
  env.clear_dirty();
  env.set_join_summary(first, {});
  env.set_join_summary(later, {});
  require(!env.is_dirty(), "Repeated empty summaries are stable");
  std::cout << "Concurrent blackboard invalidation passed\n";
}
