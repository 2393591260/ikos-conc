#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <errno.h>
extern int __VERIFIER_nondet_int(void);
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
sem_t permit;
#if CASE == 2
sem_t other;
#endif
datum payload;
void assume_positive(int n) { if (n < 0) abort(); }
#if CASE == 9
int sem_wait(sem_t *s) { (void)s; return 0; }
#endif
void update(void) {
#if CASE == 7
  payload = __VERIFIER_nondet_int();
#endif
#if CASE == 6 || CASE == 20
  sem_post(&permit);
#endif
#if CASE == 5
  if (sem_wait(&permit)) abort();
#else
  sem_wait(&permit);
#endif
  payload = __VERIFIER_nondet_int();
  sem_post(&permit);
}
void *worker(void *arg) { (void)arg; update(); return 0; }
int main(void) {
#if CASE == 10
  errno = 0;
#endif
#if CASE == 11 || CASE == 19 || CASE == 20
  sem_init(&permit, 0, 2);
#elif CASE == 12 || CASE == 13
  sem_init(&permit, 0, 0);
#elif CASE == 14
  sem_init(&permit, 0, 3);
#elif CASE == 15
  sem_init(&permit, 0, (unsigned)-1);
#elif CASE == 16
  sem_init(&permit, 0, 2147483647);
#elif CASE == 17
  if (sem_init(&permit, 0, 2)) abort();
#elif CASE == 18
  sem_init(&permit, 1, 2);
#else
  sem_init(&permit, 0, 1);
#endif
#if CASE == 8 || CASE == 19
  ((unsigned char *)&permit)[0] = 0;
#endif
  int n = __VERIFIER_nondet_int();
  assume_positive(n);
#if CASE == 7
  n = 2;
#elif CASE == 4
  n = 1;
#endif
  pthread_t *ids = malloc(n * sizeof(pthread_t));
  for (int i = 0; i < n; ++i) pthread_create(&ids[i], 0, worker, 0);
#if CASE == 2
  sem_post(&other);
#elif CASE == 3
  if (n < 0) sem_post(&permit);
#elif CASE != 1 && (CASE < 11 || CASE == 13)
  sem_post(&permit);
#endif
  for (int i = 0; i < n; ++i) pthread_join(ids[i], 0);
  free(ids);
  return 0;
}
