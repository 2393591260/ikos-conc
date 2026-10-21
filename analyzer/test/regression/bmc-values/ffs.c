// SPDX-License-Identifier: MIT
#include <pthread.h>
#if defined(WIDE_API) || defined(VARARGS) || defined(STDCALL)
#define ffs declared_ffs
#endif
#include <strings.h>
#undef ffs
#ifdef WIDE_API
extern long long ffs(long long);
#elif defined(VARARGS)
extern int ffs(int, ...);
#elif defined(STDCALL)
extern int __attribute__((stdcall)) ffs(int);
#elif defined(NO_RETURN)
extern int ffs(int) __attribute__((noreturn));
#elif defined(RETURNS_TWICE)
extern int ffs(int) __attribute__((returns_twice));
#endif
#ifdef MODULE_ASM
__asm__(".text\n.globl ffs\nffs:\nmovl $1, datum\nxorl %eax, %eax\nret\n");
#endif
#ifndef INPUT
#define INPUT 2
#endif
#ifndef EXPECTED
#define EXPECTED 2
#endif
extern int __VERIFIER_nondet_int(void);
#ifdef WIDE
long long datum;
#else
int datum;
#endif
#ifdef LOCKED
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
#endif
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
#ifdef LOCKED
  pthread_mutex_lock(&mutex);
#endif
#if defined(INDIRECT) || defined(UNRESOLVED)
#ifdef UNRESOLVED
  int (*volatile operation)(int) = ffs;
#else
  int (*operation)(int) = ffs;
#endif
  int index = operation(value);
#else
  int index = ffs(value);
#endif
  if (index != EXPECTED) {
#ifdef LOCKED
    pthread_mutex_unlock(&mutex);
#endif
    return 0;
  }
  datum = 1;
#ifdef LOCKED
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}
void *second(void *unused) {
#ifdef LOCKED
  pthread_mutex_lock(&mutex);
#endif
  datum = 2;
#ifdef LOCKED
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, first, 0);
  pthread_create(&b, 0, second, 0);
  return 0;
}
