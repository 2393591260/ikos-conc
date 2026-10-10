#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
int state, data;
void *a(void *arg) {if (state == 0) data = 1; return 0;}
void *b(void *arg) {if (state != 0) data = 2; return 0;}
int main(void) {
  state = __VERIFIER_nondet_int();
  pthread_t x, y;
  pthread_create(&x, 0, a, 0);
  pthread_create(&y, 0, b, 0);
  return 0;
}
