// SPDX-License-Identifier: MIT
#if CASE == 7
#define pthread_setspecific header_setspecific
#endif
#include <pthread.h>
#if CASE == 7
#undef pthread_setspecific
extern int pthread_setspecific(long long, const void *);
#endif
#include <stdint.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
pthread_key_t key, other;
datum shared;
void *published;
extern void opaque(void *);
extern int another_setspecific(pthread_key_t, const void *);
void destructor(void *p) { shared++; }
#if CASE == 8
int pthread_setspecific(pthread_key_t k, const void *p) { shared++; return 0; }
#endif
#if CASE == 24
void *pthread_getspecific(pthread_key_t k) { return &shared; }
#endif
void touch(void *p) { *(datum *)p = 1; }
void *idle(void *p) { return NULL; }
void *child_touch(void *p) { touch(p); return NULL; }
void *worker(void *arg) {
  datum x = 1, y = 2;
#if CASE == 1
  shared++;
#elif CASE == 2
  pthread_setspecific(key, &shared);
  *(datum *)pthread_getspecific(key) = 1;
#elif CASE == 3
  pthread_setspecific(key, (void *)42);
  if ((intptr_t)pthread_getspecific(key) != 42) abort();
#elif CASE == 4
  void *p = (void *)42;
  pthread_setspecific(key, p);
  *(datum *)p = 1;
#elif CASE == 5
  void *p = (void *)42;
  pthread_setspecific(key, p);
  published = p;
#elif CASE == 7
  pthread_setspecific(key, &x);
#elif CASE == 9
  another_setspecific(key, &x);
#elif CASE == 10
  void *p = (void *)42;
  pthread_setspecific(key, p);
  opaque(p);
#elif CASE == 11
  pthread_key_create(&key, NULL);
#elif CASE == 12
  pthread_key_create((pthread_key_t *)&published, NULL);
  if (published) touch(published);
#elif CASE == 13
  pthread_setspecific(key, &shared);
  touch(pthread_getspecific(key));
#elif CASE == 14
  if (pthread_setspecific(key, &x) == 0) shared++;
#elif CASE == 15
  if (pthread_getspecific(key) == &shared) shared++;
#elif CASE == 16
  pthread_key_delete(key);
#elif CASE == 17
  pthread_setspecific(key, &x);
  if (pthread_getspecific(other) == &x) shared++;
#elif CASE == 18
  void *p = (void *)(unsigned long long)0x100000010ULL;
  pthread_setspecific(key, p);
#elif CASE == 19
  void *p = (void *)(unsigned long long)0x100000010ULL;
  pthread_setspecific(key, p);
  touch(p);
#elif CASE == 20
  pthread_key_t local;
  pthread_key_create(&local, NULL);
  pthread_setspecific(local, &x);
  if (pthread_getspecific(local) != &x) abort();
#elif CASE == 21
  pthread_setspecific(key, &x);
  pthread_setspecific(key, &y);
  if (pthread_getspecific(key) != &y) shared++;
#elif CASE == 22
  pthread_setspecific(key, &shared);
  published = pthread_getspecific(key);
#elif CASE == 23
  pthread_t id;
  pthread_create(&id, NULL, idle, NULL);
  pthread_key_create((pthread_key_t *)&id, NULL);
  pthread_join(id, NULL);
#elif CASE == 24
  touch(pthread_getspecific(key));
#elif CASE == 25
  pthread_key_t local;
  if (pthread_key_create(&local, NULL) != 0) shared++;
#elif CASE == 26
  pthread_t id;
  pthread_setspecific(key, &x);
  pthread_create(&id, NULL, child_touch, pthread_getspecific(key));
  x++;
  pthread_join(id, NULL);
#elif CASE == 27
  pthread_setspecific(key, &x);
  published = (void *)(uintptr_t)pthread_getspecific(key);
#elif CASE == 28
  touch(*(void **)&key);
#elif CASE == 29
  union { pthread_key_t n; void *p; } u;
  u.n = key;
  touch(u.p);
#else
  pthread_setspecific(key, &x);
  if (pthread_getspecific(key) != &x) abort();
  pthread_setspecific(key, &y);
  if (pthread_getspecific(key) != &y) abort();
#endif
  return NULL;
}
int main(void) {
#if CASE == 6
  pthread_key_create(&key, destructor);
#else
  pthread_key_create(&key, NULL);
#endif
#if CASE == 15
  pthread_setspecific(key, &shared);
#endif
  pthread_t a, b;
  pthread_create(&a, NULL, worker, NULL);
  pthread_create(&b, NULL, worker, NULL);
  pthread_join(a, NULL);
  pthread_join(b, NULL);
}
