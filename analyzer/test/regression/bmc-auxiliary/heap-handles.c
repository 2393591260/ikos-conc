#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
extern int __VERIFIER_nondet_int(void);
#ifndef CASE
#define CASE 1
#endif
int data;
pthread_t *published;
uintptr_t published_bits;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
void *writer(void *arg) {
  pthread_mutex_lock(&lock);
  data = 7;
  pthread_mutex_unlock(&lock);
  return 0;
}
int main(void) {
#if CASE == 0
  int count = 1;
#else
  int count = __VERIFIER_nondet_int();
  if (count < 0) abort();
#if CASE == 2
  if (count != 2) abort();
#elif CASE == 3
  if (count != 7) abort();
#endif
#endif
  pthread_t *tids = malloc(count * sizeof(pthread_t));
#if CASE == 7
  free(tids);
#elif CASE == 9
  published = tids;
#elif CASE == 12
  published_bits = (uintptr_t)tids;
#endif
  for (int i = 0; i < count; ++i) {
#if CASE == 4
    pthread_t *slot = tids + count;
#elif CASE == 10
    pthread_t *slot = (pthread_t *)((char *)tids + 1);
#else
    pthread_t *slot = tids + i;
#endif
    pthread_create(slot, 0, writer,
#if CASE == 8
                   tids
#else
                   0
#endif
    );
  }
#if CASE == 5
  free(tids + 1);
#elif CASE != 7
  free(tids);
#endif
#if CASE == 6
  free(tids);
#elif CASE == 11
  volatile pthread_t after_free = tids[0];
  (void)after_free;
#endif
#if CASE == 13
  pthread_mutex_lock(&lock);
  int result = data;
  pthread_mutex_unlock(&lock);
  return result;
#else
  return data;
#endif
}
