#include <pthread.h>
#include <stddef.h>
extern int __VERIFIER_nondet_int(void);
#ifndef CASE
#define CASE 0
#endif
struct object {
  int data;
  pthread_mutex_t mutex;
  pthread_mutex_t other;
};
#if CASE == 8
static struct object objects[2];
#define FIRST objects[0]
#define SECOND objects[1]
#else
static struct object first, second;
#define FIRST first
#define SECOND second
#endif
static void *work(void *arg) {
  (void)arg;
#if CASE == 9 || CASE == 12
  struct object *q = __VERIFIER_nondet_int() ? &FIRST : &SECOND;
#else
  struct object *q = &FIRST;
#endif
  pthread_mutex_lock(&q->mutex);
  q->data = 1;
  pthread_mutex_unlock(&q->mutex);
  return 0;
}
static void release(pthread_mutex_t *m) { pthread_mutex_unlock(m); }
int main(void) {
  pthread_t t;
  pthread_mutex_init(&FIRST.mutex, 0);
  pthread_mutex_init(&SECOND.mutex, 0);
  pthread_mutex_init(&FIRST.other, 0);
  pthread_mutex_init(&SECOND.other, 0);
  struct object *p = __VERIFIER_nondet_int() ? &FIRST : &SECOND;
  pthread_create(&t, 0, work, 0);
#if CASE == 12
  pthread_t another;
  pthread_create(&another, 0, work, 0);
#endif
#if CASE == 1 || CASE == 10
  pthread_mutex_lock(&p->other);
#elif CASE == 4
  if (__VERIFIER_nondet_int()) pthread_mutex_lock(&p->mutex);
#else
  pthread_mutex_lock(&p->mutex);
#endif
#if CASE == 2
  pthread_mutex_unlock(&p->mutex);
#elif CASE == 3
  release(&p->mutex);
#elif CASE == 5
  p = &FIRST;
#elif CASE == 6
  p = p == &FIRST ? &SECOND : &FIRST;
#endif
#if CASE == 7 || CASE == 10
  ((unsigned char *)&p->data)[1] = 2;
#elif CASE == 11
  for (int i = 0; i < 2; ++i) {
    if (i) p = &FIRST;
    p->data = 2;
  }
#else
  p->data = 2;
#endif
#if CASE != 2 && CASE != 3 && CASE != 4 && CASE != 5 && CASE != 6 && CASE != 11
#if CASE == 1 || CASE == 10
  pthread_mutex_unlock(&p->other);
#else
  pthread_mutex_unlock(&p->mutex);
#endif
#endif
  pthread_join(t, 0);
#if CASE == 12
  pthread_join(another, 0);
#endif
  return 0;
}
