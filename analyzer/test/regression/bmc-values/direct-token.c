#include <pthread.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
#if TOKEN_BITS == 64
typedef long long token_type;
#elif TOKEN_BITS == 32
typedef int token_type;
#else
typedef _Bool token_type;
#endif
#if CASE == 19
volatile token_type latch = 1;
#elif CASE == 24
extern volatile token_type latch;
#else
volatile token_type latch;
#endif
volatile int payload;
int other, unrelated;
#if CASE == 10
volatile token_type* escape = &latch;
#endif
extern int __VERIFIER_nondet_int(void);
static void require(int condition) {
#if CASE == 21
  if (__VERIFIER_nondet_int()) return;
#endif
  if (!condition) abort();
}
#if CASE == 4
#define take ordinary_take
#define give ordinary_give
#else
#define take __VERIFIER_atomic_direct_take
#define give __VERIFIER_atomic_direct_give
#endif
void take(void) {
#if CASE == 22
  if (latch) return;
#elif CASE == 18
  require(other == 0);
#elif CASE != 5
  require(latch == 0);
#endif
#if CASE == 7
  latch = 0;
#elif CASE == 23
  latch = 2;
#else
  latch = 1;
#endif
#if CASE == 25
  payload++;
#endif
}
void give(void) {
#if CASE != 6
  require(latch == 1);
#endif
#if CASE == 8
  latch = 1;
#else
  latch = 0;
#endif
}
static void lock_helper(void) { take(); }
static void observe(void) {
#if CASE == 30
  (void)latch;
#else
  if (latch != 1) abort();
#endif
}
#if CASE == 29
void __VERIFIER_atomic_param_take(int* p) { require(*p == 0); *p = 1; }
void __VERIFIER_atomic_param_give(int* p) { require(*p == 1); *p = 0; }
#endif
static void* worker(void* ignored) {
#if CASE == 3
  for (;;) {
#endif
#if CASE == 9
  latch = 0;
#elif CASE == 11
  if (latch) unrelated = 0;
#elif CASE == 16
  *((volatile char*)&latch) = 0;
#elif CASE == 30
  observe();
#endif
#if CASE == 29
  __VERIFIER_atomic_param_take((int*)&latch);
#elif CASE == 1
  lock_helper();
#elif CASE == 31
  if (__VERIFIER_nondet_int()) take();
#else
  take();
#endif
#if CASE == 2
  if (__VERIFIER_nondet_int()) return 0;
#endif
#if CASE == 17
  extern void unknown(volatile int*);
  unknown(&payload);
#endif
#if CASE != 29
  observe();
#endif
  payload++;
#if CASE == 15
  take();
#endif
#if CASE == 29
  __VERIFIER_atomic_param_give((int*)&latch);
#else
  give();
#endif
#if CASE == 12
  if (latch) unrelated = 0;
#elif CASE == 13
  payload++;
#elif CASE == 14
  give();
#elif CASE == 28
  unrelated++;
#endif
#if CASE == 3
  }
#endif
  return 0;
}
#if CASE == 20
static void* observer(void* ignored) { if (latch) unrelated++; return 0; }
#elif CASE == 32
static void* observer(void* ignored) { give(); return 0; }
#endif
int main(void) {
  pthread_t a, b;
#if CASE == 26
  latch = 0;
#endif
  pthread_create(&a, 0, worker, 0);
#if CASE == 20 || CASE == 32
  pthread_create(&b, 0, observer, 0);
#else
  pthread_create(&b, 0, worker, 0);
#endif
  return 0;
}
