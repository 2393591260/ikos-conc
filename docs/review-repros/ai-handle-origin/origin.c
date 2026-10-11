#include <pthread.h>
#ifndef CASE
#define CASE 0
#endif
extern int __VERIFIER_nondet_int(void);
static int payload;
static pthread_t handle, other;
static void *worker(void *arg) { payload = 1; return 0; }
static void *benign(void *arg) { return 0; }
static int factory(void) {
#if CASE == 1 || CASE == 2 || CASE == 8 || CASE == 14 || CASE == 15 || CASE == 16
  if (!__VERIFIER_nondet_int()) {
#if CASE == 2 || CASE == 14
    return 0;
#else
    return -1;
#endif
  }
#endif
  pthread_create(&handle, 0, worker, 0);
#if CASE == 2 || CASE == 14
  return -1;
#elif CASE == 8
  return 7;
#else
  return 0;
#endif
}
static int forwarding(void) { return factory(); }
static void launch(pthread_t *out) { pthread_create(out, 0, worker, 0); }
static void *parent(void *arg) {
  pthread_create(&handle, 0, worker, 0);
  pthread_join(handle, 0);
  return 0;
}
static void *mutator(void *arg) { handle = other; return 0; }
#if CASE == 10
static int (*callbacks[2])(void) = {factory, factory};
#endif
int main(void) {
#if CASE == 0
  pthread_create(&handle, 0, worker, 0);
  pthread_join(handle, 0);
#elif CASE == 1 || CASE == 2
  if (factory() == 0) pthread_join(handle, 0);
#elif CASE == 3
  pthread_create(&handle, 0, benign, 0);
  pthread_t saved = handle;
  pthread_create(&handle, 0, worker, 0);
  pthread_join(saved, 0);
#elif CASE == 4
  factory();
  pthread_t saved = handle;
  pthread_join(saved, 0);
#elif CASE == 5
  pthread_create(&other, 0, benign, 0);
  factory();
  handle = other;
  pthread_join(handle, 0);
#elif CASE == 6
  for (int i = 0; i < 2; ++i) pthread_create(&handle, 0, worker, 0);
  pthread_join(handle, 0);
#elif CASE == 7
  factory(); factory();
  pthread_join(handle, 0);
#elif CASE == 8
  if (factory() == 7) pthread_join(handle, 0);
#elif CASE == 9
  launch(&handle);
  pthread_join(handle, 0);
#elif CASE == 10
  callbacks[0](); callbacks[1]();
  pthread_join(handle, 0);
#elif CASE == 11
  pthread_t changing;
  pthread_create(&other, 0, benign, 0);
  factory();
  pthread_create(&changing, 0, mutator, 0);
  pthread_join(handle, 0);
#elif CASE == 12
  pthread_t outer;
  pthread_create(&outer, 0, parent, 0);
  pthread_join(outer, 0);
#elif CASE == 13
  pthread_create(&other, 0, worker, 0);
  pthread_create(&handle, 0, benign, 0);
  pthread_join(handle, 0);
#elif CASE == 14
  int result = factory();
  result = 0;
  payload = 3;
  if (result == 0) pthread_join(handle, 0);
#elif CASE == 15
  if (forwarding() == 0) pthread_join(handle, 0);
#elif CASE == 16
  if (factory() == 0) {
    pthread_join(handle, 0);
    payload = 2;
  }
  return 0;
#endif
  payload = 2;
  return 0;
}
