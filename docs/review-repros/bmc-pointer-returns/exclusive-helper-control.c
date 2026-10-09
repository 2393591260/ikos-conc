#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
int selector, data;
int *location(void) { return &data; }
void *a(void *arg) { if (selector == 0) *location() = 1; return 0; }
void *b(void *arg) { if (selector != 0) *location() = 2; return 0; }
int main(void) {
  pthread_t x, y;
  selector = __VERIFIER_nondet_int();
  pthread_create(&x, 0, a, 0);
  pthread_create(&y, 0, b, 0);
  return 0;
}
