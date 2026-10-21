// SPDX-License-Identifier: MIT
#include <pthread.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
#define COUNT 10
static pthread_mutex_t locks[COUNT];
static pthread_mutexattr_t settings;
static datum data;
#if CASE == 29 || CASE == 30
static void initialize_again(void) {
  pthread_mutex_init(&locks[0], 0);
  pthread_mutex_init(&locks[1], 0);
  pthread_mutex_init(&locks[2], 0);
  pthread_mutex_init(&locks[3], 0);
  pthread_mutex_init(&locks[4], 0);
  pthread_mutex_init(&locks[5], 0);
  pthread_mutex_init(&locks[6], 0);
  pthread_mutex_init(&locks[7], 0);
  pthread_mutex_init(&locks[8], 0);
  pthread_mutex_init(&locks[9], 0);
}
#endif
#if CASE == 29
int pthread_mutexattr_init(pthread_mutexattr_t *p) {
  (void)p; initialize_again(); return 0;
}
#elif CASE == 24
int pthread_mutexattr_init(pthread_mutexattr_t *p) { (void)p; return 0; }
#endif
extern int __VERIFIER_nondet_int(void);
extern void inspect(void *);
extern void hidden(void);
static void *worker(void *unused) {
  (void)unused;
#if CASE == 14
  ((unsigned char *)&locks[4])[0] = 1;
#elif CASE == 15
  volatile unsigned char byte = ((unsigned char *)&locks[4])[0];
  (void)byte;
#endif
  pthread_mutex_lock(&locks[4]);
  ++data;
  pthread_mutex_unlock(&locks[4]);
  return 0;
}
int main(void) {
  pthread_t thread;
#if CASE == 21
restart:
#endif
#if CASE == 18
  pthread_mutexattr_init((pthread_mutexattr_t *)((char *)&settings + 1));
#else
  pthread_mutexattr_init(&settings);
#endif
#if CASE != 29
#if CASE == 12
  pthread_mutexattr_settype(&settings, PTHREAD_MUTEX_RECURSIVE);
#elif CASE == 13
  pthread_mutexattr_setrobust(&settings, PTHREAD_MUTEX_ROBUST);
#else
  pthread_mutexattr_settype(&settings, PTHREAD_MUTEX_ERRORCHECK);
#endif
#if CASE == 10
  int limit = __VERIFIER_nondet_int();
#elif CASE == 9
  int limit = COUNT - 1;
#else
  int limit = COUNT;
#endif
#if CASE == 11
  if (__VERIFIER_nondet_int())
#endif
#if CASE == 28
  for (int i = 536870912; i < 536870912 + COUNT; ++i) {
#elif CASE == 23
  for (long long i = 0x100000000LL; i < 0x100000000LL + COUNT; ++i) {
#elif CASE == 25
  for (int i = 0; i < limit; ) {
#elif CASE == 26
  for (int i = 0; i <= limit; ++i) {
#elif CASE == 27
  for (long long i = 0; i < limit; ++i) {
#else
  for (int i = 0; i < limit; ++i) {
#endif
#if CASE == 16
    pthread_mutex_init(&locks[0], &settings);
#else
    pthread_mutex_init(&locks[i], &settings);
#endif
  }
#endif
#if CASE == 7
  pthread_mutex_init(&locks[4], &settings);
#elif CASE == 8
  ((unsigned char *)&settings)[0] = 0;
#elif CASE == 17
  inspect(locks);
#elif CASE == 19
  hidden();
#elif CASE == 20
  pthread_mutexattr_destroy(&settings);
#endif
#if CASE == 2 || CASE == 3 || CASE == 4
  int index = __VERIFIER_nondet_int();
#if CASE != 4
  if (index < 0 || index >= COUNT) abort();
#endif
#if CASE == 3
  if (index == 4) abort();
#endif
#endif
  pthread_create(&thread, 0, worker, 0);
#if CASE == 29 || CASE == 30
  initialize_again();
#elif CASE == 21
  if (__VERIFIER_nondet_int()) goto restart;
#elif CASE == 22
  pthread_mutex_init(&locks[4], &settings);
#endif
  pthread_mutex_lock(&locks[4]);
#if CASE == 2 || CASE == 3 || CASE == 4
  pthread_mutex_unlock(&locks[index]);
#elif CASE == 5
  pthread_mutex_unlock((pthread_mutex_t *)((char *)locks + 1));
#elif CASE == 6
  pthread_mutex_unlock(&locks[COUNT]);
#elif CASE != 1
  pthread_mutex_unlock(&locks[4]);
#endif
  ++data;
  pthread_mutex_unlock(&locks[4]);
  return 0;
}
