#include <pthread.h>
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
int selector, data;
void *worker(void *arg) {
  pthread_mutex_lock(&mutex);
  int active = selector;
  pthread_mutex_unlock(&mutex);
  if (active) data = 1;
  return 0;
}
int main(void) {
  selector = 1;
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  pthread_mutex_lock(&mutex);
  selector = 0;
  pthread_mutex_unlock(&mutex);
  data = 2;
  return 0;
}
