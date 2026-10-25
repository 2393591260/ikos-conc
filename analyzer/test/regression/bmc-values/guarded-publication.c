#include <pthread.h>
#include <stdbool.h>
#if CASE == 30
extern void *malloc(__SIZE_TYPE__);
extern void free(void *);
void returning_stop(void) __asm__("abort");
void returning_stop(void) {}
#else
#include <stdlib.h>
#endif
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
extern int __VERIFIER_nondet_int(void);
#if CASE == 24
static unsigned char published;
#elif CASE == 25
static unsigned int published;
#elif CASE == 1
static bool published = true;
#else
static bool published;
#endif
static datum payload;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t other = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wakeup = PTHREAD_COND_INITIALIZER;
#if CASE == 12
extern __thread int runtime_error __asm__("errno");
#endif
#if CASE == 13
extern void unknown_effect(void);
#endif
static void assume_nonnegative(int n) {
#if CASE == 30
  if (n < 0) for (;;) {}
#else
  if (n < 0) abort();
#endif
}
static void write_payload(void) { payload = 7; }
static void *entry(void *arg) {
  (void)arg;
#if CASE == 22
  if (__VERIFIER_nondet_int()) return 0;
#endif
#if CASE == 8
  if (__VERIFIER_nondet_int()) goto read_data;
#endif
#if CASE == 9
  bool early = published;
#endif
#if CASE == 30
  if (__VERIFIER_nondet_int()) returning_stop();
  else {
#endif
#if CASE == 29
  pthread_mutex_lock(&other);
#else
  pthread_mutex_lock(&gate);
#endif
#if CASE == 4
  pthread_mutex_unlock(&gate);
#endif
#if CASE == 17
  while (published == 0) {
#elif CASE == 18
  while (published) {
#elif CASE == 20 || CASE == 31
  if (!published) {
#elif CASE == 26
  while (published != 1) {
#elif CASE == 9
  while (!early) {
#else
  while (!published) {
#endif
#if CASE == 31
    __builtin_unreachable();
#elif CASE == 19
#elif CASE == 5
    pthread_cond_wait(&wakeup, &other);
#elif CASE == 15
    pthread_mutex_unlock(&gate);
    pthread_cond_wait(&wakeup, &gate);
#else
    pthread_cond_wait(&wakeup, &gate);
#endif
  }
#if CASE == 27
  datum inside = payload;
  (void)inside;
#endif
#if CASE == 29
  pthread_mutex_unlock(&other);
#elif CASE != 4
  pthread_mutex_unlock(&gate);
#endif
#if CASE == 30
  }
#endif
#if CASE == 8
read_data: ;
#endif
#if CASE == 6
  payload = 4;
#endif
#if CASE == 12
  if (runtime_error) return 0;
#endif
#if CASE == 13
  unknown_effect();
#endif
  datum x = payload;
  (void)x;
  return 0;
}
int main(void) {
  int n = __VERIFIER_nondet_int();
  assume_nonnegative(n);
  pthread_t *ids = malloc(n * sizeof *ids);
#if CASE == 14
  pthread_cond_destroy(&wakeup);
#endif
  for (int i = 0; i < n; ++i) pthread_create(&ids[i], 0, entry, 0);
#if CASE == 11
  write_payload();
#elif CASE == 23
  if (__VERIFIER_nondet_int()) payload = 1;
#elif CASE != 2 && CASE != 28
  payload = __VERIFIER_nondet_int();
#endif
#if CASE != 3
  pthread_mutex_lock(&gate);
#endif
#if CASE == 21
  if (__VERIFIER_nondet_int())
#endif
#if CASE == 10
  for (int j = 0; j < 2; ++j)
#endif
  published = true;
#if CASE == 16
  *(unsigned short *)&published = 1;
#endif
#if CASE == 7
  published = false;
#endif
#if CASE == 28
  payload = 1;
#endif
  pthread_cond_broadcast(&wakeup);
#if CASE != 3
  pthread_mutex_unlock(&gate);
#endif
#if CASE == 2
  payload = 1;
#endif
  for (int i = 0; i < n; ++i) pthread_join(ids[i], 0);
  free(ids);
  return 0;
}
