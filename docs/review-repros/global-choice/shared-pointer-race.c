#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
int left, right;
int *shared;
void *worker(void *arg) {
  *shared = 7;
  return 0;
}
int main(void) {
  pthread_t a,b;
  shared = __VERIFIER_nondet_int() ? &left : &right;
  pthread_create(&a, 0, worker, 0);
  pthread_create(&b, 0, worker, 0);
  return 0;
}
