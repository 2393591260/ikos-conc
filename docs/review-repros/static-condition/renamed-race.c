#include <pthread.h>
pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
int payload;
void *worker(void *arg) {
  pthread_mutex_lock(&gate);
  if (pthread_cond_wait(&ready, &gate) == 0) payload = 1;
  pthread_mutex_unlock(&gate);
  return 0;
}
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  payload = 2;
  pthread_join(t, 0);
  return 0;
}
