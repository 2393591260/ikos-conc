#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
int left, right;
void *worker(void *arg) {
  int *chosen = __VERIFIER_nondet_int() ? &left : &right;
  *chosen = 7;
  return 0;
}
int main(void) {
  pthread_t a,b;
  pthread_create(&a, 0, worker, 0);
  pthread_create(&b, 0, worker, 0);
  return 0;
}
