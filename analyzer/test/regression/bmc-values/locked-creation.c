// SPDX-License-Identifier: MIT
#define _GNU_SOURCE
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
#if CASE == 17
static pthread_mutex_t gate = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
#else
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
#endif
static pthread_mutex_t other = PTHREAD_MUTEX_INITIALIZER;
static pthread_t first, second;
#if CASE == 18
__attribute__((noinline)) static void helper(void) { ++payload; }
#endif
static void *unlocked(void *arg) {
  (void)arg;
#if CASE == 47
  if (payload) abort();
#else
  ++payload;
#endif
  return 0;
}
#if CASE >= 48
static pthread_t third;
static void *leaf(void *arg) {
  (void)arg;
  ++payload;
  return 0;
}
static void *noop(void *arg) { (void)arg; return 0; }
static void *joined_child(void *arg) {
  (void)arg;
#if CASE == 49 || CASE == 62
  pthread_create(&third, 0, leaf, 0);
# if CASE == 49
  pthread_join(third, 0);
# endif
#else
  ++payload;
#endif
  return 0;
}
static void *holder(void *arg) {
  (void)arg;
  pthread_mutex_lock(&gate);
  ++payload;
  pthread_mutex_unlock(&gate);
  return 0;
}
#if CASE == 60
int pthread_join(pthread_t id, void **out) { (void)id; (void)out; return 0; }
#endif
int main(void) {
#if CASE == 57
  pthread_t local;
#endif
  pthread_create(&first, 0, holder, 0);
  pthread_mutex_lock(&gate);
#if CASE == 61
  if (__VERIFIER_nondet_int())
#endif
#if CASE == 57
  pthread_create(&local, 0, joined_child, 0);
#else
  pthread_create(&second, 0, joined_child, 0);
#endif
#if CASE == 51 || CASE == 52
  pthread_create(&third, 0, noop, 0);
#endif
#if CASE == 52
  second = third;
#elif CASE == 54
  pthread_detach(second);
#elif CASE == 58
  ++payload;
#elif CASE == 63
  third = second;
#elif CASE == 64
  volatile pthread_t copy = second;
  (void)copy;
#endif
#if CASE == 50
  pthread_mutex_unlock(&gate);
#endif
#if CASE == 56
  if (__VERIFIER_nondet_int())
#endif
#if CASE == 51 || CASE == 63
  pthread_join(third, 0);
#elif CASE == 55
  void *result;
  pthread_join(second, &result);
#elif CASE == 57
  pthread_join(local, 0);
#else
  pthread_join(second, 0);
#endif
#if CASE == 53
  pthread_join(second, 0);
#endif
#if CASE != 50 && CASE != 59
  pthread_mutex_unlock(&gate);
#endif
#if CASE == 52
  ++payload;
#endif
  return 0;
}
#elif CASE >= 30
static pthread_t third, fourth;
static void *middle(void *arg) {
  (void)arg;
  pthread_create(&third, 0, unlocked, 0);
  return 0;
}
static void *bypass(void *arg) {
  (void)arg;
#if CASE == 47
  pthread_mutex_lock(&gate);
#endif
  pthread_create(&fourth, 0, unlocked, 0);
#if CASE == 47
  pthread_mutex_unlock(&gate);
#endif
  return 0;
}
static void *starter(void *arg) {
  (void)arg;
#if CASE == 31
  pthread_create(&second, 0, unlocked, 0);
#endif
#if CASE == 35
  int take = __VERIFIER_nondet_int();
  if (take)
#endif
#if CASE == 41
  pthread_mutex_lock(&other);
#else
  pthread_mutex_lock(&gate);
#endif
#if CASE != 31 && CASE != 32
# if CASE == 37
  pthread_create(&second, 0, middle, 0);
# elif CASE == 39
  pthread_create(&second, 0, starter, 0);
# else
  pthread_create(&second, 0, unlocked, 0);
# endif
#endif
#if CASE == 33
  pthread_create(&fourth, 0, unlocked, 0);
#endif
#if CASE == 35
  if (take)
#endif
#if CASE == 41
  pthread_mutex_unlock(&other);
#else
  pthread_mutex_unlock(&gate);
#endif
#if CASE == 32
  pthread_create(&second, 0, unlocked, 0);
#elif CASE == 39 || CASE == 45
  ++payload;
#endif
  return 0;
}
int main(void) {
#if CASE == 38
  pthread_create(&third, 0, unlocked, 0);
#elif CASE == 44
  pthread_create(&third, 0, starter, 0);
#endif
  pthread_mutex_lock(&gate);
  pthread_create(&first, 0, starter, 0);
#if CASE == 47
  pthread_create(&third, 0, bypass, 0);
#endif
#if CASE == 40
  pthread_create(&third, 0, unlocked, 0);
#endif
#if CASE == 34
  pthread_mutex_unlock(&gate);
#endif
  ++payload;
#if CASE != 34 && CASE != 46
  pthread_mutex_unlock(&gate);
#endif
#if CASE == 36
  pthread_create(&third, 0, bypass, 0);
#endif
  return 0;
}
#else
static void *worker(void *arg) {
  (void)arg;
#if CASE == 19
  pthread_create(&second, 0, unlocked, 0);
#endif
#if CASE != 9
# if CASE == 23 || CASE == 25
  int take = __VERIFIER_nondet_int();
  if (take)
# endif
# if CASE == 10 || CASE == 26
  pthread_mutex_lock(&other);
# else
  pthread_mutex_lock(&gate);
# endif
#endif
#if CASE == 1 || CASE == 14 || CASE == 25 || CASE == 26
  ++payload;
#endif
#if CASE != 9
# if CASE == 23 || CASE == 25
  if (take)
# elif CASE == 24
  if (__VERIFIER_nondet_int())
# endif
# if CASE == 10 || CASE == 26
  pthread_mutex_unlock(&other);
# else
  pthread_mutex_unlock(&gate);
# endif
#endif
#if CASE != 1 && CASE != 14 && CASE != 25 && CASE != 26
# if CASE == 18
  helper();
# else
  ++payload;
# endif
#endif
  return 0;
}
int main(void) {
#if CASE == 27
  pthread_t local;
#endif
#if CASE == 1 || CASE == 14 || CASE == 15 || CASE == 25 || CASE == 26
  pthread_create(&first, 0, worker, 0);
#elif CASE == 5 || CASE == 13
  pthread_create(&second, 0, worker, 0);
#endif
#if CASE == 6
  if (__VERIFIER_nondet_int())
#endif
  pthread_mutex_lock(&gate);
#if CASE == 3
  pthread_mutex_lock(&other);
#elif CASE == 7
  if (__VERIFIER_nondet_int()) pthread_mutex_unlock(&gate);
#elif CASE == 21
  *((char *)&gate) = 0;
#elif CASE == 22
  extern void unknown(void *);
  unknown(&gate);
#endif
#if CASE == 2
  if (__VERIFIER_nondet_int())
#endif
#if CASE == 1 || CASE == 11 || CASE == 12 || CASE == 14 || CASE == 15 || CASE == 25 || CASE == 26
  pthread_create(&second, 0, unlocked, 0);
#elif CASE == 27
  pthread_create(&local, 0, worker, 0);
#elif CASE == 29
  for (int i=0;i<2;++i) pthread_create(&first, 0, worker, 0);
#elif CASE != 5
  pthread_create(&first, 0, worker, 0);
#endif
#if CASE == 12
  pthread_create(&first, 0, unlocked, 0);
#elif CASE == 28
  pthread_create(&second, 0, worker, 0);
#endif
#if CASE == 8
  pthread_mutex_unlock(&gate);
#endif
#if CASE != 1 && CASE != 12 && CASE != 14 && CASE != 15 && CASE != 25 && CASE != 26
  ++payload;
#endif
#if CASE == 3
  pthread_mutex_unlock(&other);
#endif
#if CASE != 1 && CASE != 3 && CASE != 6 && CASE != 7 && CASE != 8 && CASE != 11 && CASE != 12 && CASE != 15 && CASE != 25 && CASE != 26
  pthread_mutex_unlock(&gate);
#endif
#if CASE == 16
  pthread_mutex_lock(&gate);
  pthread_mutex_unlock(&gate);
#elif CASE == 20
  pthread_join(first, 0);
#endif
  return 0;
}

#endif
