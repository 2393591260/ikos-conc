#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef CASE
#define CASE 0
#endif
#ifndef A_COUNT
#define A_COUNT 2
#endif
#ifndef B_COUNT
#define B_COUNT 2
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif

volatile datum payload[16];
volatile datum auxiliary_value;
volatile datum *expired;
pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;

__attribute__((noinline)) int emit_write(volatile datum *p, int value) {
  *p = value;
  return value + 3;
}
__attribute__((noinline)) datum emit_read(volatile datum *p) {
  return *p;
}
__attribute__((noinline)) int __VERIFIER_atomic_emit(volatile datum *p, int value) {
  *p = value;
  return value + 3;
}
__attribute__((noinline)) volatile datum *expired_object(void) {
  volatile datum local = 0;
  return &local;
}
void *auxiliary(void *unused) {
  (void)unused;
  auxiliary_value = 4;
  return 0;
}

#if CASE == 10 || CASE == 11
extern int __VERIFIER_nondet_int(void);
void *worker_left(void *argument) {
#if CASE == 10
  (void)argument;
  payload[0] = 7;
#else
  volatile datum *p = argument;
  *p = 7;
#endif
  return 0;
}
#elif CASE == 12
void *worker_same(void *argument) {
  uintptr_t start = (uintptr_t)argument;
  for (int i = 0; i < 3; ++i) payload[start + i] = i + 1;
  return 0;
}
#else
void *worker_left(void *unused) {
  (void)unused;
  for (int i = 0; i < A_COUNT; ++i) {
    volatile datum *p = i + 1 == A_COUNT ? &payload[0] : &payload[1];
#if CASE == 15
    p = i + 1 == A_COUNT ? expired : &payload[1];
#endif
#if CASE == 2
    if (i + 1 == A_COUNT) return 0;
#endif
#if CASE == 1
    pthread_mutex_lock(&guard);
#endif
#if CASE == 4
    if (emit_write(p, i + 1) != i + 4) abort();
#elif CASE == 5 || CASE == 6 || CASE == 9
    if (__VERIFIER_atomic_emit(p, i + 1) != i + 4) abort();
#elif CASE == 13
    (void)*p;
#else
    *p = i + 1;
#endif
#if CASE == 1
    pthread_mutex_unlock(&guard);
#endif
  }
#if CASE == 6
  if (payload[0] != 99) abort();
#endif
  return 0;
}
void *worker_right(void *unused) {
  (void)unused;
  for (int i = 0; i < B_COUNT; ++i) {
    volatile datum *p = i + 1 == B_COUNT ? &payload[0] : &payload[2];
#if CASE == 3
    p = i + 1 == B_COUNT ? &payload[3] : &payload[2];
#elif CASE == 15
    p = i + 1 == B_COUNT ? expired : &payload[2];
#endif
#if CASE == 1
    pthread_mutex_lock(&guard);
#endif
#if CASE == 4
    (void)emit_read(p);
#else
    (void)*p;
#endif
#if CASE == 1
    pthread_mutex_unlock(&guard);
#endif
  }
  return 0;
}
#endif

int main(void) {
  pthread_t left, right, extra;
#if CASE == 10 || CASE == 11
  int count = __VERIFIER_nondet_int();
  if (count < 3 || count > 4) return 0;
#if CASE == 10
  pthread_t *handles = malloc((size_t)count * sizeof(pthread_t));
  if (!handles) return 0;
  pthread_create(&handles[0], 0, worker_left, 0);
  for (int i = 0; i < 3; ++i) {
    volatile datum *p = i == 2 ? &payload[0] : &payload[2];
    (void)*p;
  }
  pthread_join(handles[0], 0);
  free(handles);
#else
  datum *memory = malloc((size_t)count * sizeof(datum));
  if (!memory) return 0;
  for (int i = 0; i < count; ++i) memory[i] = 0;
  pthread_create(&left, 0, worker_left, &memory[2]);
#if CASE == 11
  free(memory);
#endif
  for (int i = 0; i < 3; ++i) (void)*(volatile datum *)&memory[i];
  pthread_join(left, 0);
#endif
#elif CASE == 12
  pthread_create(&left, 0, worker_same, (void *)(uintptr_t)1);
  pthread_create(&right, 0, worker_same, (void *)(uintptr_t)9);
  pthread_create(&extra, 0, worker_same, (void *)(uintptr_t)3);
  pthread_join(left, 0);
  pthread_join(right, 0);
  pthread_join(extra, 0);
#elif CASE == 7 || CASE == 8 || CASE == 9
#if CASE == 8 || CASE == 9
  pthread_create(&right, 0, auxiliary, 0);
#endif
  pthread_create(&left, 0, worker_left, 0);
  for (int i = 0; i < B_COUNT; ++i) {
#if CASE == 8 || CASE == 9
    if (i == 1) pthread_join(right, 0);
#endif
    volatile datum *p = i + 1 == B_COUNT ? &payload[0] : &payload[2];
    (void)*p;
  }
  pthread_join(left, 0);
#else
#if CASE == 14
  payload[0] = A_COUNT;
  payload[0] = A_COUNT;
#elif CASE == 15
  expired = expired_object();
#endif
  pthread_create(&left, 0, worker_left, 0);
  pthread_create(&right, 0, worker_right, 0);
  pthread_join(left, 0);
  pthread_join(right, 0);
#endif
  return 0;
}
