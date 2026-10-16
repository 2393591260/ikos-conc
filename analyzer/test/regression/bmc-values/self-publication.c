#include <pthread.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
#ifndef WIDE
#define WIDE 0
#endif
#ifndef OPT
#define OPT 0
#endif
#ifndef LENGTH
#define LENGTH 6
#endif
#ifndef STEP
#define STEP 2
#endif
#if WIDE
typedef long long datum;
#else
typedef int datum;
#endif
extern int __VERIFIER_nondet_int();
static volatile datum state = -7;
#if CASE == 5
static datum cells[LENGTH] = {0, 1};
#else
static datum cells[LENGTH];
#endif
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
#if CASE == 6
static pthread_mutex_t other = PTHREAD_MUTEX_INITIALIZER;
#endif
extern void unknown_effect(void);
static void require(int c) {
#if CASE != 12
  if (!c) abort();
#endif
}
#if CASE == 24 || CASE == 25 || CASE == 26 || CASE == 29 || CASE == 30
static void consume(int offset, int extra) {
#else
static void consume(int offset) {
#endif
#if CASE == 7
  datum early = state;
  (void)early;
#endif
#if CASE == 9
  unknown_effect();
#endif
  datum best = -7;
#if CASE == 18
  datum observed = -7;
#endif
#if CASE == 19
  int limit = offset;
#else
  int limit = offset + STEP;
#endif
  for (int i = offset; i < limit; ++i) {
#if CASE == 14
    datum sample = cells[i + LENGTH];
#elif CASE == 21
    (void)*(volatile datum *)&cells[i];
    datum sample = 0;
#else
    datum sample = cells[i];
#endif
#if OPT
    if (sample > best) best = sample;
#else
#if CASE == 6
    if (offset == 0) pthread_mutex_lock(&other);
    else pthread_mutex_lock(&gate);
#else
    pthread_mutex_lock(&gate);
#endif
#if CASE == 18
    if (i == offset) observed = state;
    if (sample > observed)
#elif CASE == 1
    if (sample >= state)
#else
    if (sample > state)
#endif
#if CASE == 2
      state = sample + 1;
#else
#if CASE == 25
      state = sample + (offset == LENGTH - STEP && extra == 1);
#else
      state = sample;
#endif
#endif
#if CASE == 3
    state = sample;
#endif
#if CASE == 6
    if (offset == 0) pthread_mutex_unlock(&other);
    else pthread_mutex_unlock(&gate);
#else
    pthread_mutex_unlock(&gate);
#endif
#if CASE == 8
    state = -7;
#endif
    datum after = state;
    (void)after;
#endif
  }
#if OPT
  pthread_mutex_lock(&gate);
#if CASE == 1
  if (best >= state)
#else
  if (best > state)
#endif
#if CASE == 2
    state = best + 1;
#else
#if CASE == 25
    state = best + (offset == LENGTH - STEP && extra == 1);
#else
    state = best;
#endif
#endif
#if CASE == 3
  state = best;
#endif
  pthread_mutex_unlock(&gate);
#if CASE == 8
  state = -7;
#endif
  datum after = state;
  (void)after;
#endif
#if CASE == 4
  cells[0] = 1;
#endif
}
static void *entry(void *ignored) {
  int offset = __VERIFIER_nondet_int();
#if CASE == 11
  int checked = __VERIFIER_nondet_int();
#else
  int checked = offset;
#endif
#if CASE == 10
  require(checked % STEP == 0 && checked >= 0);
#elif CASE == 13
  require(checked >= 0 && checked < LENGTH);
#elif CASE == 15 || CASE == 28
  require(checked % STEP == 0 && checked >= 0 && checked <= LENGTH);
#elif CASE == 20
  require(checked >= 0 && checked < 100000);
#else
  require(checked % STEP == 0 && checked >= 0 && checked < LENGTH);
#endif
#if CASE == 16
  offset++;
#endif
#if CASE == 24 || CASE == 25
  int extra = __VERIFIER_nondet_int();
  require(0 <= extra && extra < 2);
  consume(offset, extra);
#elif CASE == 26
  consume(offset, offset);
#elif CASE == 29 || CASE == 30
  int extra = __VERIFIER_nondet_int();
  require(offset == 0 && 0 <= extra && extra < (CASE == 29 ? 65 : 64));
  consume(offset, extra);
#else
  consume(offset);
#endif
  return 0;
}
int main(void) {
  pthread_t output;
#if CASE == 17
  cells[0] = 0;
#endif
#if CASE == 22
  state = -7;
#endif
  while (1) {
#if CASE == 23
    pthread_create(&output, 0, entry, (void *)1);
#else
    pthread_create(&output, 0, entry, 0);
#endif
  }
}
