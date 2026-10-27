// Retained main-lock scope and independently joined descendants.
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
static datum payload;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_t first, second, third, fourth;
static void *holder(void *unused) {
  (void)unused;
  pthread_mutex_lock(&gate);
  ++payload;
  pthread_mutex_unlock(&gate);
  return 0;
}
static void *noop(void *unused) { (void)unused; return 0; }
static void *tail(void *unused) {
  (void)unused;
#if CASE == 1 || CASE == 26 || CASE == 33 || CASE == 34
  ++payload;
#endif
  return 0;
}
static void *leaf(void *unused) {
  (void)unused;
  ++payload;
#if CASE == 1 || CASE == 26
  pthread_create(&fourth, 0, tail, 0);
# if CASE == 1
  pthread_join(fourth, 0);
# endif
#endif
  return 0;
}
#if CASE == 28
int pthread_join(pthread_t t, void **out) { (void)t; (void)out; return 0; }
#endif
static void *middle(void *unused) {
  (void)unused;
#if CASE == 34
  pthread_create(&fourth, 0, tail, 0);
#endif
#if CASE == 17
  for (int n = 0; n != 2; ++n)
#endif
  pthread_create(&third, 0, leaf, 0);
#if CASE == 33
  pthread_create(&fourth, 0, tail, 0);
#endif
#if CASE == 2 || CASE == 14 || CASE == 15
  pthread_create(&fourth, 0, noop, 0);
#endif
#if CASE == 11
  if (__VERIFIER_nondet_int()) return 0;
#endif
#if CASE == 15
  third = fourth;
#elif CASE == 16
  pthread_detach(third);
#elif CASE == 19
  volatile pthread_t observed = third;
  (void)observed;
#endif
#if CASE == 12
  if (__VERIFIER_nondet_int())
#endif
#if CASE == 14
  pthread_join(fourth, 0);
#elif CASE == 20
  void *result;
  pthread_join(third, &result);
#elif CASE != 10 && CASE != 16
  pthread_join(third, 0);
#endif
#if CASE == 2 || CASE == 33 || CASE == 34
  pthread_join(fourth, 0);
#endif
#if CASE == 32
  return &payload;
#else
  return 0;
#endif
}
int main(void) {
  pthread_create(&first, 0, holder, 0);
  pthread_mutex_lock(&gate);
#if CASE == 18
  pthread_create(&fourth, 0, leaf, 0);
#endif
  pthread_create(&second, 0, middle, 0);
#if CASE == 24
  ++payload;
#endif
#if CASE == 13
  pthread_mutex_unlock(&gate);
#elif CASE == 22
  if (__VERIFIER_nondet_int()) pthread_mutex_unlock(&gate);
#elif CASE == 21
  second = third;
#elif CASE == 29
  *((volatile unsigned char *)&gate) = 1;
#endif
#if CASE != 31
  pthread_join(second, 0);
#endif
#if CASE != 13
  pthread_mutex_unlock(&gate);
#endif
  return 0;
}
