/* DRAFT: expectations unexecuted. Compile a generated #define CASE N first. */
#include <pthread.h>
#ifndef CASE
#define CASE 0
#endif
extern int __VERIFIER_nondet_int(void);
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t other = PTHREAD_MUTEX_INITIALIZER;
static int shared, ready;
static void *worker_a(void *arg) {
  (void)arg;
  pthread_mutex_lock(&mutex);
  shared++;
  pthread_mutex_unlock(&mutex);
  return 0;
}
static void *worker_b(void *arg) {
  (void)arg;
#if CASE == 4
  pthread_mutex_lock(&mutex);
#endif
  shared++;
#if CASE == 4
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}
static void *provider(void *arg) { (void)arg; ready = 1; return 0; }
static void *empty(void *arg) { (void)arg; return 0; }
static void release(void) { pthread_mutex_unlock(&mutex); }
static void *same_worker(void *arg) {
  if (arg) return worker_a(0);
  return worker_b(0);
}
int main(void) {
  pthread_t a,b,p,q;
#if CASE == 12
  pthread_create(&p,0,provider,0);
  pthread_join(p,0);
#endif
#if CASE == 11
  pthread_create(&a,0,same_worker,(void*)1);
#else
  pthread_create(&a,0,worker_a,0);
#endif
  pthread_mutex_lock(&mutex);
#if CASE == 11
  pthread_create(&b,0,same_worker,0);
#else
  pthread_create(&b,0,worker_b,0);
#endif
#if CASE == 1
  if (__VERIFIER_nondet_int()) pthread_join(b,0);
  pthread_mutex_unlock(&mutex);
#elif CASE == 2
  if (__VERIFIER_nondet_int()) pthread_mutex_unlock(&mutex);
  pthread_join(b,0);
#elif CASE == 3
  release();
#elif CASE == 5
  pthread_join(b,0);
  pthread_mutex_unlock(&mutex);
#elif CASE == 6
  /* Deliberately retain ownership. */
#elif CASE == 7
  pthread_mutex_unlock(&other);
#elif CASE == 8
  { int n = __VERIFIER_nondet_int(); if (n && !n) pthread_mutex_unlock(&mutex); }
#elif CASE == 9
  return 0;
  pthread_mutex_unlock(&mutex);
#elif CASE == 10
  pthread_create(&p,0,empty,0);
  pthread_create(&q,0,empty,0);
  pthread_mutex_unlock(&mutex);
#elif CASE == 12
  if (ready == 1) pthread_mutex_unlock(&mutex);
#elif CASE == 13
  { long long n = 0x100000001LL; if (n == 0x100000001LL) pthread_mutex_unlock(&mutex); }
#else
  pthread_mutex_unlock(&mutex);
#endif
  pthread_join(a,0);
  pthread_join(b,0);
  return 0;
}
