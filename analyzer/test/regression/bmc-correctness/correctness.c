// Safety controls for confirmation, with genuine races beside them.
#include <pthread.h>
#include <stdlib.h>
#include <stdatomic.h>
extern int __ikos_nondet_int(void);
extern void __VERIFIER_assume(int);
extern void __VERIFIER_atomic_begin(void);
extern void __VERIFIER_atomic_end(void);
#ifndef CASE
#define CASE 0
#endif
int selector, datum;
_Atomic int atomic_flag_value;
void unused_sync(void) {__sync_fetch_and_add(&selector, 1);}
pthread_mutex_t left = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t right = PTHREAD_MUTEX_INITIALIZER;
pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
void gate_left(void) {if (selector != 0) abort();}
void gate_right(void) {if (selector != 1) abort();}
void nested_left(void) {gate_left();}
void nested_right(void) {gate_right();}
void assumed_left(void) {__VERIFIER_assume(selector == 0);}
void assumed_right(void) {__VERIFIER_assume(selector == 1);}
void self_block(void) {pthread_mutex_lock(&left); pthread_mutex_lock(&left);}
void constant_write(void) {datum = 1;}
int uninitialized_filter(void) {int value; return value;}
int constant_return(void) {return 5;}
void sink(int value) {}
int repeated_stack(int initialize) {volatile int value; if (initialize) value = 7; return value;}
void optional_return(void) {if (__ikos_nondet_int()) abort();}
static void* worker_a(void* ignored) {
#if CASE == 0
  gate_left(); datum = 1;
#elif CASE == 1
  assumed_left(); datum = 1;
#elif CASE == 2
  nested_left(); datum = 1;
#elif CASE == 3
  pthread_mutex_lock(&left); pthread_mutex_lock(&left); datum = 1;
#elif CASE == 4
  self_block(); datum = 1;
#elif CASE == 5
  int value = __ikos_nondet_int();
  if (value != 256) return 0;
  __VERIFIER_assume((unsigned char)value); datum = 1;
#elif CASE == 6 || CASE == 15 || CASE == 20
  constant_write();
#elif CASE == 7
  pthread_mutex_lock(&left); datum = 1; pthread_mutex_unlock(&left);
#elif CASE == 8
  optional_return(); datum = 1;
#elif CASE == 9
  pthread_rwlock_wrlock(&rw); datum = 1; pthread_rwlock_unlock(&rw);
#elif CASE == 10
  extern void opaque(void); opaque(); datum = 1;
#elif CASE == 11
  pthread_mutex_lock(&left); pthread_mutex_unlock(&left);
  pthread_mutex_lock(&left); datum = 1; pthread_mutex_unlock(&left);
#elif CASE == 12
  pthread_mutex_t* pointer = &left;
  pthread_mutex_lock(pointer); datum = 1; pthread_mutex_unlock(pointer);
#elif CASE == 13
  __VERIFIER_assume(0); datum = 1;
#elif CASE == 14
  __VERIFIER_assume(__ikos_nondet_int()); datum = 1;
#elif CASE == 16
  int uninitialized;
  if (uninitialized) datum = 1;
#elif CASE == 17
  if (uninitialized_filter()) datum = 1;
#elif CASE == 18
  __VERIFIER_atomic_begin(); datum = 1; __VERIFIER_atomic_end();
#elif CASE == 19
  __VERIFIER_atomic_begin(); __VERIFIER_atomic_end(); datum = 1;
#elif CASE == 21
  int value; datum = value;
#elif CASE == 22
  volatile int value; datum = value;
#elif CASE == 23
  datum = uninitialized_filter();
#elif CASE == 24
  int value; sink(value); datum = 1;
#elif CASE == 25 || CASE == 31 || CASE == 33
  datum = 1;
#elif CASE == 26
  datum = __ikos_nondet_int();
#elif CASE == 27
  volatile int value = 7; datum = value;
#elif CASE == 28
  datum = constant_return();
#elif CASE == 29
  (void)repeated_stack(1); datum = repeated_stack(0);
#elif CASE == 30
  unsigned char value; datum = value;
#elif CASE == 32
  sink(__ikos_nondet_int()); datum = 1;
#elif CASE == 34
  datum = 1;
#elif CASE == 35
  datum = 1; __sync_fetch_and_add(&selector, 1);
#elif CASE == 36
  __sync_fetch_and_add(&selector, 1); datum = 1;
#elif CASE == 37
  if (selector == 0) {__sync_fetch_and_add(&selector, 1); return 0;}
  datum = 1;
#elif CASE == 38
  if (atomic_load(&atomic_flag_value) == 0) datum = 1;
#elif CASE == 39
  datum = 1; (void)atomic_load(&atomic_flag_value);
#endif
  return 0;
}
static void* worker_b(void* ignored) {
#if CASE == 0
  gate_right();
#elif CASE == 1
  assumed_right();
#elif CASE == 2
  nested_right();
#elif CASE == 7 || CASE == 11
  pthread_mutex_lock(&right);
#elif CASE == 9
  pthread_rwlock_wrlock(&rw);
#elif CASE == 12
  pthread_mutex_lock(&left);
#endif
  datum = 2;
#if CASE == 7 || CASE == 11
  pthread_mutex_unlock(&right);
#elif CASE == 9
  pthread_rwlock_unlock(&rw);
#elif CASE == 12
  pthread_mutex_unlock(&left);
#endif
  return 0;
}
int main(void) {
  pthread_t a, b;
  selector = __ikos_nondet_int();
#if CASE == 25
  int value; datum = value;
#endif
#if CASE == 20
  __VERIFIER_atomic_begin();
  pthread_create(&a, 0, worker_a, 0);
  datum = 3;
  __VERIFIER_atomic_end(); pthread_join(a, 0);
#elif CASE == 15
  if (__ikos_nondet_int()) pthread_create(&a, 0, worker_a, 0);
  else pthread_create(&b, 0, worker_b, 0);
  datum = 3;
#else
#if CASE == 31
  pthread_create(0, 0, worker_a, 0);
#elif CASE == 33
  pthread_attr_t attribute;
  pthread_create(&a, &attribute, worker_a, 0);
#else
  pthread_create(&a, 0, worker_a, 0);
#endif
  pthread_create(&b, 0, worker_b, 0);
  pthread_join(a, 0); pthread_join(b, 0);
#endif
  return 0;
}
