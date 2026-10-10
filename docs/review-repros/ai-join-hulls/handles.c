#include <pthread.h>

#ifndef CASE
#define CASE 0
#endif
extern int __VERIFIER_nondet_int(void);
static int value;
static void *inert(void *unused) { return 0; }
static void *writer(void *unused) { value = 1; return 0; }

int main(void) {
  pthread_t a, b, chosen;
  pthread_create(&a, 0, inert, 0);
  pthread_create(&b, 0, writer, 0);
#if CASE == 0
  chosen = 0;
  if (__VERIFIER_nondet_int()) chosen = a;
  else if (__VERIFIER_nondet_int()) chosen = b;
#elif CASE == 1
  /* All executions join a valid thread; one can leave writer running. */
  chosen = __VERIFIER_nondet_int() ? a : b;
#elif CASE == 2
  chosen = a;
#elif CASE == 3
  chosen = b;
#elif CASE == 4
  /* The memory effect remains after joining both possible targets. */
  pthread_join(a, 0);
  pthread_join(b, 0);
  value = 2;
  return 0;
#else
  chosen = 0;
  if (__VERIFIER_nondet_int()) chosen = b;
#endif
  pthread_join(chosen, 0);
  value = 2;
  return 0;
}
