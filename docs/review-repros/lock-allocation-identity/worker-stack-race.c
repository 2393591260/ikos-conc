#include <pthread.h>
#include <stdlib.h>
int data;
void *worker(void *arg) {
  pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_t *own = &lock;
  pthread_mutex_lock(own);
  data = 1;
  pthread_mutex_unlock(own);
  return 0;
}
int main(void) {
  pthread_t a,b;
  pthread_create(&a,0,worker,0);
  pthread_create(&b,0,worker,0);
  pthread_join(a,0);
  pthread_join(b,0);
  return 0;
}
