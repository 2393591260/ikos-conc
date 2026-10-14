#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
#ifndef CASE
#define CASE 1
#endif
int first_object;
int data;
unsigned flag;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
void *writer(void *arg) {
  pthread_mutex_lock(&lock);
  data = __VERIFIER_nondet_int();
  pthread_mutex_unlock(&lock);
  return 0;
}
void *argument_writer(void *arg) {
  pthread_mutex_lock(&lock);
  *(int *)arg = 7;
  pthread_mutex_unlock(&lock);
  return 0;
}
void *inert(void *arg) { return 0; }
void *setter(void *arg) {
#if CASE == 8 || CASE == 9
  flag = 256;
#else
  flag = 1;
#endif
  return 0;
}
void *guarded(void *arg) {
#if CASE == 7
  if (flag != 0) return 0;
#elif CASE == 8
  if (((unsigned char *)&flag)[1] != 1) return 0;
#elif CASE == 9
  if (((unsigned char *)&flag)[1] != 0) return 0;
#else
  if (flag != 1) return 0;
#endif
  data = 7;
  return 0;
}
int main(void) {
  pthread_t tids[4];
#if CASE >= 12 && CASE <= 23
  pthread_t worker_tid, idle_tid, other_tid;
  pthread_create(&worker_tid, 0, writer, 0);
#if CASE == 20
  pthread_join(worker_tid, 0);
#elif CASE >= 21
  pthread_detach(worker_tid);
#if CASE == 22
  pthread_create(&worker_tid, 0, writer, 0);
  pthread_detach(worker_tid);
#elif CASE == 23
  pthread_detach(worker_tid);
#endif
#else
  pthread_create(&idle_tid, 0, inert, 0);
#if CASE == 14 || CASE == 16
  pthread_detach(idle_tid);
#if CASE == 16
  pthread_detach(idle_tid);
#endif
#endif
  pthread_join(idle_tid, 0);
#if CASE == 13
  pthread_join(idle_tid, 0);
#elif CASE == 15
  pthread_detach(idle_tid);
#elif CASE == 17
  pthread_create(&other_tid, 0, inert, 0);
  pthread_join(other_tid, 0);
#elif CASE == 18
  pthread_create(&idle_tid, 0, inert, 0);
  pthread_join(idle_tid, 0);
#elif CASE == 19
  extern void retain_handle(pthread_t *);
  retain_handle(&idle_tid);
#endif
#endif
  data = 7;
#elif CASE == 10 || CASE == 11
  pthread_t first_tid, second_tid, inert_tid;
  pthread_create(&first_tid, 0, argument_writer, &first_object);
#if CASE == 10
  pthread_create(&second_tid, 0, argument_writer, &data);
#else
  pthread_create(&second_tid, 0, argument_writer, &first_object);
#endif
  pthread_create(&inert_tid, 0, inert, 0);
  pthread_join(inert_tid, 0);
#elif CASE == 4 || CASE == 5
  pthread_create(&tids[0], 0, writer, 0);
  pthread_t old = tids[0];
  pthread_create(&tids[0], 0, inert, 0);
#if CASE == 4
  pthread_join(old, 0);
#else
  pthread_join(tids[0], 0);
#endif
#elif CASE >= 6
  pthread_create(&tids[0], 0, setter, 0);
  pthread_join(tids[0], 0);
  pthread_create(&tids[1], 0, guarded, 0);
#else
  for (int i = 0; i < 4; ++i) pthread_create(&tids[i], 0, writer, 0);
#if CASE == 3
  pthread_create(&tids[0], 0, writer, 0);
#endif
  for (int i = 0; i < (CASE == 1 ? 3 : 4); ++i) pthread_join(tids[i], 0);
#endif
  return data;
}
