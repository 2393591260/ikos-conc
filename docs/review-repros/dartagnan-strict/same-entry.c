#include <pthread.h>
int data;
int other;
void *worker(void *arg) {
  data = 7;
  other = 9;
  return 0;
}
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, worker, 0);
  pthread_create(&b, 0, worker, 0);
  pthread_join(a, 0);
  pthread_join(b, 0);
  return 0;
}
