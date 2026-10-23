// Purpose-built relation controls. MODE 0: local branch reconstruction;
// MODE 1: same-critical-section pair; MODE 2: cross-thread pair handoff.
// BROKEN preserves a reachable payload race; this is not a benchmark score.
#include <pthread.h>

#ifndef MODE
#define MODE 0
#endif
#ifndef BROKEN
#define BROKEN 0
#endif

extern int __VERIFIER_nondet_int(void);
static pthread_mutex_t mu;
static int payload;

#if MODE == 0
static struct { int lo, hi; } state = {3, 11};
#else
static struct { int x, y; } state = {0, 0};
#endif

#if MODE != 1
static void *writer(void *unused) {
  (void)unused;
#if MODE == 0
  int v = __VERIFIER_nondet_int() ? 2 : 5;
#else
  int v = __VERIFIER_nondet_int() ? 1 : 2;
#endif
  pthread_mutex_lock(&mu);
#if MODE == 0
  state.lo = BROKEN ? 11 : v;
  state.hi = v + 8;
#else
  state.x = v;
  state.y = BROKEN ? 3 - v : v;
#endif
  pthread_mutex_unlock(&mu);
  return 0;
}
#endif

static void *reader(void *unused) {
  (void)unused;
  pthread_mutex_lock(&mu);
#if MODE == 0
  int k, x;
  if (__VERIFIER_nondet_int()) {
    k = 0;
    x = state.lo;
  } else {
    k = 1;
    x = state.hi;
  }
  pthread_mutex_unlock(&mu);
  int delta = x - 8 * k;
  if (delta < 0 || delta > 7) payload++;
#else
#if MODE == 1
  int v = __VERIFIER_nondet_int() ? 1 : 2;
  state.x = v;
  state.y = BROKEN ? 3 - v : v;
#endif
  int a = state.x;
  int b = state.y;
  pthread_mutex_unlock(&mu);
  if (a != b) payload++;
#endif
  return 0;
}

int main(void) {
  pthread_t a, b;
#if MODE != 1
  pthread_t producer;
#endif
  pthread_mutex_init(&mu, 0);
#if MODE != 1
  pthread_create(&producer, 0, writer, 0);
#endif
  pthread_create(&a, 0, reader, 0);
  pthread_create(&b, 0, reader, 0);
#if MODE != 1
  pthread_join(producer, 0);
#endif
  pthread_join(a, 0);
  pthread_join(b, 0);
  pthread_mutex_destroy(&mu);
  return 0;
}
