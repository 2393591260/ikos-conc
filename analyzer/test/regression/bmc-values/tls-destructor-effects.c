#include <pthread.h>
#include <stdlib.h>
#include <assert.h>
#include <setjmp.h>
#ifndef CASE
#define CASE 0
#endif
int shared;
void* escaped;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_key_t extra_key;
jmp_buf saved;
#if CASE == 16
void abort(void) { shared++; for (;;) {} }
#elif CASE == 17
void __assert_fail(const char* x, const char* y, unsigned line, const char* f) {
  shared++; for (;;) {}
}
#endif
static void helper(void* p) {
#if CASE == 7
  shared++;
#else
  if ((long)p == 42) abort();
#endif
}
#if CASE == 25
static void* dispose(void* p) { return p; }
#elif CASE == 20
extern void dispose(void* p);
#else
static void dispose(void* p) {
#if CASE == 1 || CASE == 22
  (void)p;
#elif CASE == 2 || CASE == 7 || CASE == 23
  helper(p);
#elif CASE == 3
  for (;;) {}
#elif CASE == 4
  shared++;
#elif CASE == 5
  if (shared) abort();
#elif CASE == 6
  if (*(int*)p) abort();
#elif CASE == 8
  escaped = malloc(4);
#elif CASE == 9
  free(p);
#elif CASE == 10
  ((void (*)(void*))p)(p);
#elif CASE == 11
  extern void unknown(void);
  unknown();
#elif CASE == 12
  pthread_mutex_lock(&mutex);
#elif CASE == 13
  pthread_key_create(&extra_key, 0);
#elif CASE == 14
  pthread_exit(0);
#elif CASE == 15
  longjmp(saved, 1);
#elif CASE == 16
  if ((long)p == 42) abort();
#elif CASE == 18
  __assert_fail((const char*)p, __FILE__, __LINE__, __func__);
#elif CASE == 24
  dispose(p);
#else
  long value = (long)p;
  assert(value != 42);
#endif
}
#endif
static void* worker(void* argument) {
  pthread_key_t* key = argument;
  int result = pthread_setspecific(*key, (void*)42);
  if (result) abort();
#if CASE == 19
  shared++;
#endif
  return 0;
}
int main(void) {
  pthread_key_t key;
#if CASE == 22
  int result = pthread_key_create(&key, 0);
#elif CASE == 25
  int result = pthread_key_create(&key, (void (*)(void*))dispose);
#else
  int result = pthread_key_create(&key, dispose);
#endif
  if (result) abort();
  pthread_t a, b;
  pthread_create(&a, 0, worker, &key);
  pthread_create(&b, 0, worker, &key);
  result = pthread_setspecific(key, (void*)16);
  if (result) abort();
  pthread_join(a, 0);
  pthread_join(b, 0);
  return 0;
}
