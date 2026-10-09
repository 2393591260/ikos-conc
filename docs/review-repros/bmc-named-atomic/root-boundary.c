#include <pthread.h>
extern void __VERIFIER_assume(int);
int state, data;
void *__VERIFIER_atomic_worker(void *arg) { data = 1; return 0; }
void *a(void *arg) {  return 0; }
void *b(void *arg) { data = 2; return 0; }
int main(void) {
  pthread_t x, y;
  pthread_create(&x, 0, __VERIFIER_atomic_worker, 0);
  pthread_create(&y, 0, b, 0);
  return 0;
}
