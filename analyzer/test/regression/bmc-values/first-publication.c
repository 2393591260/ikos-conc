// SPDX-License-Identifier: MIT
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#ifndef CASE
#define CASE 0
#endif
#ifndef COUNT
#define COUNT 3
#endif
#ifdef WIDE
typedef long long payload;
#else
typedef int payload;
#endif
extern int __VERIFIER_nondet_int(void);
extern void opaque(void *);
struct cell { payload datum; struct cell *next; };
struct cell *head, *other;
pthread_mutex_t graph = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t data = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t wrong = PTHREAD_MUTEX_INITIALIZER;

void initialize(struct cell *p, payload value) {
#if CASE == 1
  other = p;
#endif
  p->datum = value;
  p->next = 0;
}
void insert(struct cell *p, struct cell **slot) {
#if CASE != 2
  pthread_mutex_lock(&graph);
#endif
  p->next = *slot;
  *slot = p;
#if CASE != 2
  pthread_mutex_unlock(&graph);
#endif
}
struct cell *take(struct cell *p) {
#if CASE == 3
  pthread_mutex_lock(&wrong);
#else
  pthread_mutex_lock(&graph);
#endif
  while (p->next && __VERIFIER_nondet_int()) p = p->next;
#if CASE == 3
  pthread_mutex_unlock(&wrong);
#else
  pthread_mutex_unlock(&graph);
#endif
#if CASE == 4
  p = p->next;
#endif
#if CASE == 20
  if (__VERIFIER_nondet_int()) return p;
  struct cell *saved = p;
  return saved;
#else
  return p;
#endif
}
void *writer(void *arg) {
  (void)arg;
  for (int k = 0; k < 2; ++k) {
    struct cell *p = malloc(sizeof *p);
#if CASE == 7
    other = p;
#elif CASE == 14
    struct cell *old = p;
    p = malloc(sizeof *p);
    initialize(old, 4);
#elif CASE == 15
    opaque(p);
#endif
#if CASE != 14
    initialize(p, 4);
#endif
    insert(p, &head);
#if CASE == 8
    free(p);
#elif CASE == 9
    memcpy(&p->next, &head, sizeof head);
#elif CASE == 21
    p->next = (struct cell *)((char *)0 + 4);
#elif CASE == 10
    other = (struct cell *)(unsigned long long)(unsigned long)p;
#elif CASE == 11
    opaque(&head);
#elif CASE == 13
    ((long long *)&p->next)[0] = 0;
#endif
  }
#if CASE == 6
  initialize(head, 7);
#elif CASE == 16
  pthread_mutex_lock(&graph);
  struct cell *captured = head;
  pthread_mutex_unlock(&graph);
  struct cell *p = take(captured);
  insert(p, &other);
#elif CASE == 18
  struct cell local;
  pthread_mutex_lock(&graph);
  head = &local;
  pthread_mutex_unlock(&graph);
#elif CASE == 19
  pthread_mutex_lock(&graph);
  head = other;
  pthread_mutex_unlock(&graph);
#endif
  return 0;
}
void *reader(void *arg) {
  (void)arg;
  pthread_mutex_lock(&graph);
  struct cell *p = head;
  pthread_mutex_unlock(&graph);
  p = take(p);
#if CASE != 5
  pthread_mutex_lock(&data);
#endif
  p->datum++;
#if CASE != 5
  pthread_mutex_unlock(&data);
#endif
  return 0;
}
int main(void) {
  head = malloc(sizeof *head);
  initialize(head, 0);
  other = malloc(sizeof *other);
  initialize(other, 0);
  pthread_t a[COUNT], b[COUNT];
  for (int k = 0; k < COUNT; ++k) pthread_create(&a[k], 0, writer, 0);
  for (int k = 0; k < COUNT; ++k) pthread_create(&b[k], 0, reader, 0);
#if CASE == 12
  head = other;
#endif
  // An immutable seed can be acquired without the graph lock. A concurrent
  // root writer, even in a later-visited callback, must invalidate that fact.
#if CASE == 17 || CASE == 19
  pthread_mutex_lock(&data);
  head->datum++;
  pthread_mutex_unlock(&data);
#endif
  for (int k = 0; k < COUNT; ++k) pthread_join(a[k], 0);
  for (int k = 0; k < COUNT; ++k) pthread_join(b[k], 0);
  return 0;
}
