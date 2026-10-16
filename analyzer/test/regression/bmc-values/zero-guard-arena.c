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
datum cells[65];
int cursor = 1, shared;
extern int __VERIFIER_nondet_int(void);
void __VERIFIER_atomic_reserve(int* out) {
  if (cursor + 1 > 65) *out = 0;
  else { *out = cursor; cursor += 2; }
}
#if CASE == 14
static void condition_exit(unsigned char cond) {
#else
static void condition_exit(int cond) {
#endif
#if CASE == 3 || CASE == 4
  return;
#elif CASE == 5
  if (__VERIFIER_nondet_int()) return;
  abort();
#elif CASE == 15
  if (cond) abort();
#elif CASE == 6
  if (shared) return;
  abort();
#elif CASE == 11
  shared = 1;
  abort();
#elif CASE == 13
  condition_exit(cond);
#elif CASE == 1 || CASE == 18
  if (cond == 0) abort();
#elif CASE == 2
  abort();
#elif CASE == 12
  if (cond) return;
  abort();
#elif CASE == 17
  if ((unsigned)cond > 0) return;
  abort();
#elif CASE == 21
  if (cond == 1) abort();
#else
  if (!cond) abort();
#endif
}
static void before_exit(void) { cells[0] = 3; }
static void touch(void) {
  int ticket;
  __VERIFIER_atomic_reserve(&ticket);
#if CASE == 10
  if (__VERIFIER_nondet_int()) goto after;
#endif
#if CASE == 20
  if (ticket < 0) {
#elif CASE == 16
  if (ticket != 0) goto after;
  {
#else
  if (ticket == 0) {
#endif
#if CASE == 8
    cells[ticket] = 4; /* A real access before termination must remain. */
#elif CASE == 9
    before_exit(); /* No call before the proved terminating call. */
#endif
#if CASE == 7
    for (;;) { } /* Refuse this new helper rule; old CFG dominance may suffice. */
#elif CASE == 22
    abort();
#elif CASE == 3 || CASE == 21
    condition_exit(1);
#elif CASE == 23
    condition_exit(__VERIFIER_nondet_int());
#else
    condition_exit(0);
#endif
  }
after:
#if CASE == 19
  shared++;
#endif
  cells[ticket] = 1;
  cells[ticket + 1] = 2;
}
static void* thread(void* ignored) {
  for (;;) touch();
  return 0;
}
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, thread, 0);
  pthread_create(&b, 0, thread, 0);
  return 0;
}
