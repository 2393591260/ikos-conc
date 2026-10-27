#include <pthread.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
static int *published;
static int *make(void) {
  int *p = malloc(sizeof(*p));
  if (!p) abort();
  *p = 0;
  return p;
}
static void *first(void *arg) {
  (void)arg;
  int *p = make();
#if CASE == 1
  published = p;
#else
  (*p)++;
#endif
  return NULL;
}
static void *second(void *arg) {
#if CASE == 1
  (void)arg;
  if (published) *published = 42;
  return NULL;
#else
  return first(arg);
#endif
}
int main(void) {
  pthread_t a, b;
  pthread_create(&a, NULL, first, NULL);
  pthread_create(&b, NULL, second, NULL);
  return 0;
}
