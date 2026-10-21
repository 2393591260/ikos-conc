// SPDX-License-Identifier: MIT
#include <pthread.h>
#include <stdint.h>
#include <string.h>
extern int __VERIFIER_nondet_int(void);
extern void unknown(void *);
extern void other_effect(void);
#ifdef MODULE_ASM
__asm__(".text");
#endif
#ifdef WIDE
typedef long long element;
#else
typedef int element;
#endif
int datum;
element *published;
uintptr_t address;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

void *array_user(void *unused) {
#ifdef POINTERS
  element *array[8];
#elif defined(STRUCTS)
  struct { element value; } array[8];
#elif defined(DYNAMIC)
  element array[(__VERIFIER_nondet_int() & 7) + 1];
#else
  element array[8];
#endif
#if !defined(POINTERS) && !defined(STRUCTS) && !defined(DYNAMIC)
  array[0] = 7;
#endif
#ifdef LOOP
  for (int iteration = 0; iteration < 2; ++iteration) {
#endif
#ifdef PARTIAL
  __builtin_memset(array, 0, sizeof(array) - sizeof(element));
#elif defined(SHIFTED)
  __builtin_memset(array + 1, 0, sizeof(array) - sizeof(element));
#elif defined(NONZERO)
  __builtin_memset(array, 1, sizeof(array));
#else
  __builtin_memset(array, 0, sizeof(array));
#endif
#ifdef LOOP
  }
#endif
#ifdef ESCAPE
  published = (element *)array;
#elif defined(THREAD_ARG)
  pthread_t child;
  pthread_create(&child, 0, array_user, array);
#elif defined(RETURN_POINTER)
  return array;
#elif defined(ADDRESS)
  address = (uintptr_t)array;
#elif defined(HELPER)
  unknown(array);
#elif defined(MIXED)
  element *p = __VERIFIER_nondet_int() ? (element *)array : published;
  *p = 2;
#elif defined(RECURSIVE)
  if (__VERIFIER_nondet_int()) array_user(0);
#elif defined(VOLATILE_USE)
  ((volatile element *)array)[0] = 1;
#elif !defined(POINTERS) && !defined(STRUCTS) && !defined(DYNAMIC)
#ifdef INDEX
  int index = __VERIFIER_nondet_int() & 7;
#else
  int index = 0;
#endif
#ifdef OVERWRITE
  array[index] = 7;
#endif
  // A real zero fill replaces 7; the opposite guard must stay infeasible.
#ifdef ZERO_GUARD
  if (array[index] != 0) {
#else
  if (array[index] != 7) {
#endif
#ifdef LOCKED
    pthread_mutex_lock(&mutex);
#endif
    datum = 1;
#ifdef LOCKED
    pthread_mutex_unlock(&mutex);
#endif
  }
#endif
#ifdef OTHER_EFFECT
  other_effect();
#endif
  return 0;
}
void *writer(void *unused) {
#ifdef LOCKED
  pthread_mutex_lock(&mutex);
#endif
  datum = 3;
#ifdef LOCKED
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, array_user, 0);
#ifdef TWO_INSTANCES
  pthread_create(&b, 0, array_user, 0);
#endif
#ifdef SEQUENTIAL_CALL
  array_user(0);
#endif
#ifndef CONTENT
  pthread_create(&b, 0, writer, 0);
#endif
#ifdef LOCKED
  pthread_mutex_lock(&mutex);
#endif
  datum = 2;
#ifdef LOCKED
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}
