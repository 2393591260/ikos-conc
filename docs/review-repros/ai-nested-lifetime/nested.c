#include <pthread.h>
#ifndef CASE
#define CASE 0
#endif
static int payload;
static void *child(void *unused) { (void)unused; payload = 1; return 0; }
#if CASE == 4 || CASE == 8
__attribute__((noinline)) static void make_child(unsigned remaining) {
  pthread_t thread;
  pthread_create(&thread, 0, child, 0);
#if CASE == 8
  if (remaining) make_child(remaining - 1);
#else
  (void)remaining;
#endif
  pthread_join(thread, 0);
}
#endif
static void *parent(void *unused) {
  (void)unused;
#if CASE == 4
  make_child(0); make_child(0);
#elif CASE == 8
  make_child(1);
#elif CASE == 3 || CASE == 6
  pthread_t thread;
  for (unsigned i = 0; i < 2; ++i) {
    pthread_create(&thread, 0, child, 0);
#if CASE == 6
    pthread_join(thread, 0);
#endif
  }
#if CASE == 3
  pthread_join(thread, 0);
#endif
#elif CASE == 5
  pthread_t left, right;
  pthread_create(&left, 0, child, 0);
  pthread_create(&right, 0, child, 0);
  pthread_join(left, 0); pthread_join(right, 0);
#else
  pthread_t thread;
  pthread_create(&thread, 0, child, 0);
#if CASE != 1
  pthread_join(thread, 0);
#endif
#endif
  return 0;
}
#if CASE == 9
__attribute__((noinline)) static void factory(pthread_t *slot) {
  pthread_create(slot, 0, parent, 0);
}
#endif
#if CASE == 11
static void *(*callback)(void *) = parent;
#endif
#if CASE == 12
static void *ancestor(void *unused) {
  (void)unused;
  pthread_t thread;
  pthread_create(&thread, 0, parent, 0);
  pthread_join(thread, 0);
  return 0;
}
#endif
int main(void) {
  pthread_t thread;
#if CASE == 9
  factory(&thread);
#elif CASE == 11
  pthread_create(&thread, 0, callback, 0);
#elif CASE == 12
  pthread_create(&thread, 0, ancestor, 0);
#else
  pthread_create(&thread, 0, parent, 0);
#endif
#if CASE == 2
  pthread_t other;
  pthread_create(&other, 0, parent, 0);
  pthread_join(other, 0);
#elif CASE == 10
  parent(0);
#endif
  pthread_join(thread, 0);
  payload = 2;
  return 0;
}
