// SPDX-License-Identifier: MIT
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
extern unsigned int __VERIFIER_nondet_uint(void);
datum shared;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

void *worker(void *arg) {
  void *p;
#if CASE == 0
  p = calloc(SIZE_MAX, 2);
  if (p) free(p);
  shared++;
#elif CASE == 1
  size_t n = __VERIFIER_nondet_uint();
  if (n <= SIZE_MAX / 2) abort();
  p = calloc(n, 2);
  shared++;
#elif CASE == 2
  size_t n = __VERIFIER_nondet_uint();
  p = calloc(n, 2);
  if (n > SIZE_MAX / 2) shared++;
#elif CASE == 3
  size_t n = __VERIFIER_nondet_uint();
  p = calloc(2, n);
  if (n > SIZE_MAX / 2) shared++;
#elif CASE == 4
  size_t n = __VERIFIER_nondet_uint(), m = __VERIFIER_nondet_uint();
  p = calloc(n, m);
  if (n > SIZE_MAX / 2 && m >= 2) shared++;
#elif CASE == 5
  size_t n = __VERIFIER_nondet_uint();
  p = calloc(n, 2);
  if (!p && n > SIZE_MAX / 2) shared++;
#elif CASE == 6
  p = calloc(SIZE_MAX, 2);
  if (p) shared++;
#elif CASE == 7
  p = calloc(SIZE_MAX, 2);
  if (!p) shared++;
#elif CASE == 8
  p = calloc(4, 4);
  pthread_mutex_lock(&mutex);
  shared++;
  pthread_mutex_unlock(&mutex);
#elif CASE == 9
  p = calloc(0, SIZE_MAX);
  shared++;
#elif CASE == 10
  p = calloc((unsigned long long)0x1ffffffffULL, 2);
  if (!p) shared++;
#elif CASE == 11
  size_t n = SIZE_MAX / 2 + 1;
  p = calloc(n * 2, 4); // Caller multiplication wraps to zero before the API.
  if (p) shared++;
#elif CASE == 12
  datum *first = NULL;
  for (unsigned int i = 0; i < 2; ++i) {
    size_t n = i ? SIZE_MAX : 1;
    p = calloc(n, 2 * sizeof(datum));
    if (i == 0) {
      first = p;
      if (!first) return NULL;
      pthread_mutex_lock(&mutex);
      *first = 1;
      pthread_mutex_unlock(&mutex);
    }
  }
  pthread_mutex_lock(&mutex);
  int trigger = *first == 1;
  pthread_mutex_unlock(&mutex);
  if (trigger) shared++;
#elif CASE == 13
  p = calloc(SIZE_MAX, 2);
  if (!p) {
    pthread_mutex_lock(&mutex);
    shared++;
    pthread_mutex_unlock(&mutex);
  }
#elif CASE == 14
  p = calloc(SIZE_MAX, 1); // Largest nonoverflowing product can still fail.
  if (!p) shared++;
#elif CASE == 15
  p = calloc((unsigned long long)0x100000000ULL, 2);
  shared++;
#endif
  return NULL;
}
int main(void) {
  pthread_t a, b;
  pthread_create(&a, NULL, worker, NULL);
  pthread_create(&b, NULL, worker, NULL);
  pthread_join(a, NULL);
  pthread_join(b, NULL);
}
