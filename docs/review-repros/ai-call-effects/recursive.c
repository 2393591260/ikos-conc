#include <pthread.h>
#ifndef CONTROL
#define CONTROL 0
#endif
static int data;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static void update(int remaining) {
  if(remaining) update(remaining-1);
  else data=2;
}
static void *worker(void *arg) {
  (void)arg;
#if CONTROL
  pthread_mutex_lock(&mutex);
#endif
  data=1;
#if CONTROL
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}
int main(void) {
  pthread_t thread;
  pthread_create(&thread,0,worker,0);
#if CONTROL
  pthread_mutex_lock(&mutex);
#endif
  update(2);
#if CONTROL
  pthread_mutex_unlock(&mutex);
#endif
  pthread_join(thread,0); return 0;
}
