/* Recursive mutex finite-source controls.
 * Cases 0--5 derive from SV-Benchmarks (MIT), SPDX-FileCopyrightText:
 * 2026 Technical University of Munich. Preserve actual operation order.
 */
#define _XOPEN_SOURCE 700
#include <pthread.h>
#ifndef CASE
#define CASE 4
#endif
#if CASE == 0
int global = 0;
pthread_mutex_t mutex;
pthread_t id1, id2;

void *t1(void *arg) {
  pthread_mutex_lock(&mutex);
  global++; // RACE!
  pthread_mutex_unlock(&mutex);
  return NULL;
}

void *t2(void *arg) {
  global++; // RACE!
  return NULL;
}

int main(void) {
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&mutex, &attr);

  pthread_create(&id1, NULL, t1, NULL);
  pthread_mutex_lock(&mutex);
  pthread_create(&id2, NULL, t2, NULL);
  pthread_mutex_lock(&mutex);
  pthread_mutex_unlock(&mutex);
  pthread_mutex_unlock(&mutex);
  pthread_join(id2, NULL);
  return 0;
}
#elif CASE == 1
int global = 0;
pthread_mutex_t mutex;
pthread_t id1, id2;

void *t1(void *arg) {
  pthread_mutex_lock(&mutex);
  global++; // RACE!
  pthread_mutex_unlock(&mutex);
  return NULL;
}

void *t2(void *arg) {
  global++; // RACE!
  return NULL;
}

int main(void) {
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&mutex, &attr);

  pthread_create(&id1, NULL, t1, NULL);
  pthread_mutex_lock(&mutex);
  pthread_mutex_lock(&mutex);
  pthread_create(&id2, NULL, t2, NULL);
  pthread_mutex_unlock(&mutex);
  pthread_mutex_unlock(&mutex);
  pthread_join(id2, NULL);
  return 0;
}
#elif CASE == 2
int global = 0;
pthread_mutex_t mutex;
pthread_t id1;

void *t1(void *arg) {
  pthread_mutex_lock(&mutex);
  pthread_mutex_unlock(&mutex);
  global++; // RACE!
  return NULL;
}

int main(void) {
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&mutex, &attr);

  pthread_mutex_lock(&mutex);
  pthread_create(&id1, NULL, t1, NULL);

  pthread_mutex_lock(&mutex);
  pthread_mutex_unlock(&mutex);

  pthread_mutex_unlock(&mutex);
  global++; // RACE!
  return 0;
}
#elif CASE == 3
int global = 0;
pthread_mutex_t mutex;
pthread_t id1;

void *t1(void *arg) {
  pthread_mutex_lock(&mutex);
  pthread_mutex_unlock(&mutex);
  global++; // RACE!
  return NULL;
}

int main(void) {
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&mutex, &attr);

  pthread_mutex_lock(&mutex);
  pthread_mutex_lock(&mutex);
  pthread_create(&id1, NULL, t1, NULL);
  pthread_mutex_unlock(&mutex);
  pthread_mutex_unlock(&mutex);
  global++; // RACE!
  return 0;
}
#elif CASE == 4
int global = 0;
pthread_mutex_t mutex;
pthread_t id1;

void *t1(void *arg) {
  pthread_mutex_lock(&mutex);
  pthread_mutex_unlock(&mutex);
  global++; // NORACE
  return NULL;
}

int main(void) {
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&mutex, &attr);

  pthread_mutex_lock(&mutex);
  pthread_mutex_lock(&mutex);
  pthread_create(&id1, NULL, t1, NULL);
  global++; // NORACE
  pthread_mutex_unlock(&mutex);
  pthread_mutex_unlock(&mutex);
  return 0;
}
#elif CASE == 5
int global = 0;
pthread_mutex_t mutex;
pthread_t id1;

void *t1(void *arg) {
  pthread_mutex_lock(&mutex);
  pthread_mutex_unlock(&mutex);
  global++; // NORACE
  return NULL;
}

int main(void) {
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&mutex, &attr);

  pthread_mutex_lock(&mutex);
  pthread_create(&id1, NULL, t1, NULL);
  global++; // NORACE
  pthread_mutex_lock(&mutex);
  global++; // NORACE
  pthread_mutex_unlock(&mutex);

  pthread_mutex_unlock(&mutex);

  return 0;
}
#elif CASE >= 30 && CASE <= 34
extern int __VERIFIER_nondet_int(void);
int global;
pthread_mutex_t mutex;
pthread_t id1;
void *t1(void *arg) {
  pthread_mutex_lock(&mutex); pthread_mutex_unlock(&mutex); global++;
  return 0;
}
int main(void) {
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
#if CASE == 30
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
#else
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
#endif
#if CASE == 31
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL); /* snapshot is normal */
#endif
  pthread_mutex_init(&mutex, &attr);
#if CASE == 32
  int extra = __VERIFIER_nondet_int() & 1;
  pthread_mutex_lock(&mutex);
  if (extra) pthread_mutex_lock(&mutex);
#elif CASE == 33
  int count = 1 + (__VERIFIER_nondet_int() & 1);
  for (int i = 0; i < count; ++i) pthread_mutex_lock(&mutex);
#else
  pthread_mutex_lock(&mutex); pthread_mutex_lock(&mutex);
#endif
#if CASE == 34
  t1(0); /* synchronous root activation cannot borrow fresh own-depth0 */
#endif
  pthread_create(&id1, 0, t1, 0);
  pthread_mutex_unlock(&mutex);
  global++; /* CASE30 EDEADLK relock, CASE32/33 depth1: actual unlocked race */
#if CASE == 32
  if (extra) pthread_mutex_unlock(&mutex);
#elif CASE == 33
  if (count == 2) pthread_mutex_unlock(&mutex);
#else
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}
#else
extern int __VERIFIER_nondet_int(void);
extern void opaque(void *);
int global, error_code;
pthread_mutex_t mutex;
pthread_t id1;
#if CASE == 24
pthread_mutex_t *escaped;
#endif
void *t1(void *arg) {
#if CASE == 10
  error_code = pthread_mutex_unlock(&mutex); /* EPERM, no ownership */
#endif
#if CASE == 28
  pthread_mutex_lock(&mutex); pthread_mutex_lock(&mutex);
  pthread_mutex_unlock(&mutex); global++;
  pthread_mutex_unlock(&mutex);
#else
  pthread_mutex_lock(&mutex); pthread_mutex_unlock(&mutex); global++;
#endif
  return 0;
}
int main(void) {
#if CASE == 23
  pthread_mutexattr_t attrs[2];
#define attr attrs[__VERIFIER_nondet_int() & 1]
#else
  pthread_mutexattr_t attr;
#endif
  pthread_mutexattr_init(&attr);
#if CASE == 17
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
#elif CASE == 18
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);
#elif CASE == 19
  pthread_mutexattr_settype(&attr, 3); /* adaptive excluded */
#else
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
#endif
#if CASE == 8
  error_code = pthread_mutexattr_settype(&attr, -1); /* EINVAL, unchanged */
#elif CASE == 20
  ((unsigned char *)&attr)[0] = 0;
#elif CASE == 21
  error_code = ((unsigned char *)&attr)[0];
#elif CASE == 22
  opaque(&attr);
#endif
  pthread_mutex_init(&mutex, &attr);
#if CASE == 9
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL); /* snapshot survives */
  pthread_mutexattr_destroy(&attr);
#elif CASE == 25
  pthread_mutex_init(&mutex, &attr); /* no second live init */
#elif CASE == 24
  escaped = &mutex;
#elif CASE == 26
  ((unsigned char *)&mutex)[0] = 0;
#endif
#if CASE == 11
  pthread_mutex_lock(&mutex);
  if (__VERIFIER_nondet_int()) pthread_mutex_lock(&mutex);
#elif CASE == 12
  for (int i = 0; i < (__VERIFIER_nondet_int() & 3); ++i)
    pthread_mutex_lock(&mutex);
#elif CASE == 13
  opaque(&mutex);
#elif CASE == 27
  pthread_mutex_trylock(&mutex);
#else
  pthread_mutex_lock(&mutex);
  pthread_mutex_lock(&mutex);
#if CASE == 6
  pthread_mutex_lock(&mutex);
#elif CASE == 7
  pthread_mutex_lock(&mutex); pthread_mutex_lock(&mutex);
  pthread_mutex_lock(&mutex); pthread_mutex_lock(&mutex);
  pthread_mutex_lock(&mutex); pthread_mutex_lock(&mutex);
#endif
#endif
  pthread_create(&id1, 0, t1, 0);
#if CASE == 14
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);
#elif CASE == 15
  pthread_mutex_init(&mutex, &attr);
#elif CASE == 16
  pthread_mutexattr_destroy(&attr);
#endif
  global++;
  pthread_mutex_unlock(&mutex);
#if CASE == 6
  pthread_mutex_unlock(&mutex);
#elif CASE == 7
  pthread_mutex_unlock(&mutex); pthread_mutex_unlock(&mutex);
  pthread_mutex_unlock(&mutex); pthread_mutex_unlock(&mutex);
  pthread_mutex_unlock(&mutex); pthread_mutex_unlock(&mutex);
#endif
  pthread_mutex_unlock(&mutex);
#if CASE == 29
  pthread_mutex_lock(&mutex); global++; pthread_mutex_unlock(&mutex);
#endif
  return 0;
}
#endif
