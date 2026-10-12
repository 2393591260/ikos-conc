// Closed readonly pointer arrays versus escaped, byte-written and late writers.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#ifndef CASE
#define CASE 0
#endif
#if CASE == 7
typedef long long payload;
#else
typedef int payload;
#endif
#if CASE == 6
static struct { payload *items[4]; payload *extra; } storage;
#define slots storage.items
#else
static payload *slots[4];
#endif
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t other = PTHREAD_MUTEX_INITIALIZER;
extern int __VERIFIER_nondet_int(void);
static void require_index(int i) {
  if (i < 0 || i >= 4) abort();
}
static void replace(payload **address) {
  *address = malloc(sizeof(payload));
}
static void *worker(void *arg) {
  int i = __VERIFIER_nondet_int();
  require_index(i);
#if CASE != 1
#if CASE == 9
  pthread_mutex_lock(&other);
#else
  pthread_mutex_lock(&lock);
#endif
#endif
  *slots[i] = 2;
#if CASE != 1
#if CASE == 9
  pthread_mutex_unlock(&other);
#else
  pthread_mutex_unlock(&lock);
#endif
#endif
  return arg;
}
int main(void) {
  pthread_t id;
  for (int k = 0; k < 4; ++k) {
    slots[k] = malloc(sizeof(payload));
    if (!slots[k]) abort();
#if CASE != 5
    *slots[k] = 1;
#endif
  }
#if CASE == 2
  ((unsigned char *)slots)[0] = 0;
#elif CASE == 4
  replace(&slots[0]); // address escapes to a helper writer
#elif CASE == 8
  memcpy(&slots[0], &slots[1], sizeof(slots[0]));
#elif CASE == 9
  slots[1] = slots[0]; // different protecting mutexes do not separate aliases
#endif
  pthread_create(&id, 0, worker, 0);
#if CASE == 5
  for (int k = 0; k < 4; ++k) *slots[k] = 1; // late, unprotected initialization
#endif
#if CASE == 3
  slots[0] = malloc(sizeof(int)); // races with a slot read
#endif
  int j = __VERIFIER_nondet_int();
  require_index(j);
#if CASE != 1
  pthread_mutex_lock(&lock);
#endif
  *slots[j] = 3;
#if CASE != 1
  pthread_mutex_unlock(&lock);
#endif
  return 0;
}
