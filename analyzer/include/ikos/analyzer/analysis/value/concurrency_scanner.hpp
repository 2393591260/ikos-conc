/*******************************************************************************
 *
 * \file
 * \brief Zero-config concurrency detection scanner (AR layer)
 *
 * Single-pass walk of every `ar::Function*` body looking for a DIRECT extern
 * call to a `pthread_*` API. Mirrors the cost envelope of
 * `requires_relational_interference` (O(n), sub-millisecond for SV-COMP
 * pthread cases) so it is safe to run once, before the engine is chosen, in
 * the `--concurrency=auto` (default) mode.
 *
 * IMPORTANT soundness caveat: only DIRECT calls are matched. Indirect
 * (function-pointer) calls to a thread routine, C++ `std::thread`, OpenMP,
 * __VERIFIER_atomic_* pseudo-primitives, and user-level thread libraries are
 * KNOWN blind spots. A `false` here therefore means "no DIRECT pthread call
 * found", NEVER "proven single-threaded". Callers must treat `false` as
 * "not proven concurrent", and thus may only WARN (never silently claim the
 * sequential result is concurrency-correct).
 *
 ******************************************************************************/

#pragma once

#include <string>

#include <ikos/ar/semantic/bundle.hpp>
#include <ikos/ar/semantic/code.hpp>
#include <ikos/ar/semantic/function.hpp>
#include <ikos/ar/semantic/statement.hpp>
#include <ikos/ar/semantic/value.hpp>
#include <ikos/ar/support/cast.hpp>

namespace ikos {
namespace analyzer {

/// \brief True when `name` is a POSIX threading primitive (the `pthread_*`
/// family). A bare prefix keeps the whitelist complete without enumerating
/// every symbol (create / join / mutex_* / rwlock_* / cond_* / attr_* …);
/// `pthread_self` is harmless to include.
inline bool is_concurrency_api(const std::string& name) {
  return name.compare(0, 8, "pthread_") == 0;
}

/// \brief Does any function body make a DIRECT extern call to a `pthread_*`
/// API? — see the file header for the soundness caveat.
inline bool requires_concurrency(ar::Bundle* bundle) {
  if (bundle == nullptr) {
    return false;
  }
  for (auto it = bundle->function_begin(), et = bundle->function_end();
       it != et;
       ++it) {
    ar::Function* fun = *it;
    if (!fun->is_definition()) {
      continue;
    }
    ar::Code* body = fun->body_or_null();
    if (body == nullptr) {
      continue;
    }
    for (ar::BasicBlock* bb : *body) {
      for (ar::Statement* stmt : *bb) {
        auto* call = ar::dyn_cast< ar::CallBase >(stmt);
        if (call == nullptr) {
          continue;
        }
        auto* fpc =
            ar::dyn_cast_or_null< ar::FunctionPointerConstant >(call->called());
        if (fpc == nullptr) {
          continue; // indirect call or non-function target: a blind spot
        }
        ar::Function* callee = fpc->function();
        if (callee != nullptr && callee->is_declaration() &&
            is_concurrency_api(callee->name())) {
          return true;
        }
      }
    }
  }
  return false;
}

} // namespace analyzer
} // namespace ikos