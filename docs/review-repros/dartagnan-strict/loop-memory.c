#include <pthread.h>
extern int __VERIFIER_nondet_int(void);

int data, skip_first;
void* worker(void* arg) {
  for (int i = 0; i < 2; ++i) {
    if (i == 0 && skip_first)
      continue;
    data = i + 1;
  }
  return 0;
}
int main(void) {
  pthread_t a, b;
  skip_first = __VERIFIER_nondet_int();
  pthread_create(&a, 0, worker, 0);
  pthread_create(&b, 0, worker, 0);
  return 0;
}
