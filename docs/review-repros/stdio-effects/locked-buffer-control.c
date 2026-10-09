#include <pthread.h>
#include <stdio.h>
char data[8];
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
void *format(void *arg) {
  pthread_mutex_lock(&lock);
  snprintf(data, sizeof data, "%c", 'a');
  pthread_mutex_unlock(&lock);
  return 0;
}
void *write(void *arg) {
  pthread_mutex_lock(&lock);
  data[0] = 'b';
  pthread_mutex_unlock(&lock);
  return 0;
}
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, format, 0);
  pthread_create(&b, 0, write, 0);
  return 0;
}
