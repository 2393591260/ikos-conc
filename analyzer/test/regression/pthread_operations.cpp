// Copyright and license: see ar/semantic/intrinsic.hpp.
#include <ikos/ar/semantic/pthread.hpp>
#include <ikos/ar/semantic/bundle.hpp>
#include <cstdlib>
#include <iostream>
#include <utility>

namespace ar = ikos::ar;
using Op = ar::PthreadOperation;
static unsigned checks = 0;
static void require(bool ok) {
  ++checks;
  if (!ok) { std::cerr << "pthread operation contract failed at " << checks << '\n'; std::exit(1); }
}

int main() {
  for (unsigned bits : {32u, 64u}) {
    ar::Context context;
    auto* bundle = ar::Bundle::create(context,
        ar::DataLayout::create(ar::LittleEndian, {bits, bits / 8, bits / 8}), "");
    auto* status = ar::IntegerType::si32(context);
    auto* word = ar::IntegerType::get(context, bits, ar::Unsigned);
    auto* pointer = ar::PointerType::get(context, ar::IntegerType::si8(context));
    auto* type = ar::FunctionType::get(context, status, {word, pointer}, false);
    const std::pair<const char*, Op> names[] = {
      {"pthread_create", Op::Create}, {"pthread_join", Op::Join},
      {"pthread_mutex_lock", Op::MutexLock}, {"pthread_mutex_unlock", Op::MutexUnlock},
      {"pthread_mutex_trylock", Op::MutexTryLock}, {"pthread_mutex_timedlock", Op::MutexTimedLock},
      {"pthread_spin_lock", Op::SpinLock}, {"pthread_spin_unlock", Op::SpinUnlock},
      {"pthread_spin_trylock", Op::SpinTryLock}, {"pthread_rwlock_rdlock", Op::RwlockReadLock},
      {"pthread_rwlock_wrlock", Op::RwlockWriteLock}, {"pthread_rwlock_tryrdlock", Op::RwlockTryReadLock},
      {"pthread_rwlock_trywrlock", Op::RwlockTryWriteLock}, {"pthread_rwlock_timedrdlock", Op::RwlockTimedReadLock},
      {"pthread_rwlock_timedwrlock", Op::RwlockTimedWriteLock}, {"pthread_rwlock_unlock", Op::RwlockUnlock}
    };
    require(ar::pthread_operation(nullptr) == Op::None);
    for (const auto& item : names) {
      auto* function = ar::Function::create(bundle, type, item.first, false);
      auto* original_type = function->type();
      require(ar::pthread_operation(function) == item.second);
      require(function->type() == original_type && !function->is_intrinsic());
      require(ar::is_pthread_create(function) == (item.second == Op::Create));
      require(ar::is_pthread_join(function) == (item.second == Op::Join));
    }
    const std::pair<ar::Intrinsic::ID, Op> intrinsics[] = {
      {ar::Intrinsic::PthreadCreate, Op::Create}, {ar::Intrinsic::PthreadJoin, Op::Join},
      {ar::Intrinsic::PthreadMutexLock, Op::MutexLock}, {ar::Intrinsic::PthreadMutexUnlock, Op::MutexUnlock}
    };
    for (const auto& item : intrinsics) {
      auto* function = bundle->intrinsic_function(item.first);
      auto* original_type = function->type();
      require(ar::pthread_operation(function) == item.second);
      require(function->type() == original_type && function->intrinsic_id() == item.first);
    }
    // A spelling that resembles an intrinsic does not supply its actual ID.
    for (const char* name : {"ar.pthread.create.fake", "ar.pthread.join.fake", "my_pthread_create",
                             "pthread_join_suffix", "pthread_mutex_trylock_suffix"})
      require(ar::pthread_operation(ar::Function::create(bundle, type, name, false)) == Op::None);
    require(ar::pthread_operation(bundle->intrinsic_function(ar::Intrinsic::LibcAbort)) == Op::None);
    // Identification deliberately grants neither valid prototype nor extern status.
    auto* malformed_bundle = ar::Bundle::create(context,
        ar::DataLayout::create(ar::LittleEndian, {bits, bits / 8, bits / 8}), "");
    auto* malformed = ar::Function::create(malformed_bundle,
        ar::FunctionType::get(context, ar::VoidType::get(context), {}, false),
        "pthread_create", true);
    require(ar::is_pthread_create(malformed) && malformed->is_definition());
    auto* join = bundle->intrinsic_function(ar::Intrinsic::PthreadJoin);
    require(join->type()->param_type(0)->is_pointer());
    require(type->param_type(0)->is_integer() &&
            ar::cast<ar::IntegerType>(type->param_type(0))->bit_width() == bits);
    // Conditional and shared/exclusive operations must remain distinct.
    require(Op::MutexLock != Op::MutexTryLock && Op::MutexTryLock != Op::MutexTimedLock);
    require(Op::RwlockReadLock != Op::RwlockWriteLock && Op::RwlockUnlock != Op::MutexUnlock);
  }
  std::cout << checks << " dual-ABI pthread identity checks passed\n";
}
