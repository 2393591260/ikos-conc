/* CASE selects a source-level once execution contract control. */
#include <pthread.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
extern void foreign_touch(void *);
#if CASE == 14
static long long g;
#else
static int g;
#endif
#if CASE == 11
static pthread_once_t once = 1;
#else
static pthread_once_t once = PTHREAD_ONCE_INIT;
#endif
static pthread_once_t second = PTHREAD_ONCE_INIT;
static void init(void);
static void *empty(void *p) { (void)p; return 0; }
static void helper(void) { g++; }
static void init(void) {
#if CASE == 20
  /* Deliberately empty: both race targets occur after API-result guards. */
#elif CASE == 5
  g = 0;
#elif CASE == 7
  helper(); helper();
#elif CASE == 8
  pthread_once(&once, init);
  g++;
#elif CASE == 9
  pthread_t t; pthread_create(&t,0,empty,0); g++;
#elif CASE == 15
  g++; abort();
#elif CASE == 16
  if (0) g++;
#else
  g++;
#endif
}
#if CASE == 10
int pthread_once(pthread_once_t *c, void (*cb)(void)) { (void)c; cb(); return 0; }
#endif
static void *worker(void *arg) {
  (void)arg;
#if CASE == 2 || CASE == 7
  init();
#elif CASE == 20
  int result = pthread_once(&once,init);
  if (result != 0) return 0;
#elif CASE == 10
  pthread_once(&once,init);
#else
  pthread_once(&once,init);
#endif
#if CASE == 4
  int read = g; (void)read;
#elif CASE == 5 || CASE == 20
  g++;
#endif
  return 0;
}
#if CASE == 18 || CASE == 19
int main(int argc, char **argv) {
  (void)argv;
#else
int main(void) {
#endif
  pthread_t t;
#if CASE == 18
  if (argc != 1) return 0;
#elif CASE == 19
  if (argc == 1) return 0;
#endif
#if CASE == 5 || CASE == 6
  pthread_once(&once,init);
#endif
#if CASE == 12
  once = 0;
#elif CASE == 13
  foreign_touch(&once);
#endif
#if CASE == 17
  init(); pthread_once(&once,init); return 0;
#else
  pthread_create(&t,0,worker,0);
#endif
#if CASE == 0 || CASE == 18 || CASE == 19 || CASE == 14 || CASE == 8 || CASE == 9 || CASE == 11 || CASE == 12 || CASE == 13 || CASE == 15
  g++;
#elif CASE == 1
  pthread_once(&second,init);
#elif CASE == 2 || CASE == 7 || CASE == 20
  int result = pthread_once(&once,init);
  if (result != 0) return 0;
#if CASE == 20
  g++;
#endif
#elif CASE == 3 || CASE == 4
  pthread_once(&once,init);
#elif CASE == 5
  pthread_once(&once,init);
  g++;
#elif CASE == 10
  pthread_once(&second,init);
#elif CASE == 16
  g++;
#endif
  pthread_join(t,0);
  return 0;
}
