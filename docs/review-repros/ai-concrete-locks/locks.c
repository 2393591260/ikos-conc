#include <pthread.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
extern int __VERIFIER_nondet_int(void);
struct box { pthread_mutex_t a, b; int value; };
static struct box boxes[2];
static int payload;
static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
static void *worker(void *arg) {
  struct box *p = arg;
#if CASE == 5 || CASE == 6
  struct box local;
  pthread_mutex_init(&local.a, 0);
  p = &local;
#elif CASE == 2
  p = &boxes[__VERIFIER_nondet_int() ? 0 : 1];
#endif
#if CASE == 7
  pthread_rwlock_rdlock(&rw);
#elif CASE == 8
  pthread_rwlock_wrlock(&rw);
#elif CASE == 3
  pthread_mutex_lock(__VERIFIER_nondet_int() ? &p->a : &p->b);
#elif CASE == 10
  if (__VERIFIER_nondet_int()) pthread_mutex_lock(&p->a);
#else
  pthread_mutex_lock(&p->a);
#endif
#if CASE == 9
  pthread_mutex_unlock(&p->a);
#endif
#if CASE == 1 || CASE == 2
  boxes[1].value = 1;
#elif CASE == 5 || CASE == 6 || CASE == 7 || CASE == 8 || CASE == 11
  payload = 1;
#else
  p->value = 1;
#endif
#if CASE == 7 || CASE == 8
  pthread_rwlock_unlock(&rw);
#elif CASE != 9 && CASE != 10
  pthread_mutex_unlock(&p->a);
#endif
  return 0;
}
static void launch(void) {
  struct box local;
  struct box *p = &boxes[0];
#if CASE == 0 || CASE == 4
  p = &local;
#endif
  pthread_mutex_init(&p->a, 0);
  pthread_mutex_init(&p->b, 0);
#if CASE == 2
  pthread_mutex_init(&boxes[1].a, 0);
#endif
  pthread_t first, second;
  pthread_create(&first, 0, worker, p);
  pthread_create(&second, 0, worker, p);
  pthread_join(first, 0);
  pthread_join(second, 0);
}
int main(void) {
#if CASE == 6
  for (int i = 0; i < 2; ++i) launch();
#else
  launch();
#endif
  return 0;
}
