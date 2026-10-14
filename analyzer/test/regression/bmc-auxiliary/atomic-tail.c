#include <pthread.h>
extern void __VERIFIER_assume(int);
extern void __VERIFIER_atomic_begin(void);
extern void __VERIFIER_atomic_end(void);
#ifndef TAIL
#define TAIL 0
#endif
#ifndef MAIN_ATOMIC
#define MAIN_ATOMIC 0
#endif
#ifndef ATOMIC_TARGET
#define ATOMIC_TARGET 1
#endif
#ifndef NAMED_ATOMIC
#define NAMED_ATOMIC 0
#endif
#ifndef NESTED_ATOMIC
#define NESTED_ATOMIC 0
#endif
int data, flag;
void __VERIFIER_atomic_worker(void) { data = 1; }
void __VERIFIER_atomic_main(void) { data = 2; }
void *aux(void *arg) { flag = 1; return 0; }
void *writer(void *arg) {
#if NAMED_ATOMIC && ATOMIC_TARGET && !MAIN_ATOMIC
  __VERIFIER_atomic_worker();
#else
#if !MAIN_ATOMIC && ATOMIC_TARGET
  __VERIFIER_atomic_begin();
#if NESTED_ATOMIC
  __VERIFIER_atomic_begin();
#endif
#endif
  data = 1;
#if !MAIN_ATOMIC && ATOMIC_TARGET
#if NESTED_ATOMIC
  __VERIFIER_atomic_end();
#endif
  __VERIFIER_atomic_end();
#endif
#endif
#if !MAIN_ATOMIC && ATOMIC_TARGET
  __VERIFIER_assume(TAIL);
#endif
  return 0;
}
int main(void) {
  pthread_t x, y;
  pthread_create(&x, 0, aux, 0);
  pthread_create(&y, 0, writer, 0);
  pthread_join(x, 0);
#if NAMED_ATOMIC && ATOMIC_TARGET && MAIN_ATOMIC
  __VERIFIER_atomic_main();
#else
#if MAIN_ATOMIC
  __VERIFIER_atomic_begin();
#if NESTED_ATOMIC
  __VERIFIER_atomic_begin();
#endif
#endif
  data = 2;
#if MAIN_ATOMIC
#if NESTED_ATOMIC
  __VERIFIER_atomic_end();
#endif
  __VERIFIER_atomic_end();
#endif
#endif
#if MAIN_ATOMIC
  __VERIFIER_assume(TAIL);
#endif
  return 0;
}
