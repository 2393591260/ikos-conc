#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
int left, right, flag;
void *worker(void *arg) {
  if (flag) right = 9;
  return 0;
}
int main(void) {
  pthread_t t;
  flag = __VERIFIER_nondet_int();
  int *chosen = __VERIFIER_nondet_int() ? &left : &right;
  pthread_create(&t, 0, worker, 0);
  if (!flag) *chosen = 7;
  return 0;
}
