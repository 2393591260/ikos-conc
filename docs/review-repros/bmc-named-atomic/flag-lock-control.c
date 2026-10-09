#include <pthread.h>
extern void __VERIFIER_assume(int);
int state, data;
void __VERIFIER_atomic_acquire(void) { __VERIFIER_assume(state == 0); state = 1; }
void __VERIFIER_atomic_release(void) { __VERIFIER_assume(state == 1); state = 0; }
void *a(void *arg) { __VERIFIER_atomic_acquire(); data = 1; __VERIFIER_atomic_release(); return 0; }
void *b(void *arg) { __VERIFIER_atomic_acquire(); data = 2; __VERIFIER_atomic_release(); return 0; }
int main(void) {
  pthread_t x, y;
  pthread_create(&x, 0, a, 0);
  pthread_create(&y, 0, b, 0);
  return 0;
}
