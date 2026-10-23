// Integer aggregate leaves must use the current protected value, not a name
// heuristic, a neighboring leaf, or the final value of a critical section.
#include <pthread.h>
#include <string.h>

#ifndef CASE
#define CASE 0
#endif

#if CASE == 4
typedef unsigned int scalar;
#define EXPECTED 4045620583U
#elif CASE == 5
typedef long long scalar;
#define EXPECTED (-4294967297LL)
#elif CASE == 6
typedef unsigned long long scalar;
#define EXPECTED 18364758544493064720ULL
#elif CASE == 3
typedef int scalar;
#define EXPECTED (-7)
#else
typedef int scalar;
#define EXPECTED 1
#endif

#if CASE == 7
#define INITIAL 2
#elif CASE == 16 || CASE == 17
#define INITIAL 3
#elif CASE == 18
#define INITIAL 1
#else
#define INITIAL 0
#endif

struct fields {
  int prefix;
  scalar value;
  scalar neighbor;
  scalar slots[2];
};

#if CASE == 14
static volatile struct fields state = {7, INITIAL, 9, {0, 0}};
#else
static struct fields state = {7, INITIAL, 9, {0, 0}};
#endif
static pthread_mutex_t mu, nu;
static int payload;

#if CASE == 15 || CASE == 19 || CASE == 20
extern int __VERIFIER_nondet_int(void);
#endif

#if CASE == 11
__attribute__((noinline)) static void save_through_pointer(scalar *destination) {
  *destination = EXPECTED;
}
#endif

static void *worker(void *argument) {
  (void)argument;
  scalar observed;
#if CASE == 2
  scalar adjacent;
#endif

#if CASE != 10
  pthread_mutex_lock(&mu);
#endif

#if CASE == 7
  state.value = 2;
  pthread_mutex_unlock(&mu);
  pthread_mutex_lock(&mu);
  observed = state.value;
#elif CASE == 8
  state.value = 8;
  observed = state.value;
  state.value = 0;
#elif CASE == 11
  save_through_pointer(&state.value);
  observed = state.value;
#elif CASE == 12
  state.value = 1;
  *(unsigned char *)&state.value = 0;
  observed = state.value;
#elif CASE == 13
  state.value = 1;
  memset(&state.value, 0, sizeof(state.value));
  observed = state.value;
#elif CASE == 15
  int index = __VERIFIER_nondet_int() != 0;
  state.slots[index] = 1;
  observed = state.slots[0];
#elif CASE == 16 || CASE == 17 || CASE == 18 || CASE == 25 || CASE == 26
  // CASE25/26 consume another thread's write; these readers never store first.
  observed = state.value;
#elif CASE == 19 || CASE == 20
  if (__VERIFIER_nondet_int()) {
    state.value = 1;
  } else {
#if CASE == 19
    state.value = 2;
#else
    state.value = 1;
#endif
  }
  observed = state.value;
#elif CASE == 21
  struct fields replacement = {7, 0, 9, {0, 0}};
  state.value = 1;
  state = replacement;
  observed = state.value;
#elif CASE == 22
  // Both locks intersect each writer's lockset separately.  There is no one
  // mutex shared by every writer and this read; that is not a value proof.
  pthread_mutex_lock(&nu);
  observed = state.value;
  pthread_mutex_unlock(&nu);
#elif CASE == 24
  state.value = EXPECTED;
  // Acquiring nu must retain the mutex mu that was already held while waiting.
  pthread_mutex_lock(&nu);
  observed = state.value;
  pthread_mutex_unlock(&nu);
#else
  state.value = EXPECTED;
#if CASE == 2
  state.neighbor = 9;
  adjacent = state.neighbor;
#endif
  observed = state.value;
#endif

#if CASE != 10
  pthread_mutex_unlock(&mu);
#endif

#if CASE == 1
  if (observed == EXPECTED) payload++;
#elif CASE == 2
  if (observed != EXPECTED || adjacent != 9) payload++;
#elif CASE == 7
  if (observed != 2) payload++;
#elif CASE == 8
  if (observed == 8) payload++;
#elif CASE == 16
  if (observed != 3) payload++;
#elif CASE == 17
  if (observed == 3) payload++;
#elif CASE == 22
  // UNKNOWN is the oracle.  This fixture does not assert that C permits an
  // actual torn read with this value on the selected target.
  if (observed == 511) payload++;
#elif CASE == 25
  if (observed == 1) payload++;
#elif CASE == 26
  if (observed < 0 || observed > 1) payload++;
#else
  if (observed != EXPECTED) payload++;
#endif
  return 0;
}

#if CASE == 9 || CASE == 10 || CASE == 22 || CASE == 23 || CASE == 25 || CASE == 26
static void *other_writer(void *argument) {
  (void)argument;
#if CASE == 25 || CASE == 26
  pthread_mutex_lock(&mu);
#elif CASE != 10
  pthread_mutex_lock(&nu);
#endif
#if CASE == 22
  state.value = 256;
#elif CASE == 23 || CASE == 25 || CASE == 26
  state.value = EXPECTED;
#else
  state.value = 2;
#endif
#if CASE == 25 || CASE == 26
  pthread_mutex_unlock(&mu);
#elif CASE != 10
  pthread_mutex_unlock(&nu);
#endif
  return 0;
}
#endif

#if CASE == 22
static void *first_writer(void *argument) {
  (void)argument;
  pthread_mutex_lock(&mu);
  state.value = 255;
  pthread_mutex_unlock(&mu);
  return 0;
}
#endif

int main(void) {
  pthread_t a, b;
#if CASE == 9 || CASE == 10 || CASE == 23 || CASE == 25 || CASE == 26
  pthread_t c;
#elif CASE == 22
  pthread_t c, d;
#endif
  pthread_mutex_init(&mu, 0);
  pthread_mutex_init(&nu, 0);
#if CASE == 22
  pthread_create(&c, 0, first_writer, 0);
  pthread_create(&d, 0, other_writer, 0);
#elif CASE == 9 || CASE == 10 || CASE == 25 || CASE == 26
  pthread_create(&c, 0, other_writer, 0);
#endif
  pthread_create(&a, 0, worker, 0);
#if CASE == 23
  // New writer, same scalar value, different mutex: discovery must still dirty
  // the fixpoint and revoke field refinement instead of retaining an old gate.
  pthread_create(&c, 0, other_writer, 0);
#endif
#if CASE == 18
  // This happens after a worker is live: its initial value is not permanent.
  pthread_mutex_lock(&mu);
  state.value = 2;
  pthread_mutex_unlock(&mu);
#endif
  pthread_create(&b, 0, worker, 0);
  pthread_join(a, 0);
  pthread_join(b, 0);
#if CASE == 9 || CASE == 10 || CASE == 22 || CASE == 23 || CASE == 25 || CASE == 26
  pthread_join(c, 0);
#endif
#if CASE == 22
  pthread_join(d, 0);
#endif
  pthread_mutex_destroy(&mu);
  pthread_mutex_destroy(&nu);
  return 0;
}
