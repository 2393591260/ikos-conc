#include <pthread.h>
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
int storage[12];
int data;
void *worker(void *arg) {
  pthread_mutex_lock(&mutex);
  if (pthread_cond_wait((pthread_cond_t *)storage, &mutex) == 0) data = 1;
  pthread_mutex_unlock(&mutex);
  return 0;
}
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  data = 2;
  pthread_join(t, 0);
  return 0;
}
