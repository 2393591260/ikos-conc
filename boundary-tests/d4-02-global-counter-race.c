#include "_boilerplate.h"
int x, cur = 1, prev = 0, next = 0;   /* fib loop globals */
int i, j;                              /* race globals */
int sink;                              /* live-read sink */
int fib() {
  for (x = 0; x < 12; x++) { next = prev + cur; prev = cur; cur = next; }
  return prev;
}
void *t1(void *a){ __VERIFIER_atomic_begin(); i = i + j; __VERIFIER_atomic_end(); return 0; }
void *t2(void *a){ __VERIFIER_atomic_begin(); j = j + i; __VERIFIER_atomic_end(); return 0; }
int main(){
  pthread_t a, b;
  __VERIFIER_atomic_begin(); i = 1; __VERIFIER_atomic_end();
  __VERIFIER_atomic_begin(); j = 1; __VERIFIER_atomic_end();
  pthread_create(&a, 0, t1, 0);
  pthread_create(&b, 0, t2, 0);
  int correct = fib();
  sink = i + correct;   /* live read of i (races t1/t2 writes), after the loop */
  pthread_join(a, 0); pthread_join(b, 0);
  return 0;
}
