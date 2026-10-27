// Source-exact scalar completion; payload width is independent of handle width.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
extern int __VERIFIER_nondet_int(void);
extern _Bool __VERIFIER_nondet_bool(void);
#if CASE == 34 || CASE == 35
extern void __VERIFIER_atomic_begin();
extern void __VERIFIER_atomic_end();
#elif CASE == 36
extern int __VERIFIER_atomic_begin();
extern int __VERIFIER_atomic_end();
#else
extern void __VERIFIER_atomic_begin(void);
extern void __VERIFIER_atomic_end(void);
#endif
extern void opaque(void);
datum payload1, payload2, payload3, payload4, payload5;
datum payload6, payload7, payload8, payload9;
char *buffer;
pthread_mutex_t mutex;
#if CASE == 23
int pthread_join(pthread_t h, void **p) { (void)h; (void)p; return 0; }
#endif
static void touch(void) { payload1 = 7; }
static void *worker1(void *arg) {
  (void)arg;
#if CASE == 33
  while (__VERIFIER_nondet_bool()) touch();
#elif CASE == 4
  while (__VERIFIER_nondet_int()) touch();
#elif CASE == 5
  datum *local = malloc(4 * sizeof *local);
  if (local) { local[1] = 9; payload1 = local[1]; free(local); }
#elif CASE == 35
  __VERIFIER_atomic_begin(1); payload1 = 7; __VERIFIER_atomic_end();
#elif CASE == 6 || CASE == 34 || CASE == 36
  __VERIFIER_atomic_begin(); payload1 = 7; __VERIFIER_atomic_end();
#elif CASE == 7
  buffer = malloc(8);
#elif CASE == 28
  payload1 = 4294967297LL;
#else
  payload1 = 7;
#endif
  return NULL;
}
static void *worker2(void *arg) {
  (void)arg;
#if CASE == 7
  if (buffer) strcpy(buffer, "Bigshot");
#elif CASE == 8
  payload1 = 9;
#else
  payload2 = 9;
#endif
  return NULL;
}
#define WORKER(N) static void *worker##N(void *p) { (void)p; payload##N = N; return NULL; }
WORKER(3) WORKER(4) WORKER(5) WORKER(6) WORKER(7) WORKER(8) WORKER(9)
#if CASE == 20
static void finish(pthread_t h) { pthread_join(h, NULL); }
#endif
#if CASE == 21
static void start(pthread_t *h) { pthread_create(h, NULL, worker1, NULL); }
#endif
int main(void) {
  pthread_t t1, t2, t3, t4, t5, t6, t7, t8, t9;
#if CASE == 24
  void *result;
#endif
  datum before = 0;
#if CASE == 21
  start(&t1);
#elif CASE == 11
  if (__VERIFIER_nondet_int()) pthread_create(&t1, NULL, worker1, NULL);
#elif CASE == 25
  if (pthread_create(&t1, NULL, worker1, NULL)) return 0;
#elif CASE == 27
  pthread_create(&t1, NULL, worker1, &payload1);
#else
  pthread_create(&t1, NULL, worker1, NULL);
#endif
#if CASE == 30 || CASE == 31 || CASE == 32
#if CASE == 31
  pthread_create(&t2, NULL, worker1, NULL);
#endif
  pthread_join(t1, NULL);
#if CASE != 32
  payload1 = 8;
#endif
#if CASE != 31
  pthread_create(&t2, NULL, worker1, NULL);
#endif
  pthread_join(t2, NULL);
  return (int)payload1;
#else
#if CASE == 14
  pthread_create(&t1, NULL, worker2, NULL);
#endif
#if CASE == 1 || CASE == 2 || CASE == 3 || CASE == 8
  pthread_create(&t2, NULL, worker2, NULL);
#endif
#if CASE == 2 || CASE == 3
  pthread_create(&t3, NULL, worker3, NULL);
#endif
#if CASE == 3
  pthread_create(&t4, NULL, worker4, NULL);
  pthread_create(&t5, NULL, worker5, NULL);
  pthread_create(&t6, NULL, worker6, NULL);
  pthread_create(&t7, NULL, worker7, NULL);
  pthread_create(&t8, NULL, worker8, NULL);
  pthread_create(&t9, NULL, worker9, NULL);
#endif
#if CASE == 26
  pthread_create(&t2, NULL, worker1, NULL);
#endif
#if CASE == 9
  before = payload1;
#endif
#if CASE == 15
  pthread_t old = t1;
#endif
#if CASE == 16
  pthread_detach(t1);
#elif CASE == 17
  pthread_cancel(t1);
#elif CASE == 18
  pthread_mutex_init(&mutex, NULL);
  pthread_mutex_lock(&mutex);
#elif CASE == 19
  opaque();
#elif CASE == 22
  ((unsigned char *)&t1)[sizeof t1 - 1] = 0;
#elif CASE == 29
  __VERIFIER_atomic_begin();
#endif
#if CASE == 10
  if (__VERIFIER_nondet_int()) pthread_join(t1, NULL);
#elif CASE == 12
  pthread_join(t2, NULL);
#elif CASE == 15
  pthread_join(old, NULL);
#elif CASE == 20
  finish(t1);
#elif CASE == 24
  pthread_join(t1, &result);
#else
  pthread_join(t1, NULL);
#endif
#if CASE == 13
  pthread_join(t1, NULL);
#endif
#if CASE == 18
  pthread_mutex_unlock(&mutex);
#elif CASE == 29
  __VERIFIER_atomic_end();
#endif
#if CASE == 7
  pthread_create(&t2, NULL, worker2, NULL);
  pthread_join(t2, NULL);
  return buffer ? buffer[0] : 0;
#endif
#if CASE == 1 || CASE == 2 || CASE == 3 || CASE == 8 || CASE == 26
  pthread_join(t2, NULL);
#endif
#if CASE == 2 || CASE == 3
  pthread_join(t3, NULL);
#endif
#if CASE == 3
  pthread_join(t4, NULL); pthread_join(t5, NULL); pthread_join(t6, NULL);
  pthread_join(t7, NULL); pthread_join(t8, NULL); pthread_join(t9, NULL);
#endif
#if CASE == 28
  return (int)payload1;
#else
  return (int)(before + payload1 + payload2 + payload3 + payload4 + payload5 +
               payload6 + payload7 + payload8 + payload9);
#endif
#endif
}
