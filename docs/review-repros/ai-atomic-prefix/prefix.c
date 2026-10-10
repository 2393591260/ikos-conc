#include <pthread.h>
#ifndef CASE
#define CASE 0
#endif
static int data, flag;
#if CASE == 0
#define NAME __VERIFIER_atomic_step
#define LINKAGE static
#elif CASE == 1
#define NAME __VERIFIER_atomic_step
#define LINKAGE
#elif CASE == 2
#define NAME __VERIFIER_atomic_acquire
#define LINKAGE
#elif CASE == 3
#define NAME __VERIFIER_atomic_release
#define LINKAGE
#elif CASE == 4
#define NAME __VERIFIER_atomic_other
#define LINKAGE static
#elif CASE == 5
#define NAME ordinary_step
#define LINKAGE static
#elif CASE == 7
#define NAME __VERIFIER_atomic_acquire
#define LINKAGE
#elif CASE == 8
#define NAME __VERIFIER_atomic_release
#define LINKAGE
#elif CASE == 9
#define NAME __VERIFIER_atomic_missing
#define LINKAGE
#else
#define NAME __VERIFIER_atomic_step
#define LINKAGE
#endif
#if CASE == 9
LINKAGE void NAME(void);
#else
LINKAGE void NAME(void) {
  data++;
  data--;
}
#endif
static void *worker(void *arg) {
  (void)arg;
  NAME();
#if CASE == 4 || CASE == 6 || CASE == 7 || CASE == 8
  flag = 1;
#endif
  return 0;
}
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  NAME();
#if CASE == 4 || CASE == 6 || CASE == 7 || CASE == 8
  flag = 2;
#endif
  pthread_join(t, 0);
  return 0;
}
