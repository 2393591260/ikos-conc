#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
#if CASE == 29
#define worker renamed_activity
#define item cell
#define value payload
#define second marker
#define bytes reserved
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
struct item { datum value; datum second; char bytes[4]; };
extern int __VERIFIER_nondet_int(void);
extern void opaque(void *);
static struct item *published;
static int shared;
__attribute__((noinline)) static void *worker(void *arg) {
  struct item *p = arg;
#if CASE == 9
  published = p;
#elif CASE == 10
  shared = (uintptr_t)p;
#elif CASE == 11
  *(struct item **)p = p;
#elif CASE == 12
  opaque(p);
#elif CASE == 26
  free(p);
#endif
#if CASE == 14
  ((char *)p)[sizeof(*p)] = 1;
#elif CASE == 15
  ((datum *)p)[-1] = 1;
#elif CASE == 20
  for (int i = 0; i < 3; ++i) p->value = __VERIFIER_nondet_int();
#else
  p->value = __VERIFIER_nondet_int();
#endif
#if CASE == 19
  p->second = 2;
#elif CASE == 30
  p->bytes[2] = 3;
#endif
#if CASE == 28
  shared++;
#endif
#if CASE != 1 && CASE != 18 && CASE != 13 && CASE != 26
  free(p);
#endif
#if CASE == 27
  if (p) shared = 1;
#elif CASE == 13
  return p;
#endif
  return 0;
}
__attribute__((noinline)) static struct item *allocate(void) { return malloc(sizeof(struct item)); }
int main(void) {
  pthread_t ids[4];
#if CASE == 1
  struct item *outside = malloc(sizeof(*outside));
#endif
  struct item *cached = 0;
  for (int i = 0; i < 2; ++i) {
#if CASE == 21
    if (__VERIFIER_nondet_int()) continue;
#endif
#if CASE == 1
    struct item *p = outside;
#elif CASE == 16
    struct item *p = malloc(1);
#elif CASE == 17
    struct item *p = malloc((unsigned)__VERIFIER_nondet_int());
#elif CASE == 25
    struct item *p = allocate();
#else
    struct item *p = malloc(sizeof(*p));
#endif
#if CASE == 5
    if (i == 0) published = p;
    p = published;
#elif CASE == 22
    if (i == 0) cached = p;
    p = cached;
#elif CASE == 8
    published = p;
#elif CASE == 24
    worker(p);
#endif
#if CASE == 3
    for (int repeat = 0; repeat < 2; ++repeat) {
#endif
#if CASE == 4
retry:
#endif
    pthread_create(&ids[i], 0, worker, p);
#if CASE == 4
    if (__VERIFIER_nondet_int()) goto retry;
#endif
#if CASE == 3
    }
#elif CASE == 2
    pthread_create(&ids[i + 2], 0, worker, p);
#elif CASE == 6
    p->value = 3;
#elif CASE == 7
    free(p);
#elif CASE == 23
    pthread_create(&ids[i + 2], 0, worker, published);
#endif
  }
  for (int i = 0; i < 2; ++i) pthread_join(ids[i], 0);
  return 0;
}
