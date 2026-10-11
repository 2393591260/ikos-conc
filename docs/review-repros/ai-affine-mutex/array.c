#include <pthread.h>
#include <stddef.h>
extern int __VERIFIER_nondet_int(void);
extern void abort(void);
#ifndef CASE
#define CASE 0
#endif
static int index_in(int n) {
  int i = __VERIFIER_nondet_int();
  if (i < 0 || i >= n) abort();
  return i;
}
struct item { int data; pthread_mutex_t mutex, other; };
#if CASE == 12
static struct item items[260];
#else
static struct item items[4];
#endif
static void release_owned_mutex(pthread_mutex_t *m) { pthread_mutex_unlock(m); }
#if CASE == 10
typedef struct { unsigned char byte[64]; } wide_block;
static struct { unsigned char bytes[1024]; pthread_mutex_t mutexes[2]; } arena;
#endif
static void *work(void *arg) {
  (void)arg;
#if CASE == 10
  int i = index_in(2);
  char *base = (char *)&arena + i * sizeof(pthread_mutex_t);
  pthread_mutex_t *m = (pthread_mutex_t *)(base + offsetof(__typeof__(arena), mutexes));
  pthread_mutex_lock(m);
  *(volatile wide_block *)base = (wide_block){{1}};
  pthread_mutex_unlock(m);
#else
  int i = index_in(4);
  struct item *p = &items[i];
  pthread_mutex_lock(&p->mutex);
#if CASE == 3
  ++i; /* p keeps its earlier value */
#endif
  p->data = 1;
  pthread_mutex_unlock(&p->mutex);
#endif
  return 0;
}
int main(void) {
  pthread_t t;
#if CASE == 10
  pthread_mutex_init(&arena.mutexes[0], 0);
  pthread_mutex_init(&arena.mutexes[1], 0);
#else
  for (int j = 0; j < (CASE == 12 ? 260 : 4); ++j) {
    pthread_mutex_init(&items[j].mutex, 0);
    pthread_mutex_init(&items[j].other, 0);
  }
#endif
  pthread_create(&t, 0, work, 0);
#if CASE == 11
  pthread_t another;
  pthread_create(&another, 0, work, 0);
#endif
#if CASE == 10
  int i = index_in(2);
  char *base = (char *)&arena + i * sizeof(pthread_mutex_t);
  pthread_mutex_t *m = (pthread_mutex_t *)(base + offsetof(__typeof__(arena), mutexes));
  pthread_mutex_lock(m);
  *(volatile wide_block *)base = (wide_block){{2}};
  pthread_mutex_unlock(m);
#else
  int i = index_in(CASE == 5 ? 2 : 4);
#if CASE == 12
  i += 256;
#endif
#if CASE == 14
  int independent = index_in(4);
#endif
  struct item *p = &items[i];
#if CASE == 1
  pthread_mutex_t *m = &p->other;
#elif CASE == 2
  pthread_mutex_t *m = &items[(i + 1) % 4].mutex;
#elif CASE == 5
  pthread_mutex_t *m = &items[2 * i].mutex;
#elif CASE == 7
  pthread_mutex_t *m = (pthread_mutex_t *)((char *)(void *)p + offsetof(struct item, mutex));
#elif CASE == 9
  pthread_mutex_t *m = &items[(unsigned char)i].mutex;
#else
  pthread_mutex_t *m = &p->mutex;
#endif
  pthread_mutex_lock(m);
#if CASE == 8
  release_owned_mutex(m);
#endif
#if CASE == 4
  items[(i + 1) % 4].data = 2;
#elif CASE == 12
  items[(unsigned char)i].data = 2;
#elif CASE == 13
  for (int round = 0; round < 2; ++round) {
    i = (i + 1) % 4;
    items[i].data = 2;
  }
#elif CASE == 14
  items[independent].data = 2;
#elif CASE == 6
  ((unsigned char *)&p->data)[1] = 2;
#else
  p->data = 2;
#endif
#if CASE != 8
  pthread_mutex_unlock(m);
#endif
#endif
  pthread_join(t, 0);
#if CASE == 11
  pthread_join(another, 0);
#endif
  return 0;
}
