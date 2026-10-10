#include <pthread.h>
extern void __VERIFIER_atomic_begin(void);
extern void __VERIFIER_atomic_end(void);
int data = 1, counter;
int cur = 1, prev, next, x;
void *worker(void *arg) {
  for (counter = 0; counter < 5; ++counter) {
    __VERIFIER_atomic_begin();
    data += 1;
    __VERIFIER_atomic_end();
  }
  pthread_exit(0);
}
int main(void) {
  pthread_t t;
  data = 1;
  pthread_create(&t, 0, worker, 0);
  for (x = 0; x < 2; ++x) {
    next = prev + cur;
    prev = cur;
    cur = next;
  }
  data = 9;
  if (data < prev) return 0;
  return 1;
}
