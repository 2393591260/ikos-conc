#include <pthread.h>
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
extern int __VERIFIER_nondet_int(void);
extern unsigned __VERIFIER_nondet_uint(void);
#ifndef CASE
#define CASE 0
#endif
int payload;
volatile int selector;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t other_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;
struct {int padding; struct {int value;} inner;} configuration = {3, {7}};
int table[2] = {4, 7};
int bump(int x) {return x + 1;}
int choose(void) {return __VERIFIER_nondet_int();}
int return_seven(void) {return 7;}
int return_eight(void) {return 8;}
int apply_callback(int (*callback)(void)) {return callback();}
void filter(int x) {if (x != 0) abort();}
void assume_wrapper(int x) {if (!x) abort();}
int stack_value(int initialize) {volatile int x; if (initialize) x = 7; return x;}
void locked_write(pthread_mutex_t* m) {pthread_mutex_lock(m); payload++; pthread_mutex_unlock(m);}
void* first(void* arg) {
#if CASE == 0 || CASE == 1
  if (selector != 0) return 0;
#elif CASE == 2
  int x = __VERIFIER_nondet_int(); if (x < 0 || x > 10) return 0;
  ++x; if (x != 3) return 0;
#elif CASE == 3 || CASE == 5
  unsigned x = __VERIFIER_nondet_uint(); if (x != UINT_MAX) return 0;
  unsigned y = x + 1U;
#if CASE == 3
  if (y != 0) return 0;
#else
  if (y > x) payload = 1; return 0;
#endif
#elif CASE == 4
  unsigned x = __VERIFIER_nondet_uint(); if (x != 256) return 0;
  if ((unsigned char)x != 0) return 0;
#elif CASE == 6
  int x = __VERIFIER_nondet_int(); if (x != -7) return 0;
  if (x / 3 != -2) return 0;
#elif CASE == 7 || CASE == 8
  unsigned n = __VERIFIER_nondet_uint();
#if CASE == 7
  if (n >= 32) return 0;
#else
  if (n != 32) return 0;
#endif
  if ((1U << n) != 2) return 0;
#elif CASE == 9
  int x = __VERIFIER_nondet_int(); if (x != 0) return 0;
  if (1 / x != 2) return 0;
#elif CASE == 10
  int x = __VERIFIER_nondet_int(); if (x < 0 || x > 10) return 0;
  if (bump(x) != 3) return 0;
#elif CASE == 11
  int a = choose(), b = choose(); if (a != 0 || b != 1) return 0;
#elif CASE == 12
  filter(1);
#elif CASE == 13
  int* pointer = arg; *pointer = 1; return 0;
#elif CASE == 14 || CASE == 15
  if (selector != 0) return 0;
#elif CASE == 16
  selector = 1; if (selector != 0) return 0;
#elif CASE == 18 || CASE == 19
  pthread_mutex_lock(&mutex);
  int value = selector;
  pthread_mutex_unlock(&mutex);
  if (value != 0) return 0;
#elif CASE == 21
  if (configuration.inner.value != 7) return 0;
#elif CASE == 22
  if (table[1] != 7) return 0;
#elif CASE == 23
  int x = __VERIFIER_nondet_int(); if (x != -1) return 0;
  if ((unsigned)x < 1U) return 0;
#elif CASE == 25
  volatile int local = __VERIFIER_nondet_int(); if (local != 7) return 0;
#elif CASE == 26
  (void)stack_value(1); payload = stack_value(0); return 0;
#elif CASE == 27
  assume_wrapper(0);
#elif CASE == 28
  assume_wrapper(1);
#elif CASE == 29
  int x = __VERIFIER_nondet_int(); if (x != 10) return 0;
  if (x % 3 != 1 || (x & 3) != 2 || (x ^ 3) != 9) return 0;
#elif CASE == 30
  int x = __VERIFIER_nondet_int(); if (x != INT_MAX) return 0;
  if (x + 1 != INT_MIN) return 0;
#elif CASE == 31
  int x = __VERIFIER_nondet_int(); if (x < 0 || x > 10) return 0;
  payload = x + 1; return 0;
#elif CASE == 32
  // Shared control cannot certify a purely local computation through a stale
  // abstract read. The reachable shared==1 branch overflows before the write.
  int x = __VERIFIER_nondet_int();
  if (selector) {if (x != INT_MAX) return 0;}
  else {if (x != 0) return 0; return 0;}
  payload = x + 1; return 0;
#elif CASE == 34 || CASE == 35
#if CASE == 34
  pthread_rwlock_rdlock(&rwlock);
#else
  pthread_rwlock_wrlock(&rwlock);
#endif
  payload++; pthread_rwlock_unlock(&rwlock); return 0;
#elif CASE == 36 || CASE == 37
  locked_write(&mutex); return 0;
#elif CASE == 38
  puts("a literal"); printf("value=%d %%", 7);
#elif CASE == 39
  // A printf %n side effect must never be covered by the output-only summary.
  int count; printf("%n", &count);
#elif CASE == 45
  payload = 1; selector = table[0]; return 0;
#elif CASE == 46
  int count = 1; printf("%n", &count);
  if (count != 1) return 0; // %n wrote zero; the shared write is unreachable.
#elif CASE == 49 || CASE == 50
  int index = __VERIFIER_nondet_int(); if (index < 0 || index >= 2) return 0;
  if (table[index] !=
#if CASE == 49
      7
#else
      9
#endif
      ) return 0;
#elif CASE == 51 || CASE == 52
  int index = __VERIFIER_nondet_int(); if (index != 0) return 0;
  if (table[index] != 0) return 0;
#elif CASE == 53
  int index = __VERIFIER_nondet_int(); if (index < 0 || index >= 2) return 0;
  table[index] = 10; table[1 - index] = 20;
  if (table[index] != 10) return 0;
#elif CASE == 54 || CASE == 55
  int a = apply_callback(return_seven), b = apply_callback(return_eight);
  if (a != 7 || b !=
#if CASE == 54
      8
#else
      7
#endif
      ) return 0;
#endif
  payload = 1; return 0;
}
void* second(void* arg) {
#if CASE == 45
  table[0] = 1; selector = payload; return 0;
#elif CASE == 51 || CASE == 52
  int index = __VERIFIER_nondet_int(); if (index != 0) return 0;
  if (table[index] !=
#if CASE == 51
      1
#else
      0
#endif
      ) return 0;
#elif CASE == 34 || CASE == 35
  pthread_rwlock_rdlock(&rwlock); payload++; pthread_rwlock_unlock(&rwlock); return 0;
#elif CASE == 36
  locked_write(&other_mutex); return 0;
#elif CASE == 37
  locked_write(&mutex); return 0;
#elif CASE == 0
  if (selector != 1) return 0;
#elif CASE == 1
  if (selector != 0) return 0;
#elif CASE == 18 || CASE == 19
  pthread_mutex_lock(&mutex); selector = 2; pthread_mutex_unlock(&mutex);
#endif
  payload = 2; return 0;
}
int main(void) {
  pthread_t a, b;
#if CASE == 47 || CASE == 48
  selector =
#if CASE == 47
      0
#else
      1
#endif
      ;
  int saved = selector; selector = 1 - selector;
  if (saved != 1) return 0;
#elif CASE == 51 || CASE == 52
  table[0] = __VERIFIER_nondet_int();
#endif
#if CASE == 45
  pthread_create(&a, 0, first, 0);
  for (int i = 0; i < 10; ++i) pthread_create(&b, 0, second, 0);
  return 0;
#elif CASE == 40 || CASE == 43 || CASE == 44
  int condition = __VERIFIER_nondet_int();
  if (condition) pthread_create(&a, 0, first, 0);
#if CASE == 43
  if (!condition) pthread_create(&b, 0, second, 0);
#elif CASE == 44
  if (condition) pthread_create(&b, 0, first, 0);
#else
  pthread_create(&b, 0, second, 0);
#endif
  return 0; // Successful creations may execute before main exits.
#else
#if CASE == 0 || CASE == 1
  selector = __VERIFIER_nondet_int();
#elif CASE == 14 || CASE == 18 || CASE == 32
  selector = 1;
#endif
#if CASE == 13
  pthread_create(&a, 0, first, &payload);
#else
  pthread_create(&a, 0, first, 0);
#endif
  pthread_create(&b, 0, second, 0);
  pthread_join(a, 0); pthread_join(b, 0); return 0;
#endif
}
