#include <pthread.h>
#include <stdlib.h>
#include <limits.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
#if CASE == 1
#define CAP 64
#elif CASE == 2
#define CAP 7
#elif CASE == 3
#define CAP 1921
#else
#define CAP 65
#endif
datum cells[CAP];
int next = 1, published, unrelated;
pthread_mutex_t serial = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t other = PTHREAD_MUTEX_INITIALIZER;
extern int __VERIFIER_nondet_int(void);
static int allocate_ticket(void) {
  int result = -1;
#if CASE == 4
  int stale = next;
#endif
#if CASE != 5
  pthread_mutex_lock(&serial);
#endif
#if CASE == 6
  pthread_mutex_unlock(&serial);
  int stale = next;
  pthread_mutex_lock(&serial);
#endif
#if CASE == 7
  int stale = next;
  pthread_mutex_unlock(&serial);
  pthread_mutex_lock(&serial);
#endif
#if CASE == 8
  pthread_mutex_lock(&serial);
#endif
#if CASE == 9
  if (__VERIFIER_nondet_int()) pthread_mutex_unlock(&serial);
#endif
#if CASE == 10
  if (__VERIFIER_nondet_int()) next = 1;
#endif
#if CASE == 11
  result = next; next += 2;
  pthread_mutex_unlock(&serial);
  return result; /* Incomplete finite counter closure. */
#else
  if (next + 2 - 1 > CAP) {
    pthread_mutex_unlock(&serial);
#if CASE == 12
    next += 2;
#endif
    result = 0;
  } else {
#if CASE == 4 || CASE == 6 || CASE == 7
    result = stale;
#elif CASE == 13
    result = next + 1;
#else
    result = next;
#endif
#if CASE != 14
    next += 2;
#endif
#if CASE == 15
    pthread_mutex_unlock(&other);
#else
    pthread_mutex_unlock(&serial);
#endif
#if CASE == 16
    result = next; /* A concurrent reservation may have happened by this load. */
#endif
  }
#endif
  return result;
}
static void touch(void) {
  int ticket = allocate_ticket();
#if CASE == 17
  ticket = published;
#elif CASE == 18
  if (__VERIFIER_nondet_int()) ticket = 1;
#endif
#if CASE != 19
  if (ticket == 0) return;
#endif
#if CASE == 20
  cells[ticket + 2] = 1;
#elif CASE == 21
  *((long long*)&cells[ticket + 1]) = 1;
#else
  cells[ticket] = 1;
  cells[ticket + 1] = 2;
#endif
  pthread_mutex_lock(&serial);
  published = ticket;
  pthread_mutex_unlock(&serial);
}
static void* thread(void* ignored) {
#if CASE == 22
  pthread_mutex_unlock(&serial); /* Foreign unmatched unlock invalidates L. */
#elif CASE == 23
  next = 1;
#elif CASE == 24
  pthread_mutex_destroy(&serial);
#elif CASE == 25
  pthread_mutex_init(&serial, 0);
#elif CASE == 26
  ((char*)&serial)[0] = 0;
#elif CASE == 27
  pthread_mutex_trylock(&serial);
#elif CASE == 28
  unrelated++;
#elif CASE == 29
  cells[published] = 3;
#elif CASE == 30
  pthread_mutex_lock(&serial);
#endif
  for (;;) touch();
  return 0;
}
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, thread, 0);
  pthread_create(&b, 0, thread, 0);
  return 0;
}
