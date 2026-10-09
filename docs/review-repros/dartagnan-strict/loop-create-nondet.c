#include <pthread.h>

extern int __VERIFIER_nondet_int(void);
volatile int data;
void* worker(void* arg) {
  data = 7;
  return 0;
}
int main(void) {
  int count = __VERIFIER_nondet_int();
  if (count < 0 || count > 3)
    return 0;
  for (int i = 0; i < count; ++i) {
    pthread_t thread;
    pthread_create(&thread, 0, worker, 0);
  }
  return data;
}
