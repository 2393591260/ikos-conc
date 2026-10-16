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
volatile int phase;
volatile datum payload, other_payload;
int shared_output, unrelated;
int* escaped;
#ifdef NATIVE
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
#define take() pthread_mutex_lock(&mutex)
#define give() pthread_mutex_unlock(&mutex)
#else
volatile _Bool latch;
#if CASE == 8
#define enter ordinary_enter
#define leave ordinary_leave
#else
#define enter __VERIFIER_atomic_global_enter
#define leave __VERIFIER_atomic_global_leave
#endif
static void condition(int yes) { if (!yes) abort(); }
void enter(void) {
#if CASE != 9
  condition(latch == 0);
#endif
  latch = 1;
}
void leave(void) { condition(latch == 1); latch = 0; }
#define take() enter()
#define give() leave()
#endif
extern int __VERIFIER_nondet_int(void);
void construct(datum value) { payload = value; }
#if CASE == 25
void forward(datum value) { construct(value); }
#endif
void __VERIFIER_atomic_change(volatile datum* data, datum expected, datum desired, int* result) {
  if (*data == expected) { *data = desired; *result = 1; }
  else *result = 0;
}
static void* worker(void* ignored) {
  int result;
#if CASE == 23
  construct(1);
#endif
  take();
  if (phase == 0) {
#if CASE == 6
    phase = 1;
#endif
#if CASE == 1
    payload = 1;
#elif CASE == 2
    construct((datum)0x100000001LL);
#elif CASE == 3
    construct(-1);
#elif CASE == 4
    construct(__VERIFIER_nondet_int());
#elif CASE == 18
    if (__VERIFIER_nondet_int()) construct(1);
#elif CASE == 25
    forward(1);
#else
    construct(1);
#endif
#if CASE == 24
    construct(2);
#endif
#if CASE == 7
    phase = 2;
#elif CASE == 13
    phase = 0;
#elif CASE != 6
    phase = 1;
#endif
  }
  give();
#if CASE == 5
  construct(1);
#elif CASE == 12
  escaped = &result;
#elif CASE == 22
  phase = 0;
#endif
#if CASE == 11
  __VERIFIER_atomic_change(&payload, 0, 2, &shared_output);
#else
  __VERIFIER_atomic_change(&payload, 0, 2, &result);
#endif
#if CASE == 10
  __VERIFIER_atomic_change(&other_payload, 0, 2, &result);
#elif CASE == 14
  payload++;
#elif CASE == 16
  extern void unknown(volatile datum*);
  unknown(&payload);
#elif CASE == 20
  construct(2);
#elif CASE == 30
  __VERIFIER_atomic_change((volatile datum*)&phase, 0, 0, &result);
#endif
  return 0;
}
#if CASE == 15 && !defined(NATIVE)
static void* foreign(void* ignored) { give(); return 0; }
#endif
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, worker, 0);
  pthread_create(&b, 0, worker, 0);
#if CASE == 15 && !defined(NATIVE)
  pthread_t c;
  pthread_create(&c, 0, foreign, 0);
#endif
  return 0;
}
