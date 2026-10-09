#include <pthread.h>
int output;
pthread_mutexattr_t attr;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
void *getter(void *unused) {
  pthread_mutex_lock(&lock);
  pthread_mutexattr_gettype(&attr, &output);
  pthread_mutex_unlock(&lock);
  return 0;
}
void *writer(void *unused) {
  pthread_mutex_lock(&lock);
  output = 1;
  pthread_mutex_unlock(&lock);
  return 0;
}
int main(void) {
  pthread_t a, b;
  pthread_mutexattr_init(&attr);
  pthread_create(&a, 0, getter, 0);
  pthread_create(&b, 0, writer, 0);
  return 0;
}
