#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
int selector, data;
void *worker(void *arg) {
  if (selector == 1) data = 1;
  return 0;
}
int main(void) {
  selector = __VERIFIER_nondet_int();
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  if (selector == 0) data = 2;
  return 0;
}
