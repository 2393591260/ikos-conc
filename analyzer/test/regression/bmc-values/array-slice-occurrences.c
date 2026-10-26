// CASE 0: unshifted positive; CASE 1 (default): mixed offset, real race.
// CASE 2: always shifted, real race; CASE 3: two same-offset source paths.
// Build with -DWIDE to use a 64-bit datum independently of pointer ABI.
#include <pthread.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 1
#endif
#ifdef WIDE
typedef unsigned long long element;
#else
typedef unsigned element;
#endif
extern int __VERIFIER_nondet_int(void);
static element *output;
static int length, cut;
static void bound(int condition) {
  if (!condition) abort();
}
static element *make(int n) {
  bound(n >= 2 && n <= 100000);
  element *p = malloc(sizeof(element) * (unsigned)n);
  for (int i = 0; i < n; ++i) p[i] = 0;
  return p;
}
static void *left(void *unused) {
  (void)unused;
  element *p = output;
#if CASE == 1
  if (__VERIFIER_nondet_int()) p = p + 1;
#elif CASE == 2
  p = p + 1;
#elif CASE == 4
  if (!__VERIFIER_nondet_int()) p = p + 1;
#elif CASE == 5
  p = p + 1;
  if (__VERIFIER_nondet_int()) p = output;
#elif CASE == 3
  // Separate source loads may survive preprocessing; both offsets are zero.
  if (__VERIFIER_nondet_int()) p = output;
#endif
  for (int i = 0; i < cut; ++i) p[i] = 1;
  return NULL;
}
static void *right(void *unused) {
  (void)unused;
  for (int i = cut; i < length; ++i) output[i] = 2;
  return NULL;
}
int main(void) {
  pthread_t a, b;
  length = __VERIFIER_nondet_int();
  cut = __VERIFIER_nondet_int();
  bound(length >= 2 && length <= 100000);
  bound(cut > 0 && cut < length);
  output = make(length);
  pthread_create(&a, NULL, left, NULL);
  pthread_create(&b, NULL, right, NULL);
  pthread_join(a, NULL);
  pthread_join(b, NULL);
  return 0;
}
