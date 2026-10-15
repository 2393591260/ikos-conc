// SPDX-License-Identifier: MIT
#include <pthread.h>
#include <stdint.h>
#ifndef N
#define N 2
#endif
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long counter;
#else
typedef intptr_t counter;
#endif
extern void __VERIFIER_atomic_begin(void);
extern void __VERIFIER_atomic_end(void);
extern void __VERIFIER_assume(int);
intptr_t value;
void __VERIFIER_atomic_publish(intptr_t id) { value = id; }
void *worker(void *argument) {
  intptr_t id = (intptr_t)argument;
#if CASE == 5
  if (id != 2) return 0;
#endif
#if CASE == 7
  __VERIFIER_atomic_publish(id);
#else
  __VERIFIER_atomic_begin();
  value = id;
  __VERIFIER_atomic_end();
#endif
#if CASE == 1
  __VERIFIER_assume(0);
#elif CASE == 9
  __VERIFIER_assume(value == id);
#endif
  return 0;
}
int main(void) {
  pthread_t threads[N + 1];
  int token = 0;
  __VERIFIER_atomic_begin();
  value = 0;
  __VERIFIER_atomic_end();
#if CASE == 11
  for (counter i = (1LL << 32) + 1; i <= (1LL << 32) + N; ++i) {
#else
  for (counter i = 1; i <= N; ++i) {
#endif
#if CASE == 6
    pthread_create(&threads[(intptr_t)i], 0, worker, &token);
#else
    pthread_create(&threads[(intptr_t)i], 0, worker, (void *)(intptr_t)i);
#endif
  }
#if CASE == 3
  for (counter i = 1; i <= N; ++i) pthread_join(threads[i], 0);
#endif
  intptr_t observed = value;
#if CASE == 2
  __VERIFIER_assume(0);
#elif CASE == 8
  __VERIFIER_assume(observed < 0);
#elif CASE == 10
  __VERIFIER_assume(observed == N);
#else
  __VERIFIER_assume(observed <= N);
#endif
#if CASE != 3
  for (counter i = 1; i <= N; ++i) {
#if CASE == 4
    pthread_join(threads[1], 0);
#else
    pthread_join(threads[i], 0);
#endif
  }
#endif
  return 0;
}
