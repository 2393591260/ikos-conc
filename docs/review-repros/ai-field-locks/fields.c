#include <pthread.h>

#ifndef CASE
#define CASE 0
#endif

struct object {
#ifdef REORDER
  int padding, value;
  pthread_mutex_t second, first;
#else
  pthread_mutex_t first, second;
  int value;
#endif
};

static struct object state = {
    .first = PTHREAD_MUTEX_INITIALIZER,
    .second = PTHREAD_MUTEX_INITIALIZER,
};
static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;

static void *writer_a(void *unused) {
#if CASE >= 3
  pthread_rwlock_rdlock(&rw);
#else
  pthread_mutex_lock(&state.first);
#endif
  state.value = 1;
#if CASE >= 3
  pthread_rwlock_unlock(&rw);
#else
  pthread_mutex_unlock(&state.first);
#endif
  return 0;
}

static void *writer_b(void *unused) {
#if CASE == 0
  pthread_mutex_lock(&state.second);
#elif CASE == 1
  pthread_mutex_lock(&state.first);
#elif CASE == 3
  pthread_rwlock_rdlock(&rw);
#elif CASE == 4
  pthread_rwlock_wrlock(&rw);
#endif
  state.value = 2;
#if CASE == 0
  pthread_mutex_unlock(&state.second);
#elif CASE == 1
  pthread_mutex_unlock(&state.first);
#elif CASE >= 3
  pthread_rwlock_unlock(&rw);
#endif
  return 0;
}

int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, writer_a, 0);
  pthread_create(&b, 0, writer_b, 0);
  pthread_join(a, 0);
  pthread_join(b, 0);
  return 0;
}
