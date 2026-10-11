#include <pthread.h>
#include <stdint.h>
#include <limits.h>
extern int __VERIFIER_nondet_int(void);
extern unsigned __VERIFIER_nondet_uint(void);
extern unsigned long __VERIFIER_nondet_ulong(void);
extern unsigned long long __VERIFIER_nondet_ulonglong(void);
#ifndef CASE
#define CASE 0
#endif
static int payload;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static void *worker(void *arg) {
  (void)arg;
  pthread_mutex_lock(&mutex);
  payload = 1;
  pthread_mutex_unlock(&mutex);
  return 0;
}
__attribute__((noinline)) static void conditional(unsigned value, int acquire) {
  if (acquire) { if (value) pthread_mutex_lock(&mutex); }
  unsigned shifted = value + 1;
  if (shifted - 1) payload = 2;
}
int main(void) {
  pthread_t thread;
  unsigned value = __VERIFIER_nondet_uint();
  pthread_create(&thread, 0, worker, 0);
#if CASE == 0
  int signed_value = __VERIFIER_nondet_int();
  if (signed_value == INT_MAX) return 0;
  if (signed_value) pthread_mutex_lock(&mutex);
  int shifted = signed_value + 1;
  if (shifted - 1) payload = 2;
  if (signed_value) pthread_mutex_unlock(&mutex);
#elif CASE == 1
  if (value) pthread_mutex_lock(&mutex);
  unsigned shifted = value + 77;
  if (shifted - 77) payload = 2;
  if (value) pthread_mutex_unlock(&mutex);
#elif CASE == 2
  unsigned char narrow = value;
  if (narrow) pthread_mutex_lock(&mutex);
  unsigned char shifted = narrow + 1;
  if ((unsigned char)(shifted - 1)) payload = 2;
  if (narrow) pthread_mutex_unlock(&mutex);
#elif CASE == 3
  if (value) pthread_mutex_lock(&mutex);
  if (value + 1) payload = 2;
  if (value) pthread_mutex_unlock(&mutex);
#elif CASE == 4
  if (value) pthread_mutex_lock(&mutex);
  unsigned shifted = 9 + value;
  if (shifted - 9) payload = 2;
  if (value) pthread_mutex_unlock(&mutex);
#elif CASE == 5
  uint64_t wide = __VERIFIER_nondet_ulonglong();
  if (wide) pthread_mutex_lock(&mutex);
  uint64_t shifted = wide + UINT64_C(0x8000000000000000);
  if (shifted - UINT64_C(0x8000000000000000)) payload = 2;
  if (wide) pthread_mutex_unlock(&mutex);
#elif CASE == 6
  if (value) pthread_mutex_lock(&mutex);
  unsigned shifted = value - 1;
  uint64_t widened = shifted;
  if (widened + 1) payload = 2; // value=0: widened+1 is 2^32
  if (value) pthread_mutex_unlock(&mutex);
#elif CASE == 7
  if (value) pthread_mutex_lock(&mutex);
  unsigned char narrowed = value - 1;
  if ((unsigned)narrowed + 1) payload = 2; // value=0: 256
  if (value) pthread_mutex_unlock(&mutex);
#elif CASE == 8
  int signed_value = __VERIFIER_nondet_int();
  if (signed_value == INT_MIN) return 0;
  if (signed_value) pthread_mutex_lock(&mutex);
  unsigned shifted = (unsigned)(signed_value - 1);
  if (shifted + 1) payload = 2;
  if (signed_value) pthread_mutex_unlock(&mutex);
#elif CASE == 9
  conditional(value, 1);
  conditional(1, 0); // value=0: second activation has no mutex
  if (value) pthread_mutex_unlock(&mutex);
#elif CASE == 10
  unsigned held = value;
  for (unsigned step = 0; step < 2; ++step) {
    if (step == 0) { if (value) pthread_mutex_lock(&mutex); }
    else { value = 1; unsigned shifted = value + 1; if (shifted - 1) payload = 2; }
  }
  if (held) pthread_mutex_unlock(&mutex);
#elif CASE == 11
  unsigned held = value;
  if (value) pthread_mutex_lock(&mutex);
  value = value ? 0 : 1;
  if ((value + 1) - 1) payload = 2;
  if (held) pthread_mutex_unlock(&mutex);
#elif CASE == 12
  unsigned shifted = value - 19;
  if (shifted + 19) pthread_mutex_lock(&mutex);
  if (value) payload = 2;
  if (shifted + 19) pthread_mutex_unlock(&mutex);
#elif CASE == 13
  uint64_t wide = __VERIFIER_nondet_ulonglong();
  if (wide) pthread_mutex_lock(&mutex);
  uint64_t shifted = wide + UINT64_C(0x100000001);
  if (shifted - 1) payload = 2; // wide=0: 2^32, not zero at width64
  if (wide) pthread_mutex_unlock(&mutex);
#elif CASE == 14
  unsigned long wide = __VERIFIER_nondet_ulong();
  if (wide) pthread_mutex_lock(&mutex);
  unsigned long shifted = wide + (1UL << 31);
  if (shifted - (1UL << 31)) payload = 2;
  if (wide) pthread_mutex_unlock(&mutex);
#elif CASE == 15
  if (value * 2U) pthread_mutex_lock(&mutex);
  if (value) payload = 2; // value=2^31: multiplication wraps to zero
  if (value * 2U) pthread_mutex_unlock(&mutex);
#endif
  pthread_join(thread, 0);
  return 0;
}
