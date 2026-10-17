// SPDX-License-Identifier: MIT
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
#if CASE == 26
int pthread_mutex_init(pthread_mutex_t *p, const pthread_mutexattr_t *a) {
  (void)p; (void)a; return 0;
}
#endif
static pthread_mutexattr_t attributes;
struct cell { datum value; pthread_mutex_t mutex; int link; };
static struct cell *cells;
static void initialize(struct cell *p, datum value) {
#if CASE != 22
  p->value = value;
#endif
#if CASE == 21
  pthread_mutex_init(&p->mutex, 0);
#else
  pthread_mutex_init(&p->mutex, &attributes);
#endif
}
static void change(int *link) {
  struct cell *p = (struct cell *)((char *)link -
      (unsigned long)&((struct cell *)0)->link);
#if CASE != 2
  pthread_mutex_lock(&p->mutex);
#endif
#if CASE == 11
  pthread_mutex_lock(&p->mutex);
#endif
#if CASE == 27
  p += 2;
#else
  ++p;
#endif
  ++p->value;
#if CASE == 20 || CASE == 21
  --p;
#endif
  pthread_mutex_unlock(&p->mutex);
}
static void *worker(void *arg) {
  (void)arg;
#if CASE == 1
  pthread_mutex_lock(&cells[1].mutex);
  ++cells[1].value;
  pthread_mutex_unlock(&cells[1].mutex);
#else
  change(&cells->link);
#endif
#if CASE == 31
  cells[1].link = ((unsigned char *)&cells[0].mutex)[0];
#endif
#if CASE == 18
  volatile char observed = ((char *)&cells[0].mutex)[0];
  (void)observed;
#endif
  return 0;
}
int main(void) {
  pthread_t thread;
#if CASE != 21
  pthread_mutexattr_init(&attributes);
#if CASE == 10 || CASE == 20
  pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_NORMAL);
#elif CASE == 17
  pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
#else
  pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_ERRORCHECK);
#endif
#endif
#if CASE == 6
  pthread_mutexattr_setprotocol(&attributes, PTHREAD_PRIO_INHERIT);
#elif CASE == 19
  ((char *)&attributes)[0] = 0;
#endif
#if CASE == 14
  cells = 0;
#elif CASE == 16
  cells = malloc(16);
#elif CASE == 27
  cells = malloc(3 * sizeof(struct cell));
#else
  cells = malloc(2 * sizeof(struct cell));
#endif
#if CASE == 15
  cells = (struct cell *)((char *)cells + 1);
#endif
#if CASE == 24
  extern int __VERIFIER_nondet_int(void);
  if (__VERIFIER_nondet_int()) initialize(cells, 1);
#else
  initialize(cells, 1);
#endif
#if CASE == 3
  initialize(cells, 2);
#endif
#if CASE != 4 && CASE != 5
  initialize(&cells[1], 2);
#endif
#if CASE == 27
  initialize(&cells[2], 3);
#endif
#if CASE == 7
  ((char *)&cells[0].mutex)[0] = 0;
#elif CASE == 8
  extern void observe(void *);
  observe(cells);
#elif CASE == 9
  free(cells);
#elif CASE == 13
  volatile unsigned representation = (unsigned)cells;
  (void)representation;
#endif
#if CASE == 28
  *(unsigned *)&cells = 0;
#elif CASE == 29
  volatile unsigned raw = *(unsigned *)&cells;
  (void)raw;
#endif
  pthread_create(&thread, 0, worker, 0);
#if CASE == 30
  ((char *)&cells)[0] = 0;
#endif
#if CASE == 5
  initialize(&cells[1], 2);
#elif CASE == 12
  cells = malloc(2 * sizeof(struct cell));
#elif CASE == 23
  pthread_join(thread, 0);
#endif
  change(&cells->link);
  return 0;
}
