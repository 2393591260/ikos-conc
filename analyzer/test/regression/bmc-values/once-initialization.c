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
pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t data_lock = PTHREAD_MUTEX_INITIALIZER;
#if CASE == 11
int state = 1;
#else
int state = 0;
#endif
datum payload;
int unrelated, seen;
extern int __VERIFIER_nondet_int(void);
static void initialize(void) {
#if CASE == 12
  ((char*)&payload)[0] = 0;
#elif CASE == 13
  if (__VERIFIER_nondet_int()) payload = 0;
#else
  payload = 7;
#endif
}
void unused_init(void) { initialize(); }
static void later(void) {
#if CASE != 1
  pthread_mutex_lock(&data_lock);
#endif
  payload++;
#if CASE != 1
  pthread_mutex_unlock(&data_lock);
#endif
}
static void leaf(void) { later(); }
static void pass(void) {
#if CASE == 9
  int snapshot = state;
#endif
  pthread_mutex_lock(&gate);
#if CASE == 9
  switch (snapshot) {
#else
  switch (state) {
#endif
    case 0:
#if CASE == 7
      state = 1;
      pthread_mutex_unlock(&gate);
      initialize();
      return;
#elif CASE == 8
      state = 1;
      initialize();
#elif CASE == 18
      pthread_mutex_unlock(&gate);
      initialize();
      pthread_mutex_lock(&gate);
      state = 1;
#else
      initialize();
      state = 1;
#endif
    case 1:
      pthread_mutex_unlock(&gate);
      break;
  }
}
static void* thread(void* ignored) {
#if CASE == 2
  later();
#elif CASE == 3
  if (__VERIFIER_nondet_int()) { later(); return 0; }
#elif CASE == 4
  if (__VERIFIER_nondet_int()) goto after_gate;
#elif CASE == 5
  for (int i = 0; i < 2; i++) { later(); pass(); }
#elif CASE == 14
  state = 1;
#elif CASE == 15
  pthread_mutex_destroy(&gate);
#elif CASE == 16
  ((char*)&gate)[0] = 0;
#elif CASE == 17
  pthread_mutex_unlock(&gate);
#endif
#if CASE == 6
  for (;;) { pass(); leaf(); }
#else
  pass();
#endif
#if CASE == 4
after_gate:
#endif
#if CASE == 10
  state = 0;
#elif CASE == 19
  unused_init();
#elif CASE == 20
  unrelated++;
#elif CASE == 21
  pthread_mutex_lock(&gate);
#elif CASE == 22
  payload = 7; /* Even a redundant same-value later write can race. */
#endif
  leaf();
  return 0;
}
#if CASE == 23 || CASE == 24
static void* second(void* ignored) {
#if CASE == 23
  pass();
#endif
  leaf(); return 0;
}
#endif
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, thread, 0);
#if CASE == 23 || CASE == 24
  pthread_create(&b, 0, second, 0);
#else
  pthread_create(&b, 0, thread, 0);
#endif
  return 0;
}
