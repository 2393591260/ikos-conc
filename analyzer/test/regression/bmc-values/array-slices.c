// Stable flat-array slices: full frames and counterexamples to their premises.
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
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
static element *make(int n) {
  bound(n >= 0);
#if CASE == 30
  int local = -1;
  *(unsigned char *)&local = 0;
  long long widened = local;
  if (widened == 0)
#endif
#if CASE != 5
  bound((unsigned)n <= UINT32_MAX / sizeof(element));
#endif
#if CASE == 6
  element *p = malloc((unsigned short)(sizeof(element) * (unsigned)n));
#else
  element *p = malloc(sizeof(element) * (unsigned)n);
#endif
  for (int i = 0; i < n; ++i) p[i] = arbitrary();
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
int main(void) {
  pthread_t a, b;
#if CASE == 18
  length = 4; cut = 2;
#else
  length = __VERIFIER_nondet_int();
  cut = __VERIFIER_nondet_int();
#endif
  input = make(length);
  output = make(length);
#if CASE == 19
  other = output;
#elif CASE == 31
  *(unsigned char *)&length = 0;
#elif CASE == 32
  *(unsigned *)&output = 4;
#endif
  bound(cut >= 0 && cut <= length);
#if CASE == 9
  pthread_create((pthread_t *)&cut, NULL, first, NULL);
#else
  pthread_create(&a, NULL, first, NULL);
#endif
#if CASE == 3
  cut = 0;
#elif CASE == 4
  output = input;
#elif CASE == 11
  opaque();
#endif
  pthread_create(&b, NULL, second, NULL);
#if CASE == 10
  void *result;
  pthread_join(a, &result);
#else
  pthread_join(a, NULL);
#endif
  pthread_join(b, NULL);
#if CASE == 13
  free(output);
#endif
  return 0;
}
