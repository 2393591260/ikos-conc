#include <pthread.h>
#include <semaphore.h>
sem_t semaphore;
int output;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
void query(void) { sem_getvalue(&semaphore, &output); }
void *get(void *arg) {
  pthread_mutex_lock(&lock);
  query();
  pthread_mutex_unlock(&lock);
  return 0;
}
void *write(void *arg) {
  pthread_mutex_lock(&lock);
  output = 1;
  pthread_mutex_unlock(&lock);
  return 0;
}
int main(void) {
  sem_init(&semaphore, 0, 0);
  pthread_t a, b;
  pthread_create(&a, 0, get, 0);
  pthread_create(&b, 0, write, 0);
  return 0;
}
