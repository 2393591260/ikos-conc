// Guard precision requires an independent ALL-access mutex proof.
#include <pthread.h>
#include <stdlib.h>

#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long scalar;
#else
typedef int scalar;
#endif
#if CASE == 2
static scalar first = 1;
#else
static scalar first;
#endif
static scalar active;
static scalar payload;
static pthread_mutex_t outer, inner;
#if CASE == 25
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
  (void)m; (void)a; return 0;
}
#endif
#if CASE == 26
int pthread_mutex_lock(pthread_mutex_t *m) { (void)m; return 0; }
#endif
#if CASE == 27
extern void opaque_effect(void);
#endif
__attribute__((noinline)) static int scalar_helper(int value) {
#if CASE == 24
  pthread_mutex_unlock(&inner);
#endif
  if (!value) abort();
  return value;
}
static void *producer(void *arg) {
  (void)arg;
  pthread_mutex_lock(&outer);
#if CASE == 32
  pthread_mutex_lock(&outer);
#endif
  if (first == 0) {
#if CASE == 5
    payload = 1;
#endif
#if CASE == 44
    pthread_mutex_lock(&outer);
#elif CASE != 3
    pthread_mutex_lock(&inner);
#endif
#if CASE == 7
    pthread_mutex_unlock(&inner);
#endif
#if CASE == 8
    *(unsigned char *)&active = 1;
#elif CASE == 38
    active = 4294967297LL;
#elif CASE == 40
    active = scalar_helper(1);
#elif CASE != 6
    active = 1;
#endif
#if CASE == 44
    pthread_mutex_unlock(&outer);
#elif CASE != 3 && CASE != 7
    pthread_mutex_unlock(&inner);
#endif
    first = 1;
  }
#if CASE == 4
  pthread_mutex_lock(&inner);
  active = 0;
  pthread_mutex_unlock(&inner);
#endif
#if CASE == 9
  if (active == 0) payload = 2;
#endif
#if CASE == 24
  scalar_helper(1);
#endif
#if CASE == 27
  opaque_effect();
#endif
#if CASE == 33
  pthread_mutex_unlock(&inner);
#endif
  payload = 1;
#if CASE == 31
  return 0;
#endif
#if CASE == 30
  if (payload != 1) abort();
#else
  scalar_helper(payload == 1);
#endif
  pthread_mutex_unlock(&outer);
  return 0;
}
static void *consumer(void *arg) {
  (void)arg;

#if CASE == 41
  for (int count = 0; count < 2; ++count) {
#endif
  pthread_mutex_lock(&inner);
#if CASE == 10
  scalar snapshot = active;
  pthread_mutex_unlock(&inner);
  if (snapshot == 0) {
#elif CASE == 1
  if (first == 0) {
#elif CASE == 38
  if (active != 4294967297LL) {
#else
  if (active == 0) {
#endif
    payload = 0;
#if CASE == 30
    if (payload != 0) abort();
#else
    scalar_helper(payload == 0);
#endif
  }
#if CASE != 10
  pthread_mutex_unlock(&inner);
#endif
#if CASE == 41
  }
#endif
  return 0;
}
int main(void) {
  pthread_t a, b;

#if CASE != 2
  scalar_helper(first == active);
#endif
#if CASE == 28
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutex_init(&outer, &attr);
#else
  pthread_mutex_init(&outer, 0);
#endif
#if CASE != 12
  pthread_mutex_init(&inner, 0);
#endif
#if CASE == 37
  pthread_create(&b, 0, consumer, 0);
  pthread_create(&a, 0, producer, 0);
#else
  pthread_create(&a, 0, producer, 0);
  pthread_create(&b, 0, consumer, 0);
#endif
#if CASE == 11
  pthread_mutex_destroy(&inner);
#endif
#if CASE == 12
  pthread_mutex_init(&inner, 0);
#endif
#if CASE == 13
  active = 0;
#endif
#if CASE == 14
  scalar_helper(active == 0);
#endif
#if CASE == 15
  pthread_mutex_init(&inner, 0);
#endif
#if CASE == 16
  a = b;
#endif
#if CASE == 17
  pthread_detach(a);
#endif
#if CASE != 18
  pthread_join(a, 0);
#endif
#if CASE == 19
  pthread_join(a, 0);
#else
  pthread_join(b, 0);
#endif
#if CASE == 20
  pthread_create(&a, 0, producer, 0);
#endif
#if CASE == 21
  *(unsigned char *)&inner = 0;
#endif
#if CASE == 42
  scalar_helper(active == first);
#endif
  pthread_mutex_destroy(&outer);
#if CASE != 11
  pthread_mutex_destroy(&inner);
#endif
#if CASE == 22
  pthread_mutex_lock(&inner);
  pthread_mutex_unlock(&inner);
#endif
#if CASE == 23
  pthread_join(b, 0);
#endif
#if CASE == 35
  __asm__ volatile("" ::: "memory");
#endif
  return 0;
}
