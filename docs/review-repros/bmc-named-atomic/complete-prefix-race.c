#include <pthread.h>
extern void __VERIFIER_assume(int);
int state, data;
void __VERIFIER_atomic_publish(int v) { state = 1; state = v; }
void *a(void *arg) { __VERIFIER_atomic_publish(2); data = 1; return 0; }
void *b(void *arg) { __VERIFIER_assume(state == 2); data = 2; return 0; }
int main(void) {
  pthread_t x, y;
  pthread_create(&x, 0, a, 0);
  pthread_create(&y, 0, b, 0);
  return 0;
}
