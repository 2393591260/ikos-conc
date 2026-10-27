// Helper allocation outcomes must cover opaque library destinations too.
#include <pthread.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
static int flag;
static unsigned allocation_calls;
static void *allocate(size_t n) { ++allocation_calls; return malloc(n); }
static void *worker(void *arg) { (void)arg; flag++; return NULL; }
int main(void) {
  pthread_t t;
  pthread_mutex_t *p = allocate(sizeof(*p));
  void *other = allocate(sizeof(*p));
  if (!other) abort();
#if CASE == 0
  if (p) return 0;
#else
  if (!p) abort();
#endif
  pthread_mutex_init(p, NULL);
  pthread_create(&t, NULL, worker, NULL);
  flag++;
  return 0;
}
