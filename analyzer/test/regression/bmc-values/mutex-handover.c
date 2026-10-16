#include <pthread.h>
#include <assert.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
#ifndef WIDE
#define WIDE 0
#endif
#if WIDE
typedef long long datum;
#else
typedef int datum;
#endif
extern int __VERIFIER_nondet_int(void);
pthread_mutex_t first = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t second = PTHREAD_MUTEX_INITIALIZER;
#if CASE == 28
pthread_mutex_t third = PTHREAD_MUTEX_INITIALIZER;
#endif
datum value = 2;
static void take_inner(int ignored) { pthread_mutex_lock(&second); }
static void release_inner(int ignored) { pthread_mutex_unlock(&second); }
static void observe(int ignored) { assert(value == 2); }
#if CASE == 18
static void hidden(void) { value++; }
static void (*volatile dispatch)(void) = hidden;
#endif
static void* worker(void* unused) {
#if CASE == 12 || CASE == 13
  for (int i = 0; i < 2; ++i)
#endif
  {
    pthread_mutex_lock(&first);
#if CASE != 1
#if CASE == 6
    if (__VERIFIER_nondet_int()) {
#endif
#if CASE == 10
    take_inner(7);
#elif CASE == 13
    if (i == 0) pthread_mutex_lock(&second);
#else
    pthread_mutex_lock(&second);
#endif
#if CASE == 9
    pthread_mutex_lock(&first);
#endif
#if CASE == 13
    if (i == 0) pthread_mutex_unlock(&second);
#else
    value = 1;
#if CASE == 29
    release_inner(0);
#else
    pthread_mutex_unlock(&second);
#endif
#endif
#if CASE == 6
    }
#endif
#endif
#if CASE == 8
    pthread_mutex_unlock(&second);
#elif CASE == 4
    pthread_mutex_unlock(&first);
    pthread_mutex_lock(&first);
#elif CASE == 17
    extern void unknown(void);
    unknown();
#elif CASE == 18
    dispatch();
#elif CASE == 19
    pthread_mutex_destroy(&first);
#elif CASE == 20
    int* alias = (int*)&first;
    *alias = 0;
#elif CASE == 21
    extern void escape(pthread_mutex_t*);
    escape(&first);
#elif CASE == 26
    int* heap = malloc(sizeof(int));
    *heap = 1;
    free(heap);
#elif CASE == 28
    pthread_mutex_lock(&third);
    pthread_mutex_unlock(&third);
#endif
    value = 2;
    pthread_mutex_unlock(&first);
  }
  return 0;
}
int main(void) {
#if CASE == 15
  observe(0);
#endif
  pthread_t thread;
  pthread_create(&thread, 0, worker, CASE == 27 ? (void*)&value : 0);
#if CASE == 12 || CASE == 14
  for (int i = 0; i < 2; ++i)
#endif
  {
    pthread_mutex_lock(&first);
#if CASE == 3
    pthread_mutex_unlock(&first);
    pthread_mutex_lock(&second);
#elif CASE == 7
    if (__VERIFIER_nondet_int()) {
      pthread_mutex_lock(&second);
      pthread_mutex_unlock(&first);
    } else {
      pthread_mutex_unlock(&first);
    }
#elif CASE == 14
    if (i == 0) pthread_mutex_lock(&second);
    pthread_mutex_unlock(&first);
    if (i != 0) pthread_mutex_lock(&second);
#else
    pthread_mutex_lock(&second);
    pthread_mutex_unlock(&first);
#endif
#if CASE == 30
    pthread_mutex_lock(&first);
    pthread_mutex_unlock(&first);
#endif
#if CASE == 2 || CASE == 5
    pthread_mutex_unlock(&second);
#if CASE == 5
    pthread_mutex_lock(&second);
#endif
#endif
#if CASE == 10 || CASE == 15
    observe(0);
#else
    assert(value == 2);
#endif
#if CASE == 12 || CASE == 14
    pthread_mutex_unlock(&second);
#endif
  }
  return 0;
}
