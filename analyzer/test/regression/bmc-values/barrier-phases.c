// SPDX-License-Identifier: MIT
#include <pthread.h>
#include <stdlib.h>
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
#if CASE == 41 || CASE == 42
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
#if CASE != 1 && CASE != 2 && CASE != 3 && CASE != 8 && CASE != 16 && CASE != 28 && CASE != 31 && CASE != 32 && CASE != 41 && CASE != 42
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
#if CASE == 41 || CASE == 42
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
#else
  unsigned count = 2;
#endif
#if CASE == 29
  pthread_barrierattr_t attr;
  pthread_barrier_init(&gate, &attr, count);
#else
# if CASE == 33
  if (pthread_barrier_init(&gate, 0, count)) return 0;
# else
  pthread_barrier_init(&gate, 0, count);
# endif
#endif
#if CASE == 41 || CASE == 42
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
#if CASE == 41 || CASE == 42
  pthread_barrier_wait(&other);
#endif
#if CASE != 1 && CASE != 3 && CASE != 32
  payload = 3;
#endif
  return 0;
}
