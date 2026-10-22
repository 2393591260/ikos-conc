// SPDX-License-Identifier: MIT
#include <pthread.h>
#include <stdlib.h>
#include <stdatomic.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
extern int __VERIFIER_nondet_int(void);
static datum payload;
#if CASE == 18
static __thread pthread_barrier_t gate;
#else
static pthread_barrier_t gate;
#endif
static pthread_barrier_t other;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
#if CASE == 19
int pthread_barrier_wait(pthread_barrier_t *p) { (void)p; ++payload; return 0; }
#endif
#if CASE == 21
static void helper(void) { pthread_barrier_wait(&gate); }
#endif
static void *worker(void *arg) {
  (void)arg;
#if CASE == 41 || CASE == 42 || CASE == 54 || CASE == 55
  pthread_barrier_wait(&gate);
#endif
#if CASE == 1 || CASE == 2 || CASE == 8 || CASE == 28 || CASE == 31 || CASE == 32
# if CASE == 21
  helper();
# else
  pthread_barrier_wait(&gate);
# endif
#endif
#if CASE == 6 || CASE == 7
  pthread_mutex_lock(&lock);
#endif
#if CASE == 16
  pthread_barrier_wait(&other);
#endif
#if CASE == 31
  if (payload) payload = 2;
#else
  payload = 2;
#endif
#if CASE == 6 || CASE == 7
  pthread_mutex_unlock(&lock);
#endif
#if CASE != 1 && CASE != 2 && CASE != 3 && CASE != 8 && CASE != 16 && CASE != 28 && CASE != 31 && CASE != 32 && CASE != 41 && CASE != 42 && CASE != 54 && CASE != 55
# if CASE == 9
  if (__VERIFIER_nondet_int())
# endif
# if CASE == 21
  helper();
# else
  pthread_barrier_wait(&gate);
# endif
#endif
#if CASE == 10
  payload = 4;
  pthread_barrier_wait(&gate);
#elif CASE == 27 || CASE == 34
  payload = 4;
#endif
#if CASE == 41 || CASE == 42 || CASE == 54 || CASE == 55
  pthread_barrier_wait(&other);
#endif
  return 0;
}
int main(int argc, char **argv) {
  (void)argc; (void)argv;
  pthread_t first, second;
#if CASE == 17
  pthread_barrier_t gate;
#elif CASE == 24
  pthread_barrier_wait(&gate);
#endif
#if CASE == 4
  unsigned count = 1;
#elif CASE == 5 || CASE == 6 || CASE == 27
  unsigned count = 3;
#elif CASE == 34
  unsigned count = 4;
#elif CASE == 22
  unsigned count = 0;
#elif CASE == 23
  unsigned count = 2147483647U;
#elif CASE == 25
  unsigned count = argc > 1 ? 1 : 2;
#elif CASE == 30
  unsigned count = 4294967295U;
#elif CASE == 35
  unsigned count = argc > 1 ? 2 : 3;
#elif CASE == 36
  unsigned count = 4; count -= 2;
#elif CASE == 37
  unsigned count = argc > 1 ? 2 : (unsigned)__VERIFIER_nondet_int();
#elif CASE == 38
  unsigned count = argc > 1 ? 0 : 2;
#elif CASE == 39
  unsigned count = argc > 1 ? 2 : 2147483647U;
#elif CASE == 40
  unsigned long long large = 4294967297ULL;
  unsigned count = (unsigned)large;
#elif CASE == 41
  unsigned count = 1;
#elif CASE == 43
  unsigned count = __VERIFIER_nondet_int() ? 2 : 3;
#elif CASE == 44
  unsigned count = 4294967295U; count += 3;
#elif CASE == 45 || CASE == 47 || CASE == 48 || CASE == 51 || CASE == 52 || CASE == 53 || CASE == 54 || CASE == 55 || CASE == 60 || CASE == 61 || CASE == 62
  struct { unsigned capacity; } config;
# if CASE != 51
  config.capacity = CASE == 54 ? 1 : 2;
# endif
# if CASE == 47
  ((unsigned char *)&config.capacity)[0] = 1;
# elif CASE == 48
  extern void escape(void *);
  escape(&config);
# elif CASE == 52
  if (argc > 1) config.capacity = 1;
# elif CASE == 60
  unsigned old = config.capacity;
  payload = old;
# elif CASE == 61
  if (argc > 1) config.capacity = 2;
  else config.capacity = 2;
# elif CASE == 62
  extern volatile unsigned unrelated;
  payload = unrelated;
# endif
  unsigned count = config.capacity;
# if CASE == 53
  config.capacity = 1;
# endif
#elif CASE == 46 || CASE == 67
  struct { unsigned padding; struct { unsigned capacity; } inner; } config;
  config.padding = 0;
  config.inner.capacity = CASE == 67 ? 3 : 2;
  unsigned count = config.inner.capacity;
#elif CASE == 49
  struct { volatile unsigned capacity; } config;
  config.capacity = 2;
  unsigned count = config.capacity;
#elif CASE == 50
  struct { _Atomic unsigned capacity; } config;
  atomic_store(&config.capacity, 2);
  unsigned count = atomic_load(&config.capacity);
#elif CASE == 56 || CASE == 57
  struct { unsigned long long capacity; } config;
  config.capacity = CASE == 56 ? 2ULL : 4294967297ULL;
  unsigned count = (unsigned)config.capacity;
#elif CASE == 58
  union { unsigned long long wide; unsigned capacity; } config;
  config.wide = 2;
  unsigned count = config.capacity;
#elif CASE == 59
  struct { float capacity; } config;
  config.capacity = 2.0f;
  unsigned count = *(unsigned *)&config.capacity;
#elif CASE == 63
  struct { unsigned capacity; unsigned other; } config = {2, 0};
  unsigned count = config.capacity;
#elif CASE == 64
  struct { unsigned capacity; } config[2];
  config[1].capacity = 2;
  unsigned count = config[1].capacity;
#elif CASE == 68 || CASE == 69 || CASE == 70 || CASE == 71 || CASE == 72 || CASE == 73 || CASE == 80 || CASE == 81
  unsigned count = CASE == 72 ? 4294967295U : (CASE == 80 ? 5 : 4);
# if CASE == 71
  count = (unsigned)__VERIFIER_nondet_int();
# endif
  while (count > (CASE == 70 ? 1U : (CASE == 80 ? 3U : 2U))) {
# if CASE == 73
    if (argc > 1) break;
# endif
# if CASE == 69
    count -= 2;
# elif CASE == 81
    count -= (unsigned)argc;
# else
    --count;
# endif
  }
#elif CASE == 74
  unsigned count = 0;
  while (count < 2) ++count;
#elif CASE == 84
  unsigned long long large = 4;
  while (large > 2) --large;
  unsigned count = (unsigned)large;
#else
  unsigned count = 2;
#endif
#if CASE == 79
  for (unsigned n = 0; n < 2; ++n) pthread_barrier_init(&gate, 0, count);
#elif CASE == 29
  pthread_barrierattr_t attr;
  pthread_barrier_init(&gate, &attr, count);
#else
# if CASE == 33
  if (pthread_barrier_init(&gate, 0, count)) return 0;
# else
  pthread_barrier_init(&gate, 0, count);
# endif
#endif
#if CASE == 54 || CASE == 55
  config.capacity = CASE == 54 ? 2 : 1;
  pthread_barrier_init(&other, 0, config.capacity);
  config.capacity = 3;
#elif CASE == 41 || CASE == 42
  count = CASE == 41 ? 2 : 1;
  pthread_barrier_init(&other, 0, count);
  count = CASE == 41 ? 1 : 2;
#elif CASE == 12
  pthread_barrier_init(&gate, 0, 2);
#elif CASE == 13
  pthread_barrier_destroy(&gate);
#elif CASE == 14
  *((char *)&gate) = 0;
#elif CASE == 15
  extern void escape(void *);
  escape(&gate);
#elif CASE == 16
  pthread_barrier_init(&other, 0, 2);
#elif CASE == 20
  extern void hidden(void);
  hidden();
#endif
#if CASE == 11
  for (int i=0;i<2;++i) pthread_create(&first, 0, worker, 0);
#else
  pthread_create(&first, 0, worker, 0);
#endif
#if CASE == 77
  for (int n = 0; n < argc; ++n) payload = 7;
#endif
#if CASE == 6 || CASE == 7 || CASE == 27 || CASE == 34
  pthread_create(&second, 0, worker, 0);
#endif
#if CASE == 1 || CASE == 3
  payload = 3;
#endif
#if CASE != 3 && CASE != 8
# if CASE == 28
  if (argc > 1)
# endif
# if CASE == 32
  if (pthread_barrier_wait(&gate) == PTHREAD_BARRIER_SERIAL_THREAD) payload = 3;
# else
  pthread_barrier_wait(&gate);
# endif
#endif
#if CASE == 41 || CASE == 42 || CASE == 54 || CASE == 55
  pthread_barrier_wait(&other);
#endif
#if CASE == 78
  for (int n = 0; n < argc; ++n) payload = 8;
#endif
#if CASE != 1 && CASE != 3 && CASE != 32
  payload = 3;
#endif
  return 0;
}
