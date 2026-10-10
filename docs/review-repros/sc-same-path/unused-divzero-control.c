#include <pthread.h>
#include <limits.h>
extern int __VERIFIER_nondet_int(void);
extern void __VERIFIER_assume(int);
int flag, scratch, data;
void *a(void *arg) {
  int denominator = __VERIFIER_nondet_int();
  __VERIFIER_assume(denominator == 0);
  scratch = 7 / denominator;
  if (flag == 0) data = 1;
  return 0;
}
void *b(void *arg) {data = 2; return 0;}
int main(void) {
  pthread_t x, y;
  pthread_create(&x, 0, a, 0);
  pthread_create(&y, 0, b, 0);
  return 0;
}
