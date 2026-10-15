// SPDX-License-Identifier: MIT
#include <pthread.h>
#include <strings.h>
#ifndef INPUT
#define INPUT 2
#endif
#ifndef EXPECTED
#define EXPECTED 2
#endif
extern int __VERIFIER_nondet_int(void);
int datum;
#ifdef SHARED
int bits = INPUT;
#endif
#ifdef DEFINED
int ffs(int value) { return 0; }
#endif
void *first(void *unused) {
#ifdef SHARED
  int value = bits;
#else
  int value = __VERIFIER_nondet_int();
  if (value != (int)INPUT) return 0;
#endif
#ifdef INDIRECT
  int (*operation)(int) = ffs;
  int index = operation(value);
#else
  int index = ffs(value);
#endif
  if (index != EXPECTED) return 0;
  datum = 1;
  return 0;
}
void *second(void *unused) { datum = 2; return 0; }
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, first, 0);
  pthread_create(&b, 0, second, 0);
  return 0;
}
