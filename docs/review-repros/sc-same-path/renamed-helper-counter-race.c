#include <pthread.h>
extern void __VERIFIER_atomic_begin(void);
extern void __VERIFIER_atomic_end(void);
int output = 1, progress;
int current = 1, previous, successor, x;
void *worker(void *arg) {
  for (progress = 0; progress < 5; ++progress) {
    __VERIFIER_atomic_begin();
    output += 1;
    __VERIFIER_atomic_end();
  }
  pthread_exit(0);
}
int compute(void) {
  for (x = 0; x < 16; ++x) {
    successor = previous + current;
    previous = current;
    current = successor;
  }
  return previous;
}
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  int limit = compute();
  if (output < limit) return 0;
  return 1;
}
