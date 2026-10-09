// BMC address/value regressions. CASE selects the semantic case, not analyzer rules.
#include <pthread.h>

#ifndef CASE
#define CASE 0
#endif

#if CASE == 14
extern int __VERIFIER_nondet_int();
#else
extern int __VERIFIER_nondet_int(void);
#endif
extern unsigned int __VERIFIER_nondet_uint(void);
extern unsigned long long __VERIFIER_nondet_ulonglong(void);
extern int unrelated_nondet_index(void);

int data[2];
volatile int shared_index;

static void write_data(void) {
#if CASE == 2
  unsigned int i = __VERIFIER_nondet_uint();
#elif CASE == 3
  int i = shared_index;
#elif CASE == 4
  int i = unrelated_nondet_index();
#elif CASE == 5
  int i = __VERIFIER_nondet_int();
  int j = __VERIFIER_nondet_int();
  if (i != 0 || j != 0)
    return;
  data[0] = 1;
  return;
#elif CASE == 7
  unsigned long long value = __VERIFIER_nondet_ulonglong();
  if (value != 18446744073709551615ULL)
    return;
  data[0] = 1;
  return;
#elif CASE == 10
  int i = unrelated_nondet_index();
  if (i != 0)
    return;
  data[0] = 1;
  return;
#elif CASE == 12
  unsigned int bits = (unsigned int)__VERIFIER_nondet_int();
  unsigned long long widened = bits;
  if (widened != 4294967295ULL)
    return;
  data[0] = 1;
  return;
#elif CASE == 8
  unsigned char i = (unsigned char)__VERIFIER_nondet_uint();
#elif CASE == 9
  unsigned int i = __VERIFIER_nondet_uint() + 1U;
#else
  int i = __VERIFIER_nondet_int();
#endif
#if CASE != 5 && CASE != 7 && CASE != 10 && CASE != 12
#if CASE == 1
  if (i != 0)
    return;
#elif CASE == 11
  if (i != 2)
    return;
#else
  if (i < 0 || i >= 2)
    return;
#endif
  data[i] = 1;
#endif
}

static void* worker(void* unused) {
  write_data();
  return 0;
}

int main(void) {
  pthread_t thread;
#if CASE == 3
  shared_index = __VERIFIER_nondet_int();
#endif
  pthread_create(&thread, 0, worker, 0);
#if CASE == 5 || CASE == 7 || CASE == 10 || CASE == 12
  data[0] = 2;
#elif CASE == 11
  data[2] = 2;
#elif CASE == 6
  // Different starting offsets, overlapping byte ranges at data[1].
  ((unsigned char*)data)[5] = 2;
#else
  data[1] = 2;
#endif
  pthread_join(thread, 0);
  return 0;
}
