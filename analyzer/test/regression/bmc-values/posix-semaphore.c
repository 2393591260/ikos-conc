#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <signal.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
extern int __VERIFIER_nondet_int(void);
static datum shared;
static sem_t gate;
#if CASE == 18
extern __thread int runtime_error __asm__("errno");
#endif
static void acquire(void) { sem_wait(&gate); }
static void release(void) { sem_post(&gate); }
#if CASE == 12
int sem_wait(sem_t *s) { (void)s; return 0; }
#endif
static void *entry(void *arg) {
  (void)arg;
#if CASE == 1
#elif CASE == 3
  if (__VERIFIER_nondet_int()) acquire();
#elif CASE == 4
  sem_trywait(&gate);
#elif CASE == 5
  sem_t other;
  sem_wait(&other);
#else
  acquire();
#endif
#if CASE == 7
  *(char *)&gate = 1;
#endif
  shared = __VERIFIER_nondet_int();
#if CASE == 18
  if (runtime_error) shared = 0;
#endif
#if CASE == 6
  /* Ending with the permit held blocks subsequent acquisitions. */
#else
  release();
#endif
  return 0;
}
static void assume_nonnegative(int n) { if (n < 0) abort(); }
int main(void) {
  int n = __VERIFIER_nondet_int();
#if CASE == 2
  sem_init(&gate, 0, 2);
#elif CASE == 13
  sem_init(&gate, 1, 1);
#elif CASE == 9
#else
  sem_init(&gate, 0, 1);
#endif
#if CASE == 8
  signal(SIGUSR1, SIG_IGN);
#endif
  assume_nonnegative(n);
  pthread_t *ids = malloc(n * sizeof *ids);
  for (int i = 0; i < n; ++i) {
#if CASE == 14
    pthread_create((pthread_t *)&gate, 0, entry, 0);
#else
    pthread_create(&ids[i], 0, entry, 0);
#endif
  }
#if CASE == 9
  sem_init(&gate, 0, 1);
#elif CASE == 10
  sem_post(&gate);
#elif CASE == 11
  sem_init(&gate, 0, 1);
#elif CASE == 15
  shared = 0;
#endif
  for (int i = 0; i < n; ++i) {
#if CASE == 16
    if (ids[i]) shared = 0;
#endif
#if CASE == 17
    void *result;
    pthread_join(ids[i], &result);
#else
    pthread_join(ids[i], 0);
#endif
  }
  free(ids);
  return 0;
}
