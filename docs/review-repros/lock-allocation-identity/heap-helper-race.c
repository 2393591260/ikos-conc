#include <pthread.h>
#include <stdlib.h>
int data;
pthread_mutex_t *make_lock(void) { return malloc(sizeof(pthread_mutex_t)); }
void *worker(void *arg) {
  pthread_mutex_t *own = make_lock();
  if (!own) return 0;
  pthread_mutex_init(own,0);
  pthread_mutex_lock(own);
  data = 1;
  pthread_mutex_unlock(own);
  free(own);
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
