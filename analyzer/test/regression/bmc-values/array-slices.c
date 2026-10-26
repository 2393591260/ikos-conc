// Stable flat-array slices: full frames and counterexamples to their premises.
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef unsigned long long element;
extern unsigned long long __VERIFIER_nondet_ulonglong(void);
#define arbitrary __VERIFIER_nondet_ulonglong
#else
typedef unsigned element;
extern unsigned __VERIFIER_nondet_uint(void);
#define arbitrary __VERIFIER_nondet_uint
#endif
extern int __VERIFIER_nondet_int(void);
extern void opaque(void);
element *input, *output, *other;
int length, cut;
#if CASE == 33
__asm__(".globl unused_assembly_symbol");
#endif
static void bound(int condition) { if (!condition) abort(); }
#if CASE == 44
static long long twice64(long long n) {
  bound(n >= 0 && n <= INT_MAX);
  return n + n; // i64 flagged arithmetic is proved at twice the scalar width.
}
#elif CASE == 45
extern unsigned long long __VERIFIER_nondet_ulonglong(void);
static long long increment64(long long n) { return n + 1; }
#elif CASE == 52
static void change_cut(void) { cut = 0; }
#endif
static element *make(int n) {
  bound(n >= 0);
#if CASE == 30
  int local = -1;
  *(unsigned char *)&local = 0;
  long long widened = local;
  if (widened == 0)
#endif
#if CASE != 5 && CASE != 40 && CASE != 41
  bound((unsigned)n <= UINT32_MAX / sizeof(element));
#endif
#if CASE == 6
  element *p = malloc((unsigned short)(sizeof(element) * (unsigned)n));
#elif CASE == 41
  // LP64 still truncates the actual byte count to 32 bits before malloc.
  element *p = malloc((uint32_t)(sizeof(element) * (size_t)(unsigned)n));
#else
  element *p = malloc(sizeof(element) * (unsigned)n);
#endif
#if CASE == 54
  if (!p) abort();
#elif CASE == 55
  if (!p) { cut = 0; abort(); } // Failure cannot hide this constructor effect.
#endif
#if CASE == 57
  for (long long i = 0; i < n; ++i) p[i] = arbitrary();
#else
  for (int i = 0; i < n; ++i) p[i] = arbitrary();
#endif
#if CASE == 12
  return p + 1;
#else
  return p;
#endif
}
static void *first(void *unused) {
  (void)unused;
  int i;
#if CASE == 2
  for (i = 0; i <= cut; ++i)
#else
  for (i = 0; i < cut; ++i)
#endif
  {
#if CASE == 7
    memcpy(&output[i], &input[i], 2 * sizeof(element));
#elif CASE == 15
    output[i + 1] = input[i];
#elif CASE == 42
    output[(unsigned short)i] = input[i] + 1;
#elif CASE == 43
    output[(size_t)(unsigned)i] = input[(size_t)(unsigned)i] + 1;
#elif CASE == 49
    output[(size_t)(unsigned)(i - 1)] = input[i] + 1;
#else
    output[i] = input[i] + 1;
#endif
  }
#if CASE == 8
  length = 0;
#elif CASE == 34
  // At exit i==cut: this is outside the certified left half.
  if (i < length) output[i] = 9;
#endif
  return NULL;
}
static void *second(void *unused) {
  (void)unused;
#if CASE == 1
  for (int i = cut - 1; i < length; ++i)
#elif CASE == 20
  for (int i = 0; i < cut; ++i)
#elif CASE == 23
  for (int i = cut + 1; i < length; ++i)
#else
  for (int i = cut; i < length; ++i)
#endif
  {
#if CASE == 19
    other[i] = input[i] + 1;
#else
    output[i] = input[i] + 1;
#endif
  }
  return NULL;
}
#if CASE == 51
static void *third(void *unused) {
  (void)unused;
  output[0] = 7;
  return NULL;
}
#endif
int main(void) {
  pthread_t a, b;
#if CASE == 48
  unsigned small_handle;
#elif CASE == 51
  pthread_t extra;
#endif
#if CASE == 18
  length = 4; cut = 2;
#else
  length = __VERIFIER_nondet_int();
  cut = __VERIFIER_nondet_int();
#endif
#if CASE == 40 || CASE == 41
  // Both element widths exceed 4GB without changing the signed i32 loop.
  bound(length >= 1073741825 && length <= 1073741827);
#elif CASE == 44
  bound(twice64(length) == (long long)length + (long long)length);
#elif CASE == 45
  bound(increment64((long long)__VERIFIER_nondet_ulonglong()) > 0);
#endif
  input = make(length);
  output = make(length);
#if CASE == 19
  other = output;
#elif CASE == 31
  *(unsigned char *)&length = 0;
#elif CASE == 32
  *(unsigned *)&output = 4;
#elif CASE == 46
  ((unsigned char *)&output)[sizeof(output) - 1] = 0;
#endif
  bound(cut >= 0 && cut <= length);
#if CASE == 9
  pthread_create((pthread_t *)&cut, NULL, first, NULL);
#elif CASE == 48
  pthread_create((pthread_t *)&small_handle, NULL, first, NULL);
#elif CASE == 56
  if (pthread_create(&a, NULL, first, NULL)) cut = 0;
#else
  pthread_create(&a, NULL, first, NULL);
#endif
#if CASE == 3
  cut = 0;
#elif CASE == 4
  output = input;
#elif CASE == 11
  opaque();
#elif CASE == 47
  ((unsigned char *)&a)[sizeof(a) - 1] = 0;
#elif CASE == 52
  change_cut();
#endif
  pthread_create(&b, NULL, second, NULL);
#if CASE == 51
  pthread_create(&extra, NULL, third, NULL);
#endif
#if CASE == 10
  void *result;
  pthread_join(a, &result);
#elif CASE == 48
  pthread_join((pthread_t)small_handle, NULL);
#else
  pthread_join(a, NULL);
#endif
  pthread_join(b, NULL);
#if CASE == 13
  free(output);
#endif
  return 0;
}
