#include <pthread.h>
extern int __VERIFIER_nondet_int(void);

int data, skip_first;
void* worker(void* arg) {
  data = (int)(long)arg;
  return 0;
}

int main(void) {
  pthread_t threads[2];
  skip_first = __VERIFIER_nondet_int();
  for (int i = 0; i < 2; ++i) {
    if (i == 0 && skip_first)
      continue;
    pthread_create(&threads[i], 0, worker, (void*)(long)(i + 1));
  }
  data = 3;
  return 0;
}
