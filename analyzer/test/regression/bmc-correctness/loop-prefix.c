// SPDX-License-Identifier: MIT
#include <pthread.h>
#include <stdlib.h>
#ifndef LIMIT
#define LIMIT 43
#endif
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long counter;
#else
typedef int counter;
#endif
extern int __VERIFIER_nondet_int(void);
volatile int progress;
volatile int flag;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
counter advance(counter value) { return value + 1; }
void *worker(void *unused) {
#if CASE == 1
  pthread_mutex_lock(&mutex);
#endif
  (void)flag;
#if CASE == 1
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}
int main(void) {
  pthread_t child;
  pthread_create(&child, 0, worker, 0);
  counter i;
  for (i = 0; i <= LIMIT;
#ifdef HELPER
       i = advance(i)
#else
       ++i
#endif
       ) progress = i;
#if CASE == 2
  if (i != LIMIT + 2) abort();
#elif CASE == 3
  int unavailable;
  i = unavailable;
#elif CASE == 4
  i = 0;
  if (i != LIMIT + 1) abort();
#elif CASE == 5
  i = __VERIFIER_nondet_int();
  if (i != LIMIT + 1) abort();
#endif
#if CASE == 1
  pthread_mutex_lock(&mutex);
#endif
  flag = i;
#if CASE == 1
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}
