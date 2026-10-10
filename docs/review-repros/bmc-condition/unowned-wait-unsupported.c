#include <pthread.h>
pthread_mutex_t mutex;
pthread_cond_t condition;
int data;
void *worker(void *arg) {
  if (pthread_cond_wait(&condition, &mutex) == 0) data = 1;
  return 0;
}
int main(void) {
  pthread_t t;
  pthread_mutex_init(&mutex, 0);
  pthread_cond_init(&condition, 0);
  pthread_create(&t, 0, worker, 0);
  data = 2;
  pthread_join(t, 0);
  return 0;
}
