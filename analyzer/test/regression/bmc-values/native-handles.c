// Native values must not turn a family or an old generation into MUST joined.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#if ASSERT_MODE == 1
#include <assert.h>
#elif ASSERT_MODE == 2 || ASSERT_MODE == 3
extern void __assert_fail(const char *, const char *, unsigned int, const char *);
#elif ASSERT_MODE == 4
extern void __assert_fail(const char *, const char *, unsigned long long, const char *);
#endif
#ifdef FAKE_CREATE
int pthread_create(pthread_t *out, const pthread_attr_t *attr,
                   void *(*entry)(void *), void *arg) {
  (void)attr; (void)entry; (void)arg;
  *out = 17;
  return 0;
}
#endif
#if ASSERT_MODE == 3
void __assert_fail(const char *test, const char *file, unsigned int line,
                   const char *function) {
  (void)test; (void)file; (void)line; (void)function;
}
#endif
#ifndef CASE
#define CASE 0
#endif
#ifndef COUNT
#define COUNT 3
#endif
#ifdef WIDE
typedef long long datum_type;
#else
typedef int datum_type;
#endif
pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
datum_type data;
void *escaped;
extern int __VERIFIER_nondet_int(void);
extern void unknown(void *);
void *worker(void *unused) {
  (void)unused;
#if CASE != 1 && CASE != 2 && CASE != 3 && CASE != 17
  pthread_mutex_lock(&guard);
#endif
  data++;
#if CASE != 1 && CASE != 2 && CASE != 3 && CASE != 17
  pthread_mutex_unlock(&guard);
#endif
  return NULL;
}
void *inert(void *unused) { (void)unused; return NULL; }
int main(void) {
  pthread_t handles[COUNT];
#if ASSERT_MODE == 1
  assert(__VERIFIER_nondet_int());
#elif ASSERT_MODE == 2
  static char mutable_diagnostic[] = "condition";
  if (__VERIFIER_nondet_int())
    __assert_fail(mutable_diagnostic, "source.c", 1, "main");
#elif ASSERT_MODE == 3 || ASSERT_MODE == 4
  if (__VERIFIER_nondet_int())
    __assert_fail("condition", "source.c", 1, "main");
#endif
#if CASE == 10
  volatile pthread_t self = pthread_self();
  (void)self;
#elif CASE == 11
  escaped = handles;
#elif CASE == 12
  unknown(handles);
#elif CASE == 13
  memset(handles, 0, sizeof(handles));
#endif
  for (int k = 0; k < COUNT; ++k) {
#if CASE == 8
    pthread_create(&handles[COUNT], NULL, worker, NULL);
#elif CASE == 9
    pthread_create(&handles[k-1], NULL, worker, NULL);
#elif CASE == 14
    pthread_create(&handles[k], NULL, worker, handles);
#elif CASE == 15
    int status = pthread_create(&handles[k], NULL, worker, NULL);
    if (status) data = 1;
#elif CASE == 16
    long long big = 4294967296LL + COUNT;
    pthread_create(&handles[(int)big], NULL, worker, NULL);
#else
#if CASE == 17
    if (__VERIFIER_nondet_int())
#endif
    pthread_create(&handles[k], NULL, worker, NULL);
#endif
  }
#if CASE == 3
  pthread_t old = handles[0];
#endif
#if CASE == 2 || CASE == 3
  for (int k = 0; k < COUNT; ++k)
    pthread_create(&handles[k], NULL, inert, NULL);
#endif
#if CASE == 4
  if (handles[0] == 1) data = 2;
#elif CASE == 5
  if (handles[0] != 1) data = 2;
#elif CASE == 6
  unsigned char byte = *((unsigned char *)handles);
  if (byte) data = 2;
#elif CASE == 7
  int truncated = (short)handles[0];
  if (truncated) data = 2;
#elif CASE == 18
  *(int *)(unsigned long)__VERIFIER_nondet_int() = 2;
#endif
#if CASE == 3
  pthread_join(old, NULL);
#else
  for (int k = 0; k < COUNT; ++k) {
#if CASE == 1 || CASE == 2 || CASE == 17
    pthread_join(handles[0], NULL);
#elif CASE == 19
    pthread_join(handles[k], &escaped);
#else
    pthread_join(handles[k], NULL);
#endif
  }
#endif
#if CASE == 1 || CASE == 2 || CASE == 3 || CASE == 17
  data = 7;
#endif
  return 0;
}
