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
  pthread_t a,b;
  pthread_create(&a,0,parent,0);
  pthread_create(&b,0,parent,0);
  pthread_join(a,0);
  pthread_join(b,0);
  return 0;
}
