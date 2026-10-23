// Joint handoff controls: all verdicts follow values/locks, not source names.
#include <pthread.h>
#ifndef VARIANT
#define VARIANT 0
#endif
#if VARIANT == 19 || VARIANT == 20
typedef long long datum;
#else
typedef int datum;
#endif
extern int __VERIFIER_nondet_int(void);
extern void unknown_effect(void);
static pthread_mutex_t mu, nu;
#if VARIANT == 27
static pthread_mutexattr_t attr;
#endif
#if VARIANT == 18
static struct { volatile datum x, y; } state;
#elif VARIANT == 2
static struct { datum x, y; } state = {0, 1};
#elif VARIANT == 28
static struct { datum x, y; } state = {1, 1};
#elif VARIANT == 24
static struct { int x; long long y; } state;
#elif VARIANT == 25
static struct { int x; unsigned y; } state;
#elif VARIANT == 26
static struct { datum x, y, z; } state;
#else
static struct { datum x, y; } state;
#endif
static int payload;

static void save(datum *p, datum v) { *p = v; }
static void *writer(void *unused) {
  (void)unused;
  datum v = __VERIFIER_nondet_int() ? 1 : 2;
#if VARIANT == 3
  pthread_mutex_lock(&nu);
#elif VARIANT != 4
  pthread_mutex_lock(&mu);
#endif
  state.x = v;
#if VARIANT == 11
  pthread_mutex_lock(&nu);
  pthread_mutex_unlock(&nu); // Not a release of the protecting mutex.
#endif
#if VARIANT == 1 || VARIANT == 20
  state.y = 3 - v;
#elif VARIANT == 5
  *((unsigned char *)&state.y) = 7;
#elif VARIANT == 6
  save(&state.y, 3 - v);
#elif VARIANT == 15
  // The sole updated field must not establish a joint release invariant.
#elif VARIANT == 16
  state.y = __VERIFIER_nondet_int() ? v : 3 - v;
#elif VARIANT == 23
  unknown_effect();
  state.y = v;
#else
  state.y = v;
#endif
#if VARIANT == 26
  state.z = v;
#endif
#if VARIANT == 3
  pthread_mutex_unlock(&nu);
#elif VARIANT != 4
  pthread_mutex_unlock(&mu);
#endif
  return 0;
}

#if VARIANT == 14
static void *late_writer(void *unused) {
  (void)unused;
  datum v = __VERIFIER_nondet_int() ? 1 : 2;
  pthread_mutex_lock(&mu);
  state.x = v;
  state.y = 3 - v; // Same marginal ranges, different joint states.
  pthread_mutex_unlock(&mu);
  return 0;
}
#endif

static void *reader(void *unused) {
  (void)unused;
#if VARIANT == 27
  if (__VERIFIER_nondet_int()) {
    pthread_mutex_lock(&mu);
    state.x = 7;
  }
  // A merge loses MUST-held mu; ERRORCHECK can fail without a new handoff.
#endif
#if VARIANT == 13
  pthread_mutex_lock(&nu);
#endif
#if VARIANT == 22
  pthread_mutex_trylock(&mu);
#else
  pthread_mutex_lock(&mu);
#endif
#if VARIANT == 21
  pthread_mutex_lock(&mu);
#endif
#if VARIANT == 9 || VARIANT == 10
  state.x = 7;
#if VARIANT == 10
  pthread_mutex_lock(&nu); // Must not restore x=y over our intermediate write.
#endif
#endif
  datum a = state.x;
#if VARIANT == 8
  pthread_mutex_unlock(&mu);
  pthread_mutex_lock(&mu);
#endif
  datum b = state.y;
#if VARIANT == 26
  datum c = state.z;
#endif
#if VARIANT == 9 || VARIANT == 10
  state.y = 7;
#if VARIANT == 10
  pthread_mutex_unlock(&nu);
#endif
#endif
#if VARIANT == 27
  state.y = state.x; // Preserve release equality without erasing the bad read.
#endif
  pthread_mutex_unlock(&mu);
#if VARIANT == 13
  pthread_mutex_unlock(&nu);
#endif
#if VARIANT == 7
  b = 3 - b;
#endif
#if VARIANT == 12
  pthread_mutex_lock(&mu);
  a = state.x;
  b = state.y;
  pthread_mutex_unlock(&mu);
#endif
#if VARIANT == 26
  if (a != b || b != c) payload++;
#else
  if (a != b) payload++;
#endif
  return 0;
}

int main(void) {
  pthread_t w, a, b;
#if VARIANT == 27
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
  pthread_mutex_init(&mu, &attr);
  pthread_mutex_init(&nu, &attr);
  pthread_mutexattr_destroy(&attr);
#else
  pthread_mutex_init(&mu, 0);
  pthread_mutex_init(&nu, 0);
#endif
  pthread_create(&w, 0, writer, 0);
  pthread_create(&a, 0, reader, 0);
  pthread_create(&b, 0, reader, 0);
#if VARIANT == 14
  pthread_t late;
  pthread_create(&late, 0, late_writer, 0);
  pthread_join(late, 0);
#endif
  pthread_join(w, 0);
  pthread_join(a, 0);
  pthread_join(b, 0);
#if VARIANT != 27
  pthread_mutex_destroy(&nu);
  pthread_mutex_destroy(&mu);
#endif
  return 0;
}
