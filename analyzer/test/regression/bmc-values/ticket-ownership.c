// Unique issued rights are independent of source-level thread identity.
#include <pthread.h>
#include <stdlib.h>
#include <assert.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
#if CASE == 37
typedef int ticket;
#elif CASE == 30
typedef unsigned long long ticket;
#else
typedef unsigned ticket;
#endif
static volatile ticket served, next;
static datum data;
#if CASE == 18 || CASE == 24
static ticket shared_ticket;
#endif
extern void __VERIFIER_atomic_begin(void);
extern void __VERIFIER_atomic_end(void);
extern int __VERIFIER_nondet_int(void);
#if CASE == 19
#define __VERIFIER_atomic_issue ordinary_issue
#endif
#if CASE == 31
void __VERIFIER_atomic_begin(void) {}
void __VERIFIER_atomic_end(void) {}
#endif
static void require(int condition) {
#if CASE != 3
  if (!condition) abort();
#else
  (void)condition;
#endif
}
void __VERIFIER_atomic_issue(ticket *out) {
#if CASE == 2
  require(next != (ticket)-2);
#elif CASE != 1
  require(next != (ticket)-1);
#endif
#if CASE == 4
  *out = next + 1;
#else
  *out = next;
#endif
#if CASE == 5
  next = next + 2;
#else
  next = next + 1;
#endif
#if CASE == 6
  next = 0;
#endif
}
__attribute__((noinline)) static void payload(void) { data = 1; }
static void *protocol(void *arg) {
  (void)arg;
#if CASE == 18
#define mine shared_ticket
#else
  ticket mine;
#endif
#if CASE == 29
  for (int repetition = 0; repetition < 2; ++repetition) {
#endif
  __VERIFIER_atomic_issue(&mine);
#if CASE == 7
  mine = 0;
#endif
#if CASE == 24
  shared_ticket = mine;
#endif
#if CASE == 33
  *(unsigned char *)&mine = 0;
#endif
#if CASE != 9
  __VERIFIER_atomic_begin();
#endif
#if CASE == 16
  if (__VERIFIER_nondet_int()) goto acquired;
#endif
#if CASE == 8
  while (mine != served + 1) {}
#elif CASE == 17
  while (mine != served) { data = 7; }
#elif CASE == 36
  while ((int)mine != (int)served) {}
#else
  while (mine != served) {}
#endif
#if CASE == 16
acquired:
#endif
#if CASE != 9
  __VERIFIER_atomic_end();
#endif
#if CASE == 12
  __VERIFIER_atomic_begin();
  served++;
  __VERIFIER_atomic_end();
#endif
#if CASE == 27
  payload();
#else
  data = 1;
#endif
  assert(data == 1);
  data = 0;
#if CASE == 14
  return 0;
#endif
#if CASE == 15
  if (__VERIFIER_nondet_int()) return 0;
#endif
#if CASE == 21
  if (served == 0) data = 2;
#endif
#if CASE != 12
#if CASE != 10
  __VERIFIER_atomic_begin();
#endif
#if CASE == 11
  served += 2;
#else
  served++;
#endif
#if CASE != 10
  __VERIFIER_atomic_end();
#endif
#endif
#if CASE == 13
  __VERIFIER_atomic_begin();
  served++;
  __VERIFIER_atomic_end();
#endif
#if CASE == 29
  }
#endif
  return 0;
}
#if CASE == 28
static void *other(void *arg) { (void)arg; return protocol(0); }
#endif
int main(void) {
  pthread_t handle;
#if CASE == 28
  pthread_t second_handle;
#endif
  while (__VERIFIER_nondet_int()) {
    pthread_create(&handle, 0, protocol, 0);
#if CASE == 28
    pthread_create(&second_handle, 0, other, 0);
#endif
  }
#if CASE == 20
  next = 0;
#endif
#if CASE == 22
  *(unsigned char *)&next = 0;
#endif
#if CASE == 26
  data = 0;
#endif
#if CASE == 27
  payload();
#endif
#if CASE == 32
  __asm__ volatile("" ::: "memory");
#endif
  protocol(0);
  return 0;
}
