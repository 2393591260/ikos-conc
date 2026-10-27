// Copyright and license: see semantic/intrinsic.hpp.
#pragma once

#include <ikos/ar/semantic/function.hpp>

namespace ikos {
namespace ar {

// Operation identity only: callers still establish declaration, prototype,
// source binding, success, storage and synchronization premises independently.
enum class PthreadOperation {
  None, Create, Join, MutexLock, MutexUnlock, MutexTryLock, MutexTimedLock,
  SpinLock, SpinUnlock, SpinTryLock, RwlockReadLock, RwlockWriteLock,
  RwlockTryReadLock, RwlockTryWriteLock, RwlockTimedReadLock,
  RwlockTimedWriteLock, RwlockUnlock
};

inline PthreadOperation pthread_operation(const Function* function) {
  if (!function) return PthreadOperation::None;
  switch (function->intrinsic_id()) {
    case Intrinsic::PthreadCreate: return PthreadOperation::Create;
    case Intrinsic::PthreadJoin: return PthreadOperation::Join;
    case Intrinsic::PthreadMutexLock: return PthreadOperation::MutexLock;
    case Intrinsic::PthreadMutexUnlock: return PthreadOperation::MutexUnlock;
    default: break;
  }
  if (function->is_intrinsic()) return PthreadOperation::None;
  const auto& name = function->name();
  if (name == "pthread_create") return PthreadOperation::Create;
  if (name == "pthread_join") return PthreadOperation::Join;
  if (name == "pthread_mutex_lock") return PthreadOperation::MutexLock;
  if (name == "pthread_mutex_unlock") return PthreadOperation::MutexUnlock;
  if (name == "pthread_mutex_trylock") return PthreadOperation::MutexTryLock;
  if (name == "pthread_mutex_timedlock") return PthreadOperation::MutexTimedLock;
  if (name == "pthread_spin_lock") return PthreadOperation::SpinLock;
  if (name == "pthread_spin_unlock") return PthreadOperation::SpinUnlock;
  if (name == "pthread_spin_trylock") return PthreadOperation::SpinTryLock;
  if (name == "pthread_rwlock_rdlock") return PthreadOperation::RwlockReadLock;
  if (name == "pthread_rwlock_wrlock") return PthreadOperation::RwlockWriteLock;
  if (name == "pthread_rwlock_tryrdlock") return PthreadOperation::RwlockTryReadLock;
  if (name == "pthread_rwlock_trywrlock") return PthreadOperation::RwlockTryWriteLock;
  if (name == "pthread_rwlock_timedrdlock") return PthreadOperation::RwlockTimedReadLock;
  if (name == "pthread_rwlock_timedwrlock") return PthreadOperation::RwlockTimedWriteLock;
  if (name == "pthread_rwlock_unlock") return PthreadOperation::RwlockUnlock;
  return PthreadOperation::None;
}

inline bool is_pthread_create(const Function* function) {
  return pthread_operation(function) == PthreadOperation::Create;
}

inline bool is_pthread_join(const Function* function) {
  return pthread_operation(function) == PthreadOperation::Join;
}

} // namespace ar
} // namespace ikos
