// SPDX-License-Identifier: MIT
#include <pthread.h>
#include <stdlib.h>
#include <setjmp.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
extern int __VERIFIER_nondet_int(void);
static datum shared;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_once_t other = PTHREAD_ONCE_INIT;
static void touch(void) { ++shared; }
static void first(void);
static void second(void) {
#if CASE == 24
  extern void hidden(void);
  hidden();
#elif CASE == 25
  pthread_exit(0);
#elif CASE == 26
  second();
#elif CASE == 29
  pthread_testcancel();
#elif CASE == 27
  extern jmp_buf target;
  longjmp(target, 1);
#else
  touch();
#endif
}
static void first(void) { touch(); }
#if CASE == 4
static void (*initp)() = first;
#elif CASE == 22
static __thread void (*initp)(void) = first;
#else
static void (*initp)(void) = first;
#endif
static void *worker(void *arg) {
  (void)arg;
#if CASE == 10
  first();
#elif CASE == 11
  touch();
#elif CASE == 12
  pthread_once(&other, initp);
#else
  pthread_once(&once, initp);
#endif
#if CASE == 13
  pthread_once(&other, first);
#endif
  return 0;
}
int main(void) {
  pthread_t id;
  int choice = __VERIFIER_nondet_int();
#if CASE != 1 && CASE != 14
  if (choice) initp = second;
#endif
#if CASE == 20
  if (choice) initp = 0;
#elif CASE == 21
  extern void escape(void *);
  escape(&initp);
#elif CASE == 23
  *((char *)&once) = choice;
#elif CASE == 28
  *((char *)&initp) = choice;
#elif CASE == 30
  extern void leak(void (*)(void));
  leak(initp);
#elif CASE == 31
  once = 1;
#endif
  pthread_create(&id, 0, worker, 0);
#if CASE == 1
  pthread_join(id, 0);
  if (choice) initp = second;
#elif CASE == 14
  initp = second;
#elif CASE == 10 || CASE == 11
  initp = second;
#endif
#if CASE == 2
  pthread_once(&once, first);
#else
  pthread_once(&once, initp);
#endif
#if CASE == 3
  pthread_once(&once, initp);
#endif
  return 0;
}
