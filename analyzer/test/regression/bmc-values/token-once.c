#include <pthread.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
#if CASE == 1
#define payload renamed_payload
#define state renamed_once_state
#define pass renamed_gate_helper
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
#ifdef NATIVE
pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
#define TAKE() pthread_mutex_lock(&gate)
#define PUT() pthread_mutex_unlock(&gate)
#else
int gate;
static void require(int ok) { if (!ok) abort(); }
static void __VERIFIER_atomic_take(int* p) { require(*p == 0); *p = 1; }
static void __VERIFIER_atomic_put(int* p) { require(*p == 1); *p = 0; }
#define TAKE() __VERIFIER_atomic_take(&gate)
#define PUT() __VERIFIER_atomic_put(&gate)
#endif
pthread_mutex_t data_lock = PTHREAD_MUTEX_INITIALIZER;
int state, shared_result, unrelated;
datum payload, other_payload;
int* escaped;
extern int __VERIFIER_nondet_int(void);
extern void opaque(void*);
#if CASE == 2 || CASE == 15
#define UPDATE renamed_plain_update
#else
#define UPDATE __VERIFIER_atomic_renamed_update
#endif
static void UPDATE(datum* p, datum e, datum u, int* r) {
#if CASE == 13
  escaped = (int*)p;
#elif CASE == 14
  opaque(p);
#endif
#if CASE == 11
  ((char*)p)[0] = 1;
#else
  if (*p == e) {
#if CASE == 12
    ((char*)r)[0] = 1;
#else
    *p = u; *r = 1;
#endif
  } else *r = 0;
#endif
}
static void initialize(void) {
#if CASE == 18
  if (__VERIFIER_nondet_int()) payload = 7;
#else
  payload = 7;
#endif
}
static void pass(void) {
  TAKE();
  switch (state) {
    case 0:
#if CASE == 19
      state = 1; PUT(); initialize(); return;
#else
      initialize(); state = 1;
#endif
    case 1:
      PUT(); break;
  }
}
static void later(void) {
  int result = -1;
#if CASE == 5
  int second_result = -1;
#endif
#if CASE == 2
  pthread_mutex_lock(&data_lock);
#endif
#if CASE == 3
  for (int i = 0; i < 2; ++i) {
#endif
#if CASE == 6
  UPDATE(&payload, 7, 8, (int*)&payload);
#elif CASE == 7
  UPDATE(&payload, 7, 8, &state);
#elif CASE == 8
  UPDATE(&payload, 7, 8, (int*)&gate);
#elif CASE == 9
  UPDATE(&payload, 7, 8, &shared_result);
#else
  UPDATE(&payload, 7, 8, &result);
#endif
#if CASE == 3
  }
#endif
#if CASE == 5
  UPDATE(&other_payload, 7, 8, &second_result);
#elif CASE == 10
  opaque(&result);
#elif CASE == 16
  escaped = &result;
#elif CASE == 17
  UPDATE(&payload, 8, 9, &result);
#endif
#if CASE == 2
  pthread_mutex_unlock(&data_lock);
#endif
  if (result == 3) abort();
}
static void* thread(void* ignored) {
#if CASE == 4
  later();
#elif CASE == 21
  if (__VERIFIER_nondet_int()) later();
#elif CASE == 22
  state = 0;
#elif CASE == 24
  unrelated++; /* Keep a reachable conflict before the failed release aborts. */
  PUT();
#elif CASE == 25
  ((char*)&gate)[0] = 0;
#endif
  pass();
#if CASE == 20
  initialize();
#elif CASE == 23
  unrelated++;
#endif
  later(); return 0;
}
#if CASE == 26
static void* second(void* ignored) { later(); return 0; }
#endif
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, thread, 0);
#if CASE == 26
  pthread_create(&b, 0, second, 0);
#else
  pthread_create(&b, 0, thread, 0);
#endif
  return 0;
}
