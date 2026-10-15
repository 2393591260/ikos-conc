// SPDX-License-Identifier: MIT
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
extern int __VERIFIER_nondet_int(void);
extern void __VERIFIER_assume(int);
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long counter;
#else
typedef int counter;
#endif
int datum;
pthread_t *published;
uintptr_t published_bits;
pthread_t saved_handle;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
void *writer(void *argument) {
  pthread_mutex_lock(&mutex);
  datum = 7;
  pthread_mutex_unlock(&mutex);
#if CASE == 9
  __VERIFIER_assume(0);
#endif
  return 0;
}
int main(void) {
#if CASE == 1 || CASE == 12
  const int required = 1;
#elif CASE == 11
  const int required = 0;
#else
  const int required = 2;
#endif
#ifdef LITERAL
  int count = required;
#else
  int count = __VERIFIER_nondet_int();
  __VERIFIER_assume(count == required);
#endif
  pthread_t *handles = malloc(count * sizeof(*handles));
#if CASE == 5
  published = handles;
#elif CASE == 16 || CASE == 21
  published_bits = (uintptr_t)handles;
#endif
  for (counter i = 0; i < count; ++i) {
#if CASE == 7
    pthread_t *output = (pthread_t *)((char *)handles + 1);
#elif CASE == 8
    pthread_t *output = handles + count;
#else
    pthread_t *output = handles + (uintptr_t)i;
#endif
    pthread_create(output, 0, writer,
#if CASE == 19
                   handles
#else
                   0
#endif
    );
  }
#if CASE == 25
  datum = __VERIFIER_nondet_int();
#endif
#if CASE == 6
  handles[0] = 0;
#elif CASE == 22
  ((unsigned char *)handles)[1] = 0;
#endif
#if CASE == 1
  pthread_create(&handles[0], 0, writer, 0);
#elif CASE == 12
  pthread_t old = handles[0];
  pthread_create(&handles[0], 0, writer, 0);
  pthread_join(old, 0);
#elif CASE == 13
  (void)*(volatile pthread_t *)handles;
#elif CASE == 17 || CASE == 18 || CASE == 24
  pthread_t copied = handles[0];
#if CASE == 17
  __VERIFIER_assume(copied != 0);
#elif CASE == 24
  __VERIFIER_assume(copied + 1 != 0);
#else
  saved_handle = copied;
#endif
  pthread_join(copied, 0);
#endif
#if CASE == 4
  free(handles);
#endif
#if CASE != 12 && CASE != 17 && CASE != 18 && CASE != 24
  for (counter i = 0; i < count;
#if CASE == 1 || CASE == 2 || CASE == 27
       ++i
#else
       i += 2
#endif
       ) {
#if CASE == 3
    pthread_join(handles[0], 0);
    pthread_join(handles[0], 0);
#else
    pthread_join(handles[(uintptr_t)i], 0);
#endif
  }
#endif
#if CASE == 14
  free(handles + 1);
#elif CASE != 4 && CASE != 20 && CASE != 21
  free(handles);
#endif
#if CASE == 15
  free(handles);
#elif CASE == 23
  (void)*(volatile pthread_t *)handles;
#endif
#if CASE == 26 || CASE == 27
  datum = __VERIFIER_nondet_int();
#endif
#if CASE == 10
  pthread_mutex_lock(&mutex);
  int result = datum;
  pthread_mutex_unlock(&mutex);
  return result;
#else
  return datum;
#endif
}
