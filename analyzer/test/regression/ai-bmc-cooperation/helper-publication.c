#include <pthread.h>
#include <stdint.h>
#ifndef CASE
#define CASE 0
#endif
extern int __VERIFIER_nondet_int(void);
extern void unknown_sink(void *);
struct payload { int selector, value; };
static struct payload *published;
static struct payload other;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
void publish(struct payload *p) {
#if CASE == 3
  if (__VERIFIER_nondet_int()) return;
#endif
  published = p;
#if CASE == 12
  p = &other;
  p->selector = 0;
#endif
}
#if CASE == 1
void invoke(struct payload *p) { publish(p); }
#endif
static void *left(void *unused) {
#if CASE == 7
  pthread_mutex_lock(&mutex);
#endif
#if CASE == 5 || CASE == 13
  if (published->selector == 0) published->value = 1;
#elif CASE == 8
  published->value = published->value + 1;
#elif CASE == 9
  *((unsigned char *)&published->value + 1) = 1;
#else
  published->value = 1;
#endif
#if CASE == 7
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}
static void *right(void *unused) {
#if CASE == 7
  pthread_mutex_lock(&mutex);
#endif
#if CASE == 5 || CASE == 13
  if (published->selector != 0) published->value = 2;
#elif CASE == 6
  published->selector = 2;
#else
  published->value = 2;
#endif
#if CASE == 7
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}
int main(void) {
  struct payload data;
  data.selector = 0; data.value = 0;
  other.selector = 0; other.value = 0;
#if CASE == 13
  data.selector = __VERIFIER_nondet_int();
#endif
  pthread_t a, b;
#if CASE == 4
  pthread_create(&a, 0, left, 0);
#endif
#if CASE == 1
  invoke(&data);
#elif CASE == 2
  publish(&data); publish(&other);
#elif CASE == 11
  for (int i = 0; i < 2; ++i) publish(&data);
#elif CASE == 14
  publish(&other);
#else
  publish(&data);
#endif
#if CASE == 10
  unknown_sink(&published);
#endif
#if CASE != 4
  pthread_create(&a, 0, left, 0);
#endif
  pthread_create(&b, 0, right, 0);
  pthread_join(a, 0); pthread_join(b, 0);
  return 0;
}
