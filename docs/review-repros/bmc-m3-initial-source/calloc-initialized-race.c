// calloc provides a real initialized source before the worker writes.
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
  x = calloc(1, sizeof(*x));
  y = calloc(1, sizeof(*y));
  if (!x || !y) return 0;
  pthread_create(&thread, NULL, worker, NULL);
  pthread_mutex_lock(&m);
  printf("%d\n", *x);
  pthread_mutex_unlock(&m);
  printf("%d\n", *y);
  pthread_join(thread, NULL);
  return 0;
}
