#include <pthread.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
extern int __VERIFIER_nondet_int(void);
#if CASE == 7 || CASE == 14
pthread_mutex_t guards[2] = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER};
#else
pthread_mutex_t guards[2];
#endif
pthread_mutex_t other;
pthread_mutexattr_t attributes;
datum payload;
void initialize(pthread_mutex_t *p) { pthread_mutex_init(p, 0); }
void *worker(void *arg) {
  (void)arg;
#if CASE == 11
  pthread_mutex_init(&guards[0], 0);
#endif
  pthread_mutex_lock(&guards[0]);
  payload = 1;
  pthread_mutex_unlock(&guards[0]);
  return 0;
}
int main(void) {
  pthread_t tid;
#if CASE == 7
  pthread_mutex_lock(&guards[0]);
  pthread_mutex_unlock(&guards[0]);
#endif
#if CASE == 2
  for (int i = 0; i < 2; ++i) initialize(&guards[0]);
#elif CASE == 3
  if (__VERIFIER_nondet_int()) initialize(&guards[0]);
  else pthread_mutex_init(&guards[0], 0);
#elif CASE == 8 || CASE == 9
  pthread_mutexattr_init(&attributes);
  pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_ERRORCHECK);
  pthread_mutex_init(&guards[0], &attributes);
#elif CASE != 10 && CASE != 11 && CASE != 14
  initialize(&guards[0]);
#endif
#if CASE == 1 || CASE == 9
  pthread_mutex_init(&guards[0], 0);
#elif CASE == 4
  pthread_mutex_init(&other, 0);
#elif CASE == 5 || CASE == 12 || CASE == 16
#if CASE == 16
  pthread_mutex_lock(&guards[0]);
  pthread_mutex_unlock(&guards[0]);
#endif
  initialize(&guards[1]);
#elif CASE == 6
  pthread_mutex_init((pthread_mutex_t *)((char *)guards + 4), 0);
#elif CASE == 13
  for (int i = 1; i < 2; ++i) initialize(&guards[i]);
#elif CASE == 15
  pthread_mutex_lock(&guards[0]);
  pthread_mutex_unlock(&guards[0]);
  initialize(&guards[0]);
#endif
  pthread_create(&tid, 0, worker, 0);
#if CASE == 10
  pthread_mutex_init(&guards[0], 0);
#endif
  payload = 2;
  return 0;
}
