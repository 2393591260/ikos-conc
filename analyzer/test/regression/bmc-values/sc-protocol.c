#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
#ifndef WIDE
#define WIDE 0
#endif
#if WIDE
typedef long long scalar;
#else
typedef int scalar;
#endif
#if CASE == 4
#define ORDER memory_order_relaxed
#else
#define ORDER memory_order_seq_cst
#endif
#if CASE == 26
static _Atomic scalar f = -1, g, turn;
#else
static _Atomic scalar f, g, turn;
#endif
static scalar payload;
extern void unknown_effect(void);
static void nothing(int n) { if (n) abort(); }
#if CASE == 21
static void hidden(void) { atomic_store_explicit(&f, 1, ORDER); }
#endif
static void *left(void *ignored) {
#if CASE == 9
  unknown_effect();
#endif
#if CASE == 21
  hidden();
#endif
#if CASE == 11
  atomic_fetch_add_explicit(&turn, 0, ORDER);
#endif
#if CASE == 12
  atomic_thread_fence(ORDER);
#endif
#if CASE == 13
  *(scalar *)&f = 1;
#endif
#if CASE == 14
  scalar *p = malloc(sizeof(*p)); *p = 1; free(p);
#endif
#if CASE == 1 || CASE == 2 || CASE == 8 || CASE == 26
  payload = 1;
#if CASE == 26
  atomic_store_explicit(&f, -7, ORDER);
#elif CASE == 2
  atomic_store_explicit(&f, 0, ORDER);
#else
  atomic_store_explicit(&f, 1, ORDER);
#endif
#else
#if CASE == 5
  atomic_store_explicit(&turn, 1, ORDER);
  atomic_store_explicit(&f, 1, ORDER);
#else
  atomic_store_explicit(&f, 1, ORDER);
  #if CASE != 16
  atomic_store_explicit(&turn, 1, ORDER);
  #endif
#endif
#if CASE == 3
  while (atomic_load_explicit(&g, ORDER) && 0) {}
#elif CASE == 16
  while (atomic_load_explicit(&g, ORDER)) {
    if (atomic_load_explicit(&turn, ORDER) != 0) {
      atomic_store_explicit(&f, 0, ORDER);
      while (atomic_load_explicit(&turn, ORDER) != 0) {}
      atomic_store_explicit(&f, 1, ORDER);
    }
  }
#else
  while (atomic_load_explicit(&g, ORDER) && atomic_load_explicit(&turn, ORDER) == 1) {}
#endif
#if CASE == 6
  for (int i = 0; i < 2; ++i)
#endif
#if CASE == 19
    atomic_store_explicit(&f, 0, ORDER);
#endif
    payload = 1;
#if CASE == 10
  nothing((int)payload);
#endif
#if CASE == 16
  atomic_store_explicit(&turn, 1, ORDER);
#endif
  atomic_store_explicit(&f, 0, ORDER);
#endif
  return 0;
}
static void *right(void *ignored) {
#if CASE == 1 || CASE == 2 || CASE == 8 || CASE == 26
#if CASE == 26
  while (atomic_load_explicit(&f, ORDER) != -7) {}
#elif CASE != 8
  while (!atomic_load_explicit(&f, ORDER)) {}
#endif
  payload = 2;
#else
  atomic_store_explicit(&g, 1, ORDER);
#if CASE == 7
  atomic_store_explicit(&turn, 1, ORDER);
#else
  #if CASE != 16
  atomic_store_explicit(&turn, 0, ORDER);
  #endif
#endif
#if CASE == 16
  while (atomic_load_explicit(&f, ORDER)) {
    if (atomic_load_explicit(&turn, ORDER) != 1) {
      atomic_store_explicit(&g, 0, ORDER);
      while (atomic_load_explicit(&turn, ORDER) != 1) {}
      atomic_store_explicit(&g, 1, ORDER);
    }
  }
#else
#if CASE == 20
  while (atomic_load_explicit(&f, memory_order_acquire) && atomic_load_explicit(&turn, ORDER) == 0) {}
#else
  while (atomic_load_explicit(&f, ORDER) && atomic_load_explicit(&turn, ORDER) == 0) {}
#endif
#endif
  payload = 2;
#if CASE == 16
  atomic_store_explicit(&turn, 0, ORDER);
#endif
  atomic_store_explicit(&g, 0, ORDER);
#endif
  return 0;
}
int main(void) {
  pthread_t a, b;
#if CASE == 23
  atomic_store_explicit(&turn, 0, ORDER);
#endif
#if CASE == 22
  for (int i = 0; i < 2; ++i)
#endif
#if CASE == 24
  pthread_create(&a, 0, left, (void *)1);
#else
  pthread_create(&a, 0, left, 0);
#endif
#if CASE == 18
  atomic_store_explicit(&f, 0, ORDER);
#endif
#if CASE == 25
  left(0);
#endif
#if CASE == 15
  pthread_create(&b, 0, left, 0);
#else
  pthread_create(&b, 0, right, 0);
#endif
#if CASE == 17
  pthread_create(&b, 0, right, 0);
#endif
#if CASE == 28
  a = 0;
#endif
#if CASE == 30
  nothing((int)a);
#endif
#if CASE == 31
  *(char *)&a = 0;
#endif
#if CASE == 27
  pthread_join((pthread_t)0, 0);
#elif CASE == 29
  void *result;
  pthread_join(a, &result);
#else
  pthread_join(a, 0);
#endif
  pthread_join(b, 0);
  return 0;
}
