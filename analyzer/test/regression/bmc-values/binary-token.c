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
#if CASE == 14
int token = 1;
#elif CASE == 15
_Thread_local int token;
#else
int token;
#endif
int second_token, unrelated, shared;
datum payload;
#if CASE == 13
int* escaped = &token;
#endif
extern int __VERIFIER_nondet_int(void);
static void condition(int yes) {
#if CASE == 27
  if (__VERIFIER_nondet_int()) return;
#endif
  if (!yes) abort();
}
#if CASE == 4
#define acquire ordinary_acquire
#define release ordinary_release
#else
#define acquire __VERIFIER_atomic_enter_renamed
#define release __VERIFIER_atomic_leave_renamed
#endif
#if CASE == 10
static void pointer_condition(int* p) { condition(*p == 0); }
#endif
void acquire(int* p) {
#if CASE == 10
  pointer_condition(p);
#elif CASE == 31
  condition(shared == 0);
#elif CASE != 5
#if CASE == 7
  if (*p != 0) return;
#elif CASE == 26
  if (__VERIFIER_nondet_int()) return;
  condition(*p == 0);
#else
  condition(*p == 0);
#endif
#endif
#if CASE == 8
  *p = 0;
#elif CASE == 36
  *p = 0; *p = 1;
#else
  *p = 1;
#endif
}
void release(int* p) {
#if CASE == 35
  condition(*p == 2);
#elif CASE != 6
  condition(*p == 1);
#endif
#if CASE == 9
  *p = 1;
#else
  *p = 0;
#endif
}
static void lock_helper(void) { acquire(&token); }
static void unlock_helper(void) { release(&token); }
static void access_data(void) {
#if CASE == 25
  ((char*)&payload)[0] = 1;
#else
  payload++;
#endif
}
static void nested_lock(void) { lock_helper(); }
#if CASE == 32
static void owned_forever(void) { for (;;) access_data(); }
#endif
static void* worker(void* ignored) {
#if CASE == 3
  for (;;) {
#endif
#if CASE == 11
  token = 0;
#elif CASE == 12
  ((char*)&token)[0] = 0;
#elif CASE == 17
  release(&token);
#elif CASE == 22
  access_data();
#elif CASE == 23
  payload++;
#elif CASE == 24
  unrelated++;
#endif
#if CASE == 1
  lock_helper();
#elif CASE == 33
  nested_lock();
#elif CASE == 16
  if (__VERIFIER_nondet_int()) acquire(&token);
#elif CASE == 34
  acquire((int*)(char*)&token);
#else
  acquire(&token);
#endif
#if CASE == 32
  owned_forever();
#elif CASE == 18
  acquire(&token);
#endif
#if CASE != 21
  access_data();
#endif
#if CASE == 2
  if (__VERIFIER_nondet_int()) return 0; /* A dead owner keeps token=1. */
#endif
#if CASE == 20
  release(&second_token);
#elif CASE == 1
  unlock_helper();
#else
  release(&token);
#endif
#if CASE == 19
  release(&token);
#elif CASE == 21
  access_data();
#endif
#if CASE == 3
  }
#endif
  return 0;
}
#if CASE == 17 || CASE == 28 || CASE == 29 || CASE == 30
static void* second(void* ignored) {
#if CASE == 17 || CASE == 29
  acquire(&token);
#endif
  access_data();
#if CASE == 17 || CASE == 29
  release(&token);
#endif
  return 0;
}
#endif
#if CASE == 30
static void* creator(void* ignored) {
  pthread_t t;
  pthread_create(&t, 0, second, 0);
  return 0;
}
#endif
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, worker, 0);
#if CASE == 17 || CASE == 28 || CASE == 29
  pthread_create(&b, 0, second, 0);
#elif CASE == 30
  pthread_create(&b, 0, creator, 0);
#else
  pthread_create(&b, 0, worker, 0);
#endif
  return 0;
}
