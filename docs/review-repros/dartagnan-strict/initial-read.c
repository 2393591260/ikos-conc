#include <pthread.h>
int data = 7;
int other = 7;
void *worker(void *arg) {
  other = 7;
  data = 9;
  return 0;
}
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  volatile int observed = data;
  return 0;
}
