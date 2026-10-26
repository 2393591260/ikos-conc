// SPDX-License-Identifier: MIT
#include <pthread.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long payload;
#else
typedef int payload;
#endif
extern int __VERIFIER_nondet_int(void);
struct cell { payload datum; struct cell *next; };
#if CASE == 9
struct cell *roots[sizeof(pthread_mutex_t) / sizeof(void *)];
_Static_assert(sizeof roots == sizeof(pthread_mutex_t), "overlap control extent");
#define GRAPH ((pthread_mutex_t *)roots)
#else
struct cell *roots[5];
#define GRAPH (&graph)
#endif
pthread_mutex_t graph = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t data = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t extra[5];
void initialize(struct cell *p) { p->datum = 0; p->next = 0; }
int choose(void) {
  int n = __VERIFIER_nondet_int() % 5;
  if (n < 0) abort();
  return n;
}
void *writer(void *arg) {
  (void)arg;
  int i = __VERIFIER_nondet_int() % 5;
  if (i < 0) abort();
  struct cell *p = malloc(sizeof *p);
#if CASE != 4
  initialize(p);
#endif
#if CASE == 3
  roots[i] = p;
#elif CASE == 5
  ((unsigned char *)&roots[i])[0] = 0;
#elif CASE == 6
  ((char **)roots)[i] = (char *)p + 1;
#else
  struct cell *base = roots[i];
  pthread_mutex_lock(GRAPH);
  p->next = base->next;
  base->next = p;
#if CASE == 2
  roots[i] = p;
#endif
  pthread_mutex_unlock(GRAPH);
#endif
  return 0;
}
void *reader(void *arg) {
  (void)arg;
  int i = __VERIFIER_nondet_int() % 5;
  if (i < 0) abort();
  struct cell *p = roots[i];
  pthread_mutex_lock(GRAPH);
  while (p->next && __VERIFIER_nondet_int()) p = p->next;
  pthread_mutex_unlock(GRAPH);
#if CASE == 1
  p = p->next;
#endif
  int j = __VERIFIER_nondet_int() % 5;
  if (j < 0) abort();
  pthread_mutex_lock(&extra[j]);
#if CASE != 7
  pthread_mutex_lock(&data);
#endif
  p->datum++;
#if CASE != 7
  pthread_mutex_unlock(&data);
#endif
  pthread_mutex_unlock(&extra[j]);
  return 0;
}
int main(void) {
  for (int i = 0; i < 5; ++i) {
    pthread_mutex_init(&extra[i], 0);
    struct cell *p = malloc(sizeof *p);
    initialize(p);
    roots[i] = p;
  }
  pthread_t a[3], b[3];
  for (int i = 0; i < 3; ++i) pthread_create(&a[i], 0, writer, 0);
  for (int i = 0; i < 3; ++i) pthread_create(&b[i], 0, reader, 0);
#if CASE == 8
  for (int i = 0; i < 5; ++i) pthread_mutex_init(&extra[i], 0);
#endif
  for (int i = 0; i < 3; ++i) pthread_join(a[i], 0);
  for (int i = 0; i < 3; ++i) pthread_join(b[i], 0);
  return 0;
}
