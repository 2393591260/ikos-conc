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
extern int __VERIFIER_nondet_int(void);
struct edge { struct edge *next, *previous; };
struct item { datum value; struct edge link; };
struct directory {
#if CASE == 8
  struct edge entries[3];
#else
  struct item entries[3];
#endif
  pthread_mutex_t locks[3];
} directory;
static pthread_mutex_t common = PTHREAD_MUTEX_INITIALIZER;
#if CASE == 2
static pthread_mutex_t other = PTHREAD_MUTEX_INITIALIZER;
#endif
#if CASE == 8
#define HEAD(i) (&directory.entries[i])
#else
#define HEAD(i) (&directory.entries[i].link)
#endif
static void bound(int condition) { if (!condition) abort(); }
static void reset(struct edge *p) { p->next = p; p->previous = p; }
static struct item *make(int value) {
  struct item *p = malloc(sizeof(struct item));
#if CASE != 7
  p->value = value;
#endif
  reset(&p->link);
  return p;
}
static void insert(struct edge *p, struct edge *head) {
  struct edge *next = head->next;
  next->previous = p;
  p->next = next;
  p->previous = head;
  head->next = p;
}
#if CASE == 19 || CASE == 26
static void release_common(void) { pthread_mutex_unlock(&common); }
#endif
#if CASE == 21
static void acquire_common(void) { pthread_mutex_lock(&common); }
#endif
#if CASE == 30
static void maybe_acquire_common(void) {
  if (__VERIFIER_nondet_int()) pthread_mutex_lock(&common);
}
#endif
static void touch(struct item *p) {
#if CASE == 27 || CASE == 30
#if CASE == 30
  maybe_acquire_common();
#else
  if (__VERIFIER_nondet_int()) pthread_mutex_lock(&common);
#endif
  p->value++;
  abort();
#endif
#if CASE == 18
  if (__VERIFIER_nondet_int()) {
    p->value++;
    return;
  }
#elif CASE == 22
  pthread_mutex_unlock(&common);
#endif
#if CASE == 2
  if (__VERIFIER_nondet_int()) {
    pthread_mutex_lock(&other);
    p->value++;
    pthread_mutex_unlock(&other);
    return;
  }
#endif
#if CASE == 21
  acquire_common();
#else
  pthread_mutex_lock(&common);
#endif
#if CASE == 23
  pthread_mutex_lock(&common);
#endif
#if CASE == 19
  release_common();
#endif
#if CASE == 1
  pthread_mutex_unlock(&common);
#endif
#if CASE == 4
  ((unsigned char *)&p->value)[0] = 1;
#else
  p->value++;
#endif
#if CASE == 26
  release_common();
#elif CASE != 1 && CASE != 19 && CASE != 24
  pthread_mutex_unlock(&common);
#endif
#if CASE == 25
  if (__VERIFIER_nondet_int()) p->value++;
#endif
}
static void *worker(void *argument) {
  (void)argument;
  int k = __VERIFIER_nondet_int();
  bound(k >= 0 && k < 3);
  pthread_mutex_lock(&directory.locks[k]);
  struct edge *p = HEAD(k)->next;
  while (p != HEAD(k)) {
    struct item *node = (struct item *)((char *)p - offsetof(struct item, link));
#if CASE == 5
    p->next = HEAD(k);
#elif CASE == 6
    free(node);
#elif CASE == 9
    if (p != p->next) node->value++;
#elif CASE == 17
    if (p == HEAD(1)) node->value++;
#endif
    touch(node);
    p = p->next;
  }
  pthread_mutex_unlock(&directory.locks[k]);
#if CASE == 12
  if (directory.entries[1].value == 0) pthread_mutex_lock(&common);
  directory.entries[1].value++;
#elif CASE == 13
  touch(&directory.entries[1]);
#endif
  return 0;
}
#if CASE == 20
static void *unlocked_worker(void *argument) {
  (void)argument;
  directory.entries[1].value++;
  return 0;
}
#endif
int main(void) {
#if CASE == 29
  pthread_t first, second;
#else
  pthread_t threads[2];
#endif
  for (int i = 0; i < 3; ++i) {
    reset(HEAD(i));
    pthread_mutex_init(&directory.locks[i], 0);
    for (int j = 0; j < 2; ++j) insert(&make(j)->link, HEAD(i));
  }
  struct edge *a = HEAD(0)->next;
  struct edge *b = HEAD(1)->next;
#if CASE == 3
  a->next = 0;
#elif CASE == 10
  ((unsigned char *)&a->next)[0] = 1;
#elif CASE == 11
  a->next = a;
#elif CASE == 16 || CASE == 17
  if (__VERIFIER_nondet_int()) a->next = b->next;
  else a->next = HEAD(1);
#else
  a->next = b->next;
#endif
#if CASE == 28
  pthread_mutex_lock(&common);
#endif
#if CASE == 29
  pthread_create(&first, 0, worker, 0);
  pthread_create(&second, 0, worker, 0);
  // Scalar outputs are never read: source execution must visit this write.
  directory.entries[1].value = 9;
#else
  for (int i = 0; i < 2; ++i) {
#if CASE == 20
    if (i == 0) pthread_create(&threads[i], 0, worker, 0);
    else pthread_create(&threads[i], 0, unlocked_worker, 0);
#else
    pthread_create(&threads[i + (CASE == 15)], 0, worker, 0);
#endif
  }
#if CASE == 14
  directory.entries[1].value = 9;
#endif
  for (int i = 0; i < 2; ++i) pthread_join(threads[i], 0);
#endif
  return 0;
}
