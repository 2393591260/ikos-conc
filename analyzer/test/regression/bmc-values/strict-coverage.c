// SPDX-License-Identifier: MIT
#include <pthread.h>
#include <stdint.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
static datum payload;
#if CASE == 10
static datum alternative;
#endif
static pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
extern void missing_effect(void);
extern void *missing_entry(void *);
extern int __VERIFIER_nondet_int(void);
#if CASE == 6
static void *numeric = (void *)17;
#endif
#if CASE == 7
static _Atomic int mixed;
#endif
static void *worker(void *argument) {
#if CASE == 0
  pthread_mutex_lock(&guard);
#endif
#if CASE == 10
  datum *destination = __VERIFIER_nondet_int() ? &payload : &alternative;
  *destination = 1;
#elif CASE == 8
  if (__VERIFIER_nondet_int()) payload = 1;
  else payload = 2;
#else
  payload = 1;
#endif
#if CASE == 0
  pthread_mutex_unlock(&guard);
#endif
#if CASE == 7
  mixed = 1;
#endif
  return 0;
}
#if CASE == 7
static void *ordinary(void *argument) { *((int *)&mixed) = 2; return 0; }
#endif
#if CASE == 9
__attribute__((destructor)) static void late_write(void) { payload = 3; }
#endif
int main(void) {
#if CASE == 4
  missing_effect();
  return 0;
#elif CASE == 5
  return 0;
#else
  pthread_t first, second, other;
  pthread_create(&first, 0, worker, 0);
  pthread_create(&second, 0, worker, 0);
#if CASE == 2
  missing_effect();
#elif CASE == 3
  pthread_create(&other, 0, missing_entry, 0);
#elif CASE == 6
  // Keep the initializer's registered integer-pointer conversion observable.
  if (__VERIFIER_nondet_int()) return numeric != 0;
#elif CASE == 7
  pthread_create(&other, 0, ordinary, 0);
#endif
  return 0;
#endif
}
