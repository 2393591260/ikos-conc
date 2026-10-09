#include <pthread.h>
extern unsigned char __VERIFIER_nondet_uchar(void);
int data;
void *worker(void *arg) {
  unsigned char start = __VERIFIER_nondet_uchar();
  if (start != 255) return 0;
  unsigned char end = start + 2;
  if (start < end) data = 1;
  return 0;
}
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, worker, 0);
  pthread_create(&b, 0, worker, 0);
  return 0;
}
