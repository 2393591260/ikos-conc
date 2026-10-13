// Fully initialized heap: the protected read can only return 1 or 2.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static int *x, *y;
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static void *worker(void *arg) {
  (void)arg;
  pthread_mutex_lock(&m);
  *x = 2;
  *y = 8;
  pthread_mutex_unlock(&m);
  return NULL;
}
int main(void) {
  pthread_t thread;
  x = malloc(sizeof(*x));
  y = malloc(sizeof(*y));
  if (!x || !y) return 0;
  *x = 1;
  *y = 0;
  pthread_create(&thread, NULL, worker, NULL);
  pthread_mutex_lock(&m);
  int value = *x;
  pthread_mutex_unlock(&m);
  if (value == 0) printf("%d\n", *y);
  pthread_join(thread, NULL);
  return 0;
}
