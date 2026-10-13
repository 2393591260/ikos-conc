// Explicit stores initialize both heap objects before any selected spawn.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static int *x, *y;
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static void *worker(void *arg) {
  (void)arg;
  pthread_mutex_lock(&m);
  *x = 3;
  *y = 8;
  pthread_mutex_unlock(&m);
  return NULL;
}
int main(void) {
  pthread_t thread;
  x = malloc(sizeof(*x));
  y = malloc(sizeof(*y));
  *x = 0;
  *y = 0;
  pthread_create(&thread, NULL, worker, NULL);
  pthread_mutex_lock(&m);
  printf("%d\n", *x);
  pthread_mutex_unlock(&m);
  printf("%d\n", *y);
  pthread_join(thread, NULL);
  return 0;
}
