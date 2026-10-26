#if __SIZEOF_POINTER__ == 4
#define READONLY_NON_C __attribute__((fastcall))
#else
#define READONLY_NON_C __attribute__((ms_abi))
#endif
// This file is part of the SV-Benchmarks collection of verification tasks:
// https://gitlab.com/sosy-lab/benchmarking/sv-benchmarks
//
// SPDX-FileCopyrightText: 2021 F. Schuessele <schuessf@informatik.uni-freiburg.de>
// SPDX-FileCopyrightText: 2021 D. Klumpp <klumpp@informatik.uni-freiburg.de>
//
// SPDX-License-Identifier: LicenseRef-BSD-3-Clause-Attribution-Vandikas

typedef unsigned long int pthread_t;

union pthread_attr_t
{
  char __size[36];
  long int __align;
};
typedef union pthread_attr_t pthread_attr_t;

extern void __assert_fail(const char *__assertion, const char *__file,
      unsigned int __line, const char *__function)
     __attribute__ ((__nothrow__ , __leaf__)) __attribute__ ((__noreturn__));
void reach_error() { __assert_fail("0", "popl20-prod-cons-eq.wvr.c", 21, __extension__ __PRETTY_FUNCTION__); }
extern int pthread_create (pthread_t *__restrict __newthread,
      const pthread_attr_t *__restrict __attr,
      void *(*__start_routine) (void *),
      void *__restrict __arg) __attribute__ ((__nothrow__)) __attribute__ ((__nonnull__ (1, 3)));
extern int pthread_join (pthread_t __th, void **__thread_return);

typedef __SIZE_TYPE__ size_t;
extern void *malloc (size_t __size) __attribute__ ((__nothrow__ , __leaf__)) __attribute__ ((__malloc__)) ;

extern int  __VERIFIER_nondet_int(void);
#if CASE == 15
extern _Bool READONLY_NON_C __VERIFIER_nondet_bool(void);
#elif CASE == 13
extern int __VERIFIER_nondet_bool(void);
#else
extern _Bool __VERIFIER_nondet_bool(void);
#endif
extern void __VERIFIER_atomic_begin(void);
extern void __VERIFIER_atomic_end(void);

extern void abort(void);
void assume_abort_if_not(int cond) {
  if(!cond) {abort();}
}

#ifndef CASE
#define CASE 0
#endif
#if CASE == 23
#define thread1 supply_step
#define thread2 consume_step
#define thread3 compare_step
#define state11 supply_cursor
#define state12 first_result
#define state21 compare_cursor
#define state22 second_result
#define front1 head_cursor
#define size1 queued_count
#define consume outputs
#define produce messages
#endif
#ifdef WIDE
typedef long long datum;
extern long long __VERIFIER_nondet_longlong(void);
#define row_arbitrary __VERIFIER_nondet_longlong
#else
typedef int datum;
#define row_arbitrary __VERIFIER_nondet_int
#endif
int *produce, *update, *queue1;
datum **consume;
int unrelated;
extern void opaque_effect(void);
#if CASE == 11
_Bool __VERIFIER_nondet_bool(void) { unrelated++; return 0; }
#endif
#if CASE == 14
void __VERIFIER_atomic_begin(void) { unrelated++; }
#endif
_Bool *done;
int front1, size1, state11, state12, state21, state22;
int n_queue, n_produce, n_update, n1, n2;
_Bool finished1, finished2;

int *create_fresh_int_array(int size);
datum *create_fresh_row(int size);
_Bool *create_fresh_bool_array(int size);

void* thread1(void* _argptr) {
  while (!finished1) {
#if CASE == 2
    state21 = 0;
#endif
#if CASE != 5
    __VERIFIER_atomic_begin();
#endif
#if CASE == 20
    int saved = front1 + size1;
    __VERIFIER_atomic_end();
    __VERIFIER_atomic_begin();
    assume_abort_if_not(saved >= 0 && saved < n_queue);
#else
    assume_abort_if_not(front1+size1 >= 0 && front1+size1 < n_queue);
#if CASE == 21
    __VERIFIER_atomic_end();
    __VERIFIER_atomic_begin();
#endif
#endif
    assume_abort_if_not(state11 >= 0 && state11 < n_produce);
    assume_abort_if_not(queue1[
#if CASE == 20
      saved
#elif CASE == 7
      front1 + size1 + 1
#else
      front1 + size1
#endif
    ] == produce[state11]);
    size1++;
#if CASE == 19
    __VERIFIER_atomic_begin();
#endif
#if CASE != 18 && CASE != 5
    __VERIFIER_atomic_end();
#endif
    assume_abort_if_not(state11 >= 0 && state11 < n_update);
    state11 = update[state11];
    assume_abort_if_not(state11 >= 0 && state11 < n_update);
    __VERIFIER_atomic_begin();
    finished1 = done[state11];
    __VERIFIER_atomic_end();
  }

#if CASE == 22
  unrelated = 1;
#endif
  return 0;
}

void* thread2(void* _argptr) {
  __VERIFIER_atomic_begin();
  _Bool cond = !finished1 || size1 > 0;
  __VERIFIER_atomic_end();
  while (cond) {
    __VERIFIER_atomic_begin();
    assume_abort_if_not(size1 > 0);
    assume_abort_if_not(state12 >= 0 && state12 < n1);
    assume_abort_if_not(front1 >= 0 && front1 < n_queue);
    assume_abort_if_not(queue1[front1] >= 0 && queue1[front1] < n2);
    state12 = consume[state12][queue1[front1]];
    front1++;
#if CASE == 26
    state21++;
#endif
    size1--;
    __VERIFIER_atomic_end();
    __VERIFIER_atomic_begin();
    cond = !finished1 || size1 > 0;
    __VERIFIER_atomic_end();
  }

  return 0;
}

void* thread3(void* _argptr) {
#if CASE == 25 || CASE == 26
#if CASE == 26
  __VERIFIER_atomic_begin();
#endif
  state21 = 0;
  for (int step = 0; step < n_produce; step++) {
    state22 = produce[state21];
    state21 += 2;
  }
#if CASE == 26
  __VERIFIER_atomic_end();
#endif
  return 0;
#else
#if CASE == 6
  assume_abort_if_not(state21 >= 0 && state21 < n_produce);
#endif
#if CASE == 16
  int *moving = produce;
#endif
  while (!finished2) {
    assume_abort_if_not(state22 >= 0 && state22 < n1);
#if CASE != 6
    assume_abort_if_not(state21 >= 0 && state21 < n_produce);
#endif
#if CASE == 27
    if (state21 < 0) {
      int *destination;
      if (__VERIFIER_nondet_int()) destination = &unrelated;
      else destination = &state11;
      *destination = 1;
    }
#elif CASE == 28
    if (state21 < 0) opaque_effect();
#endif
#if CASE == 12
    assume_abort_if_not(((volatile int *)produce)[state21] >= 0 && produce[state21] < n2);
#else
    assume_abort_if_not(produce[state21] >= 0 && produce[state21] < n2);
#endif
    state22 = consume[state22][produce[state21]];
    assume_abort_if_not(state21 >= 0 && state21 < n_update);
    state21 = update[state21];
    assume_abort_if_not(state21 >= 0 && state21 < n_update);
    finished2 = done[state21];
#if CASE == 3
    front1++;
#elif CASE == 4
    size1++;
#elif CASE == 16
    state21 += *moving++;
#elif CASE == 24
    produce[state21] = 0;
#endif
  }

#if CASE == 22
  unrelated = 2;
#endif
  return 0;
#endif
}

int main() {
  pthread_t t1, t2, t3, t4;

  front1 = __VERIFIER_nondet_int();
  state11 = __VERIFIER_nondet_int();
  state21 = state11;
  state12 = __VERIFIER_nondet_int();
  state22 = state12;

  n_queue = __VERIFIER_nondet_int();
  n_produce = __VERIFIER_nondet_int();
  n_update = __VERIFIER_nondet_int();
  n1 = __VERIFIER_nondet_int();
  n2 = __VERIFIER_nondet_int();

  produce = create_fresh_int_array(n_produce);
  update = create_fresh_int_array(n_update);
  queue1 = create_fresh_int_array(n_queue);
  done = create_fresh_bool_array(n_update);
  assume_abort_if_not(n1 >= 0);
  assume_abort_if_not(n1 <= (((size_t) 4294967295) / sizeof(int*)));
  consume = (datum**)malloc(sizeof(datum*) * (size_t)n1);
  for (int i=0; i<n1; i++) {
    consume[i] = create_fresh_row(n2);
  }

  // main method
  pthread_create(&t1, 0, thread1, 0);
  pthread_create(&t2, 0, thread2, 0);
  pthread_create(&t3, 0, thread3, 0);
#if CASE == 1
  pthread_create(&t4, 0, thread3, 0);
#endif
#if CASE == 9
  assume_abort_if_not(state12 != state22);
#endif
#if CASE == 10
  state21 = 0;
#endif
  pthread_join(t1, 0);
  pthread_join(t2, 0);
#if CASE != 8
  pthread_join(t3, 0);
#endif

  assume_abort_if_not(state12 != state22);
  reach_error();

  return 0;
}

int *create_fresh_int_array(int size) {
  assume_abort_if_not(size >= 0);
  assume_abort_if_not(size <= (((size_t) 4294967295) / sizeof(int)));

  int* arr = (int*)malloc(sizeof(int) * (size_t)size);
  for (int i = 0; i < size; i++) {
    arr[i] = __VERIFIER_nondet_int();
  }
  return arr;
}

_Bool *create_fresh_bool_array(int size) {
  assume_abort_if_not(size >= 0);
  assume_abort_if_not(size <= (((size_t) 4294967295) / sizeof(_Bool)));

  _Bool* arr = (_Bool*)malloc(sizeof(_Bool) * (size_t)size);
  for (int i = 0; i < size; i++) {
    arr[i] = __VERIFIER_nondet_bool();
  }
  return arr;
}

datum *create_fresh_row(int size) {
  assume_abort_if_not(size >= 0);
  assume_abort_if_not((unsigned)size <= 4294967295u / sizeof(datum));
  datum *row = malloc(sizeof(datum) * (unsigned)size);
  for (int i = 0; i < size; i++) row[i] = row_arbitrary();
  return row;
}
