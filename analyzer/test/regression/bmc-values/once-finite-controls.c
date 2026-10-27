/* Finite once-control regression variants, selected by CASE.
 * Compile through test_bmc_once_physical.py for the selected ABI.
 */
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#ifndef CASE
#define CASE 0
#endif
extern int __VERIFIER_nondet_int(void);
extern void escape_pointer(void *);
static pthread_once_t left = PTHREAD_ONCE_INIT;
static pthread_once_t right = PTHREAD_ONCE_INIT;
#if CASE == 21
static pthread_once_t third = PTHREAD_ONCE_INIT;
#endif
#if CASE == 1 || CASE == 10 || CASE == 23
static pthread_once_t *selected = &left;
#elif CASE == 22
static pthread_once_t *selected = &right;
#endif
#if CASE == 19
static pthread_once_t * volatile volatile_selected = &left;
#endif
#if CASE == 9
static unsigned long long wide_control;
#endif
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int datum;
static void initialize(void) {
#if CASE == 3
  pthread_mutex_lock(&lock);
#endif
  datum++;
#if CASE == 3
  pthread_mutex_unlock(&lock);
#endif
}
#if CASE == 5
static pthread_once_t *choose_helper(int branch) {
  return branch ? &right : &left;
}
#endif
#if CASE == 20
static void (*callback_slot)(void) = initialize;
#endif
static void *child(void *argument) {
  (void)argument;
#if CASE == 11
  initialize();
#else
  pthread_once(&left, initialize);
#endif
#if CASE == 0
  pthread_mutex_lock(&lock);
  datum = 10;
  pthread_mutex_unlock(&lock);
#elif CASE == 10
  selected = &right; /* SLOT-WRITER VIOLATION: not a main-only slot. */
#endif
  return 0;
}
int main(void) {
  pthread_t tid;
  int branch = __VERIFIER_nondet_int();
  pthread_once_t *actual = &left;
#if CASE == 6
  left = PTHREAD_ONCE_INIT; /* Raw control access outside static initialization. */
#elif CASE == 15
  memset(&left, 0, sizeof(left)); /* Byte effect must invalidate seal. */
#endif
  pthread_create(&tid, 0, child, 0);
#if CASE == 1
  if (branch) selected = &right;
  actual = selected;
#elif CASE == 2 || CASE == 13
  actual = branch ? &left : &left;
#elif CASE == 3
  actual = branch ? &left : &right;
#elif CASE == 4
  if (branch && !branch) actual = &right;
#elif CASE == 5
  actual = choose_helper(branch); /* Initial narrow profile excludes helper returns. */
#elif CASE == 7
  volatile uintptr_t bits = (uintptr_t)(branch ? &left : &right);
  actual = (pthread_once_t *)bits; /* Keep the real ptr-int origin in LLVM. */
#elif CASE == 8
  if (branch) actual = &right;
  escape_pointer(&actual); /* Slot address escapes to unknown effects. */
#elif CASE == 9
  actual = (pthread_once_t *)&wide_control; /* Wrong typed/static object. */
#elif CASE == 10
  actual = selected;
#elif CASE == 12
  for (int i = 0; i < 2; ++i) {
    actual = i ? &left : &right;
    /* right first may race with child left; later left does not erase race. */
    pthread_once(actual, initialize); /* One static callsite, TWO control IDs. */
  }
#elif CASE == 14
  actual = &right + 1; /* Out-of-object address is never a control certificate. */
#elif CASE == 17
  actual = branch ? &right : 0; /* Null remains an uncertified MAY origin. */
#elif CASE == 18
  {
    pthread_once_t *slot0 = branch ? &right : &left;
    pthread_once_t *slot1 = slot0;
    actual = slot1;
  }
#elif CASE == 19
  if (branch) volatile_selected = &right;
  actual = volatile_selected;
#elif CASE == 21
  actual = branch ? &right : &third;
#elif CASE == 22 || CASE == 23
  actual = selected; /* Exact static initializer, no runtime pointer-slot Store. */
#else
  if (branch) actual = &right;
#endif
#if CASE == 16
  /* Same name, actual definition: use a separate tiny standalone source below. */
  escape_pointer(actual);
#elif CASE == 20
  pthread_once(actual, callback_slot); /* No new indirect-callback promise. */
#elif CASE != 12
  pthread_once(actual, initialize);
#endif
#if CASE == 0
  pthread_mutex_lock(&lock);
  datum = 11;
  pthread_mutex_unlock(&lock);
#endif
  pthread_join(tid, 0);
  return 0;
}
