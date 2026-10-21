#include <pthread.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long number;
#else
typedef int number;
#endif
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
struct State { volatile number index; number data[4]; volatile int marker; };
static number identity(number x) { return x; }
void *reader(void *arg) {
  struct State *s = arg;
#if CASE == 8
  pthread_mutex_lock(&mutex);
#endif
  number index = s->index;
#if CASE == 4
  long long wide = (1LL << 32) + index;
  index = (int)wide;
#elif CASE == 9
  index = identity(index);
#endif
  number *p = s->data;
#if CASE == 10
  p = p + 4; p = p - 4;
#elif CASE == 11
  p = p + 16; p = p - 16;
#endif
  number value = p[index];
  if (value == 1) s->marker = 2;
#if CASE == 8
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}
int main(void) {
  pthread_t t;
#if CASE == 12
  struct State *s = malloc(sizeof(*s));
  if (!s) return 0;
#else
  struct State storage;
  struct State *s = &storage;
#endif
  struct State other;
#if CASE == 1
  s->index = 1;
#elif CASE == 2
  s->index = -100;
#elif CASE == 3
  s->index = 100;
#elif CASE == 5
  s->index = 7; s->index = 0;
#else
  s->index = 0;
#endif
  s->marker = 0;
  pthread_create(&t, 0, reader, s);
#if CASE == 8
  pthread_mutex_lock(&mutex);
#endif
#if CASE == 1
  s->data[1] = 1;
#elif CASE == 6
  other.data[0] = 1;
#elif CASE == 7
  s->data[0] = 0;
#else
  s->data[0] = 1;
#endif
  s->marker = 1;
#if CASE == 8
  pthread_mutex_unlock(&mutex);
#endif
  pthread_join(t, 0);
}
