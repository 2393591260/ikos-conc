#define _GNU_SOURCE
#include <pthread.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
extern int __VERIFIER_nondet_int(void);
static pthread_barrier_t gate;
static datum payload;
static volatile int side;
#if CASE == 13
int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *a,
                         unsigned n) { (void)b; (void)a; (void)n; return 0; }
#endif
static void *worker(void *unused) {
  (void)unused;
#if CASE == 14 || CASE == 15 || CASE == 16
  pthread_barrier_wait(&gate);
  payload = 2;
#else
  payload = 2;
  pthread_barrier_wait(&gate);
#endif
#if CASE == 12
  pthread_barrier_wait(&gate);
#endif
  return 0;
}
int main(int argc, char **argv) {
  (void)argc; (void)argv;
#if CASE == 1 || CASE == 15
  int capacity = 1;
#elif CASE == 2 || CASE == 16
  int capacity = 2;
#elif CASE == 3
  int capacity = __VERIFIER_nondet_int() ? 1 : 2;
#elif CASE == 7
  int capacity = 0;
#elif CASE == 8
  int capacity = -1;
#else
  int capacity = __VERIFIER_nondet_int();
# if CASE == 4
  if (capacity != 2) return 0;
# elif CASE == 17
  /* The successor join block dominates init, but the guarding edge does not. */
  if (capacity < 1) side = 1;
  if (capacity > 2) return 0;
# elif CASE == 18
  if (!(capacity >= 1 && capacity <= 2)) return 0;
# elif CASE == 19
  if ((unsigned)(capacity - 1u) > 1u) return 0;
# else
  if (capacity < 1 || capacity > 2) return 0;
# endif
#endif
#if CASE == 5
  int saved_capacity = capacity;
  pthread_barrier_init(&gate, 0, saved_capacity);
#elif CASE == 6
  pthread_barrier_init(&gate, 0, __VERIFIER_nondet_int());
#else
  pthread_barrier_init(&gate, 0, capacity);
#endif
#if CASE == 9
  pthread_barrier_init(&gate, 0, capacity);
#elif CASE == 10
  ((volatile unsigned char *)&gate)[0] = 0;
#endif
#if CASE == 20
  /* Init saw {1,2}; this actual later path requires the same count to be2. */
  if (capacity != 2) return 0;
#endif
  pthread_t child;
  pthread_create(&child, 0, worker, 0);
#if CASE == 11
  if (pthread_barrier_wait(&gate) == PTHREAD_BARRIER_SERIAL_THREAD) payload = 3;
#else
  pthread_barrier_wait(&gate);
  payload = 3;
#endif
  return 0;
}
