#include <pthread.h>
#include <stdlib.h>
int data;
pthread_mutex_t *shared;
void *worker(void *arg) {
  pthread_mutex_t *own = shared;
  pthread_mutex_lock(own);
  data = 1;
  pthread_mutex_unlock(own);
  return 0;
}
int main(void) {
  shared = malloc(sizeof(*shared));
  if (!shared) return 0;
  pthread_mutex_init(shared,0);
  pthread_t a,b;
  pthread_create(&a,0,worker,0);
  pthread_create(&b,0,worker,0);
  pthread_join(a,0);
  pthread_join(b,0);
  free(shared);
  return 0;
}
