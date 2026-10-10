#include <pthread.h>
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
int data;
void wake(pthread_cond_t *c) {pthread_cond_signal(c);}
void *worker(void *arg) {
  pthread_mutex_lock(&mutex);
  if (pthread_cond_wait(&condition, &mutex) == 0) data = 1;
  pthread_mutex_unlock(&mutex);
  return 0;
}
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  wake(&condition);
  data = 2;
  pthread_join(t, 0);
  return 0;
}
