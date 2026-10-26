#include <pthread.h>
extern unsigned int __VERIFIER_nondet_uint(void);
int data;
void *worker(void *arg) {
  unsigned int start = __VERIFIER_nondet_uint();
  if (start != 4294967295U) return 0;
  unsigned int next = start + 1U;
  if (next != 0) data = 1;
  return 0;
}
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, worker, 0);
  pthread_create(&b, 0, worker, 0);
  return 0;
}
