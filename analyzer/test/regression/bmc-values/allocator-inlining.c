// Constructors may be expanded without erasing source argument observations.
#include <pthread.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
struct cell { datum value; struct cell *next; };
struct cell *first, *second;
pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
extern int __VERIFIER_nondet_int(void);
struct cell *construct(datum value) {
  struct cell *p = malloc(sizeof(*p));
  p->value = value;
  p->next = NULL;
  return p;
}
void sink(datum value) {}
datum identity(datum value) { return value; }
datum uninitialized_return(void) { datum value; return value; }
void forward(datum value) { sink(value); }
void *worker(void *ignored) {
  pthread_mutex_lock(&guard);
  first->value++;
  pthread_mutex_unlock(&guard);
  return NULL;
}
int main(void) {
  pthread_t thread;
  datum value;
#if CASE == 0 || CASE == 1
  value = 3;
#elif CASE == 3
  value = __VERIFIER_nondet_int();
#elif CASE == 4
  sink(value); value = 3;
#elif CASE == 5
  (void)identity(value); value = 3;
#elif CASE == 6
  (void)uninitialized_return(); value = 3;
#elif CASE == 7
  forward(value); value = 3;
#endif
  first = construct(value);
  second = construct(5);
  pthread_create(&thread, NULL, worker, NULL);
  pthread_mutex_lock(&guard);
  datum *address = &first->value;
  pthread_mutex_unlock(&guard);
#if CASE == 1
  pthread_mutex_lock(&guard);
#endif
  *address = 42;
#if CASE == 1
  pthread_mutex_unlock(&guard);
#endif
  return 0;
}
