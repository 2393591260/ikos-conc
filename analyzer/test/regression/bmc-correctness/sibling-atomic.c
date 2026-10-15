// SPDX-License-Identifier: MIT
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
extern int __VERIFIER_nondet_int(void);
extern void __VERIFIER_assume(int);
extern void __VERIFIER_atomic_begin(void);
extern void __VERIFIER_atomic_end(void);
extern void opaque(void);
#ifdef WIDE
volatile long long payload;
#else
volatile int payload;
#endif
volatile int before, after, ready;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
void update(void) { before = 1; payload = 7; after = 1; }
void __VERIFIER_atomic_update(void) { update(); }
void write_payload(void) {
#if CASE == 10
  __VERIFIER_atomic_begin(); update(); __VERIFIER_atomic_end();
#else
  __VERIFIER_atomic_update();
#endif
}
void publish(void) {
  pthread_mutex_lock(&mutex); ready = 1; pthread_mutex_unlock(&mutex);
}
void acquire(void) {
  pthread_mutex_lock(&mutex); __VERIFIER_assume(ready == 1); pthread_mutex_unlock(&mutex);
}
void (*hook)(void) = publish;
void *writer(void *unused) {
  write_payload();
#if CASE == 7
  publish();
#elif CASE == 14
  hook();
#elif CASE == 8
  puts("publisher");
  __VERIFIER_atomic_begin(); ready = 1; __VERIFIER_atomic_end();
#endif
  return 0;
}
void *reader(void *unused) {
#if CASE == 7 || CASE == 14
  acquire();
#elif CASE == 8
  __VERIFIER_atomic_begin(); __VERIFIER_assume(ready == 1); __VERIFIER_atomic_end();
  puts("reader");
#endif
  (void)payload;
  return 0;
}
void *worker(void *unused) {
#if CASE == 2
  pthread_mutex_lock(&mutex);
#elif CASE == 9
  opaque();
#elif CASE == 13
  pthread_cond_signal(&condition);
#endif
  if (__VERIFIER_nondet_int()) {
    write_payload();
#if CASE == 3
    __VERIFIER_assume(0);
#endif
  } else {
#if CASE == 1
    __VERIFIER_atomic_begin();
#endif
    (void)payload;
#if CASE == 1
    __VERIFIER_atomic_end();
#elif CASE == 4
    __VERIFIER_assume(0);
#endif
  }
#if CASE == 2
  pthread_mutex_unlock(&mutex);
#elif CASE == 5
  while (1) {}
#elif CASE == 12
  pthread_exit(0);
#endif
  return 0;
}
int main(void) {
  pthread_t a, b;
#if CASE == 7 || CASE == 8 || CASE == 14
  pthread_create(&a, 0, writer, 0); pthread_create(&b, 0, reader, 0);
#else
  pthread_create(&a, 0, worker, 0);
#if CASE == 6
  pthread_join(a, 0);
#endif
  pthread_create(&b, 0, worker,
#if CASE == 11
                 (void *)7
#else
                 0
#endif
  );
#endif
  return 0;
}
