// A zero read must not be invented after explicit initialization to one.
#include <pthread.h>
#include <stdio.h>

static int x = 1, y;
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static void *worker(void *arg) {
  (void)arg;
  pthread_mutex_lock(&m);
  x = 2;
  y = 8;
  pthread_mutex_unlock(&m);
  return NULL;
}
int main(void) {
  pthread_t thread;
  pthread_create(&thread, NULL, worker, NULL);
  pthread_mutex_lock(&m);
  int value = x;
  pthread_mutex_unlock(&m);
  if (value == 0) printf("%d\n", y);
  pthread_join(thread, NULL);
  return 0;
}
