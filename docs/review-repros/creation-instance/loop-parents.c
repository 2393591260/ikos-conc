#include <pthread.h>
int data;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
void *child(void *arg) {
  volatile int observed = data;
  return 0;
}
void *parent(void *arg) {
  pthread_t c;
  pthread_mutex_lock(&lock);
  data++;
  pthread_mutex_unlock(&lock);
  pthread_create(&c, 0, child, 0);
  return 0;
}
int main(void) {
  pthread_t parents[2];
  for (int i = 0; i < 2; ++i) pthread_create(&parents[i], 0, parent, 0);
  for (int i = 0; i < 2; ++i) pthread_join(parents[i], 0);
  return 0;
}
