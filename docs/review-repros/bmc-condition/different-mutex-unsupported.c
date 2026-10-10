#include <pthread.h>
pthread_mutex_t mutex;
pthread_cond_t condition;
pthread_mutex_t other;
int data;
void *worker(void *arg) {
  pthread_mutex_lock(&mutex);
  if (pthread_cond_wait(&condition, &mutex) == 0) data = 1;
  pthread_mutex_unlock(&mutex);
  return 0;
}
int main(void) {
  pthread_t t;
  pthread_mutex_init(&mutex, 0);
  pthread_mutex_init(&other, 0);
  pthread_cond_init(&condition, 0);
  pthread_create(&t, 0, worker, 0);
  pthread_mutex_lock(&other);
  pthread_cond_wait(&condition, &other);
  data = 2;
  pthread_mutex_unlock(&other);
  pthread_mutex_lock(&other);
  pthread_cond_wait(&condition, &other);
  pthread_mutex_unlock(&other);
  pthread_join(t, 0);
  return 0;
}
