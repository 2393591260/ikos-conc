#include <pthread.h>
#include <stddef.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
struct payload { datum value; };
struct record {
#if CASE == 6
  int fake[6];
#else
  pthread_mutex_t lock;
#endif
  pthread_mutex_t other;
  struct payload data;
};
#if CASE == 6
#define LOCK(p) ((pthread_mutex_t *)(p)->fake)
#else
#define LOCK(p) (&(p)->lock)
#endif
#if CASE == 14
extern int tiny_init(int *, void *) __asm__("pthread_mutex_init");
#endif
static void initialize(struct record *p) {
#if CASE == 14
  tiny_init((int *)LOCK(p), 0);
#else
  pthread_mutex_init(LOCK(p), 0);
#endif
}
static void *worker(void *arg) {
  struct record *p = (struct record *)((char *)arg - offsetof(struct record, data));
#if CASE == 8
  pthread_mutex_t local;
  pthread_mutex_init(&local, 0);
  pthread_mutex_lock(&local);
#else
  pthread_mutex_lock(LOCK(p));
#endif
  p->data.value = 1;
#if CASE != 8
  pthread_mutex_unlock(LOCK(p));
#endif
  return 0;
}
static void spawn(struct record *p, pthread_t *t) {
#if CASE == 15
  t = (pthread_t *)((char *)LOCK(p) + 4);
#endif
  pthread_create(t, 0, worker, &p->data);
}
#if CASE == 7
static struct record *expired(pthread_t *t) {
  struct record temporary;
  initialize(&temporary);
  spawn(&temporary, t);
  return &temporary;
}
#endif
int main(void) {
  pthread_t tid;
#if CASE == 7
  struct record *p = expired(&tid);
  p->data.value = 2;
  return 0;
#elif CASE == 11
  struct record slots[2];
  struct record *p = &slots[1];
#else
  struct record object;
  struct record *p = &object;
#endif
#if CASE == 12
  struct record separate;
  initialize(&separate);
#endif
#if CASE == 4
  ((unsigned char *)LOCK(p))[0] = 0;
#endif
#if CASE != 2
  initialize(p);
#endif
#if CASE == 3
  initialize(p);
#elif CASE == 5
  ((unsigned char *)LOCK(p))[0] = 0;
#elif CASE == 9
  pthread_mutex_unlock(LOCK(p));
#elif CASE == 10
  pthread_mutex_init(&p->other, 0);
#endif
  p->data.value = 0;
  spawn(p, &tid);
#if CASE == 1
  pthread_mutex_lock(LOCK(p));
#elif CASE == 10 || CASE == 13
  pthread_mutex_lock(&p->other);
#elif CASE == 12
  pthread_mutex_lock(LOCK(&separate));
#endif
  p->data.value = 2;
#if CASE == 1
  pthread_mutex_unlock(LOCK(p));
#elif CASE == 10 || CASE == 13
  pthread_mutex_unlock(&p->other);
#elif CASE == 12
  pthread_mutex_unlock(LOCK(&separate));
#endif
  return 0;
}
