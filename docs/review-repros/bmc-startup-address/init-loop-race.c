#include <pthread.h>
int seed[3], data;
void *worker(void *arg) {
  if (seed[2] == 5) data = 1;
  return 0;
}
int main(void) {
  for (int i = 0; i < 3; ++i) seed[i] = i + 3;
  pthread_t a, b;
  pthread_create(&a, 0, worker, 0);
  pthread_create(&b, 0, worker, 0);
  return 0;
}
