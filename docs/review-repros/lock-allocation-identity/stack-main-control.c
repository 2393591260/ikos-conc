#include <pthread.h>
int data;
void *worker(void *arg) {
  pthread_mutex_t *shared = arg;
  pthread_mutex_lock(shared);
  data = 1;
  pthread_mutex_unlock(shared);
  return 0;
}
int main(void) {
  pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  pthread_t a,b;
  pthread_create(&a,0,worker,&lock);
  pthread_create(&b,0,worker,&lock);
  pthread_join(a,0);
  pthread_join(b,0);
  return 0;
}
