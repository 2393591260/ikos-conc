#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
int left, right;
void *worker(void *arg) {
  right = 9;
  return 0;
}
int main(void) {
  pthread_t t;
  int *chosen = __VERIFIER_nondet_int() ? &left : &right;
  pthread_create(&t, 0, worker, 0);
  *chosen = 7;
  pthread_join(t, 0);
  return 0;
}
