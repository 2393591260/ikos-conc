// SPDX-License-Identifier: MIT
#include <stddef.h>
#include <pthread.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
#ifndef BUCKETS
#define BUCKETS 3
#endif
#ifndef MEMBERS
#define MEMBERS 2
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
extern int __VERIFIER_nondet_int(void);
#if CASE == 38
int pthread_mutex_init(pthread_mutex_t *p, const pthread_mutexattr_t *a) {
  (void)p; (void)a; return 0;
}
#elif CASE == 39
int pthread_mutex_lock(pthread_mutex_t *p) { (void)p; return 0; }
#endif
static void bound(int condition) { if (!condition) abort(); }
struct edge { struct edge *next, *previous; };
struct item { datum value; struct edge link; };
struct directory {
  struct edge entries[BUCKETS];
  pthread_mutex_t locks[BUCKETS];
} directory;

static void reset(struct edge *entry) {
#if CASE != 8
  entry->next = entry;
#endif
#if CASE != 26
  entry->previous = entry;
#endif
}
static struct item *construct(int value) {
  struct item *fresh = malloc(sizeof(struct item) - (CASE == 21));
#if CASE != 25
  fresh->value = value;
#endif
  reset(&fresh->link);
#if CASE == 22
  return malloc(sizeof(struct item));
#else
  return fresh;
#endif
}
static void insert(struct edge *fresh, struct edge *head) {
  struct edge *old = head->next;
  old->previous = fresh;
  fresh->next = old;
  fresh->previous = head;
  head->next = fresh;
}

static void *visit(void *ignored) {
  (void)ignored;
  int bucket = __VERIFIER_nondet_int();
  bound(bucket >= 0);
  while (CASE == 18 ? bucket <= BUCKETS : bucket < BUCKETS) {
#if CASE == 2
    pthread_mutex_lock(&directory.locks[(bucket + 1) % BUCKETS]);
#elif CASE == 41
    if (__VERIFIER_nondet_int()) pthread_mutex_lock(&directory.locks[bucket]);
#elif CASE != 1
    pthread_mutex_lock(&directory.locks[bucket]);
#endif
    struct edge *edge = directory.entries[bucket].next;
    struct item *item = (struct item *)((char *)edge - offsetof(struct item, link));
    while (&item->link != &directory.entries[bucket]) {
#if CASE == 42
      if (item->value) item->link.next = &directory.entries[0];
#endif
      item->value++;
#if CASE == 4
      item->link.next = &directory.entries[0];
#endif
      edge = item->link.next;
      item = (struct item *)((char *)edge - offsetof(struct item, link));
    }
#if CASE == 2
    pthread_mutex_unlock(&directory.locks[(bucket + 1) % BUCKETS]);
#elif CASE != 1 && CASE != 40
    pthread_mutex_unlock(&directory.locks[bucket]);
#endif
#if CASE == 15
    struct edge *first = directory.entries[bucket].next;
    struct item *unlocked = (struct item *)((char *)first - offsetof(struct item, link));
    if (first != &directory.entries[bucket]) unlocked->value++;
#endif
    ++bucket;
  }
  return 0;
}

int main(void) {
  pthread_t one, two;
  struct item *saved = 0;
  for (int bucket = 0; bucket < BUCKETS; ++bucket) {
    reset(&directory.entries[bucket]);
#if CASE != 28
    pthread_mutex_init(&directory.locks[bucket], 0);
#endif
#if CASE == 27
    pthread_mutex_init(&directory.locks[bucket], 0);
#endif
    for (int member = 0; member < MEMBERS; ++member) {
      struct item *fresh = construct(member * bucket);
      if (bucket == 0 && member == 0) saved = fresh;
#if CASE == 5
      if (bucket != 0 && member == 0) fresh = saved;
#endif
      insert(&fresh->link, &directory.entries[bucket]);
#if CASE == 9
      pthread_create(&one, 0, visit, 0);
#endif
    }
  }
#if CASE == 10
  ((char *)&directory.entries[0].next)[0] = 0;
#elif CASE == 11
  extern void opaque(void *);
  opaque(&directory);
#elif CASE == 20
  free(saved);
#elif CASE == 24
  if (&((struct item *)0)->link == &directory.entries[0]) return 0;
#elif CASE == 33
  saved->link.next = &saved->link;
#elif CASE == 45
  ((char *)&directory.locks[0])[0] = 0;
#elif CASE == 46
  pthread_mutex_destroy(&directory.locks[0]);
#endif
  pthread_create(&one, 0, visit, 0);
  pthread_create(&two, 0, visit, 0);
#if CASE == 34
  *(volatile int *)(size_t)(unsigned)&directory.entries[0] = 1;
#elif CASE == 35
  if ((unsigned)&directory.entries[0] == 7) saved->value = 7;
#elif CASE == 36
  extern void observe(unsigned);
  observe((unsigned)&directory.entries[0]);
#elif CASE == 37
  return (int)(unsigned long long)(size_t)&directory.entries[0];
#endif
#if CASE == 12
  saved->value = 0;
#elif CASE == 14
  reset(&saved->link);
#elif CASE == 23
  return (int)(size_t)&directory.entries[0];
#endif
  return 0;
}
