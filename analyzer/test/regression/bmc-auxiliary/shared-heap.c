#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#ifndef MODE
#define MODE 0
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
extern void escape(void *);
struct cell { datum data; };
struct cell *published;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
#if CASE == 15
volatile int guard_flag;
void check_flag(int value) { if (!value) abort(); }
#endif
void *worker(void *argument) {
#if CASE == 13
  if (argument == 0) return 0;
#endif
#if MODE >= 3
  intptr_t index = (intptr_t)argument;
#if MODE == 3
  index /= 2;
#endif
  struct cell *p = published + index;
#else
  struct cell *p = argument;
#endif
#if CASE == 10
  p = p + 2;
  p = p - 2;
#endif
#if CASE == 11
  pthread_mutex_lock(&lock);
#endif
#if CASE == 15
  pthread_mutex_lock(&lock);
  guard_flag = 1;
  int observed = guard_flag;
  pthread_mutex_unlock(&lock);
  check_flag(observed);
#endif
#if CASE == 6
  *(long long *)p = 7;
#elif CASE == 9
  p->data = p->data + 1;
#else
  p->data = __VERIFIER_nondet_int();
#endif
#if CASE == 11
  pthread_mutex_unlock(&lock);
#endif
  return 0;
}
int main(void) {
#ifdef CONSTANT
  int count = 2;
#else
  int count = __VERIFIER_nondet_int();
#endif
  if (count < 0) abort();
#if CASE == 3
  if (count != 1) abort();
#elif CASE == 2
  if (count != 2) abort();
#endif
  pthread_t *handles = malloc(count * sizeof(*handles));
#if CASE == 6
  struct cell *data = malloc(sizeof(*data));
#else
  struct cell *data = malloc(count * sizeof(*data));
#endif
#if CASE == 2
  struct cell *other = malloc(count * sizeof(*other));
#endif
#if MODE >= 3
  published = data;
#if CASE == 4
  published = data + 1;
#endif
#if CASE == 8
  escape(&published);
#endif
#endif
#if CASE == 7
  escape(data);
#endif
  for (int i = 0; i < count; ++i) {
#if CASE == 5
    if (i == 1) free(data);
#endif
#if MODE == 0
    struct cell *argument = data;
#elif MODE == 1 || MODE == 2
    struct cell *argument = &data[i / 2];
#elif MODE == 3
    void *argument = (void *)(intptr_t)i;
#else
    void *argument = (void *)(intptr_t)(i / 2);
#endif
#if CASE == 1
    argument = &data[i];
#elif CASE == 2
    argument = i == 0 ? data : other;
#elif CASE == 12
  argument = &data[1];
#elif CASE == 13
  argument = i == 0 ? data : 0;
#endif
    pthread_create(&handles[i], 0, worker, argument);
  }
  return 0;
}
