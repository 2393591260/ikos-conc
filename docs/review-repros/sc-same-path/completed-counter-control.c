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
  pthread_create(&t, 0, worker, 0);
  for (x = 0; x < 12; ++x) {
    next = prev + cur;
    prev = cur;
    cur = next;
  }
  __VERIFIER_atomic_begin();
  int observed = data;
  __VERIFIER_atomic_end();
  if (observed < prev) return 0;
  return 1;
}
