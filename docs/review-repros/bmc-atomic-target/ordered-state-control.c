#include <pthread.h>
extern void __VERIFIER_assume(int);
extern void __VERIFIER_atomic_begin(void);
extern void __VERIFIER_atomic_end(void);
int data, state;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
void __VERIFIER_atomic_write(void) { data = 1; state = 1; }
void *a(void *arg) { __VERIFIER_atomic_write(); return 0; }
void *b(void *arg) { __VERIFIER_atomic_begin(); __VERIFIER_assume(state == 1); data = 2; __VERIFIER_atomic_end(); return 0; }
int main(void) {
  pthread_t x, y;
  pthread_create(&x, 0, a, 0);
  pthread_create(&y, 0, b, 0);
  return 0;
}
