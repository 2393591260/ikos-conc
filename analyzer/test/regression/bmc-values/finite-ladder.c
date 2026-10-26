/* Complete shallow search and execution-boundary controls. */
#include <pthread.h>
#include <stdlib.h>
#ifndef OUTER
#define OUTER 3
#endif
#ifndef CASE
#define CASE 0
#endif
extern int __VERIFIER_nondet_int(void);
static int shared;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
extern void __VERIFIER_atomic_begin(void);
extern void __VERIFIER_atomic_end(void);
static void bounded_body(void) {
  int local=0;
  for (int i=0;i<OUTER;i++) {
    for (int j=0;j<2;j++) {
      if (__VERIFIER_nondet_int()) local++; else local+=2;
    }
  }
#if CASE == 2
  __VERIFIER_atomic_begin();
#elif CASE == 3
  pthread_mutex_lock(&lock);
#endif
  shared++;
#if CASE == 1 || CASE == 2
  abort();
#endif
#if CASE == 2
  __VERIFIER_atomic_end();
#elif CASE == 3
  pthread_mutex_unlock(&lock);
#endif
  (void)local;
}
static void *a(void *p) { (void)p; bounded_body(); return 0; }
static void *b(void *p) { (void)p; bounded_body(); return 0; }
int main(void) {
  pthread_t x,y;
  pthread_create(&x,0,a,0); pthread_create(&y,0,b,0);
  pthread_join(x,0); pthread_join(y,0);
  return 0;
}
