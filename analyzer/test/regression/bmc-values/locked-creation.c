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
static void *unlocked(void *arg) { (void)arg; ++payload; return 0; }
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
