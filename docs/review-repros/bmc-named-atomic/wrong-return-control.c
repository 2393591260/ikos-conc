#include <pthread.h>
extern void __VERIFIER_assume(int);
int state, data;
int __VERIFIER_atomic_identity(int v) { return v; }
void *a(void *arg) { int x = __VERIFIER_atomic_identity(0); int y = __VERIFIER_atomic_identity(1); __VERIFIER_assume(x == 1 || y == 0); data = 1; return 0; }
void *b(void *arg) { data = 2; return 0; }
int main(void) {
  pthread_t x, y;
  pthread_create(&x, 0, a, 0);
  pthread_create(&y, 0, b, 0);
  return 0;
}
