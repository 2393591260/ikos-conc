#include <pthread.h>
#include <limits.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
#if CASE == 40
/* Even harmless module assembly lies outside the closed IR effect inventory. */
__asm__(".ident \"arena-control\"");
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
#if CASE == 1 || CASE == 18
#define CAP 64
#elif CASE == 2
#define CAP 7
#else
#define CAP 65
#endif
#if CASE == 3
#define reserve ordinary_reserve
#else
#define reserve __VERIFIER_atomic_renamed_reserve
#endif
datum storage[CAP];
#if CASE == 4
int cursor = INT_MAX - 1;
#elif CASE == 5
int cursor = INT_MAX - 3;
#elif CASE == 6
int cursor = 0;
#else
int cursor = 1;
#endif
int published, shared, shared_out;
datum* escaped_arena;
pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
extern int __VERIFIER_nondet_int(void);
#if CASE == 7
extern void reserve(int*);
#else
void reserve(int* result) {
#if CASE == 36
  *result = cursor; cursor += 2; /* No exhausted self-loop: reject incomplete closure. */
#elif CASE == 8
  *result = cursor;
#elif CASE == 9
  *result = cursor; cursor = 1;
#elif CASE == 10
  *result = cursor; cursor -= 2;
#elif CASE == 11
  if (cursor + 1 > CAP) { *result = 0; cursor += 2; }
  else { *result = cursor; cursor += 2; }
#elif CASE == 12
  if (cursor + 1 > CAP) return;
  *result = cursor; cursor += 2;
#elif CASE == 13
  if (cursor + 1 > CAP) *result = 0;
  else { cursor += 2; *result = cursor; }
#elif CASE == 14
  if (shared) { *result = cursor; cursor += 2; }
  else *result = 0;
#elif CASE == 15
  if (__VERIFIER_nondet_int()) { *result = cursor; cursor += 2; }
  else *result = 0;
#elif CASE == 4
  /* Must reject the overflowing guard before its result can constrain it. */
  if (cursor + 2 - 1 > CAP) *result = 0;
  else { *result = cursor; cursor += 2; }
#elif CASE == 5
  /* Exhaustion itself overflows, even though the first reservation fits. */
  if (cursor + 2 - 1 > INT_MAX - 1) *result = 0;
  else { *result = cursor; cursor += 2; }
#elif CASE == 16
  if (cursor + 1 > CAP) *result = 0;
  else { *((short*)result) = cursor; cursor += 2; }
#elif CASE == 17
  reserve(result);
#elif CASE == 18
  if (cursor + 1 > CAP) *result = 0;
  else { *result = cursor; cursor += 3; }
#elif CASE == 19
  /* A new counter load after its store is the updated value, not entry. */
  if (cursor + 1 > CAP) *result = 0;
  else { cursor += 2; *result = cursor - 2; }
#else
  if (cursor + 2 - 1 > CAP) *result = 0;
  else { *result = cursor; cursor += 2; }
#endif
}
#endif
static void touch(void) {
  int token = -1;
#if CASE == 20
  reserve(&shared_out); token = shared_out;
#elif CASE == 21
  reserve(&cursor); token = cursor;
#elif CASE == 22
  for (int i = 0; i < 2; ++i) reserve(&token);
#elif CASE == 23
  if (token) reserve(&token);
#else
  reserve(&token);
#endif
#if CASE == 24
  token = 1;
#elif CASE == 25
  published = (int)(long)&token;
#elif CASE == 26
  reserve(&token);
#endif
#if CASE == 27
  storage[token] = 1; /* Missing successful-reservation guard. */
#else
  if (!token) abort();
#if CASE == 28
  storage[token + 2] = 1;
#elif CASE == 29
  storage[token - 1] = 1;
#elif CASE == 30
  *((long long*)&storage[token + 1]) = 1;
#elif CASE == 31
  storage[(token + INT_MAX) - INT_MAX] = 1;
#else
  storage[token] = 1;
  pthread_mutex_lock(&mu);
  storage[token + 1] = 2;
  published = token;
  pthread_mutex_unlock(&mu);
#endif
#endif
}
static void* work(void* unused) {
#if CASE == 32
  cursor = 1;
#elif CASE == 33
  pthread_mutex_lock(&mu); cursor = 1; pthread_mutex_unlock(&mu);
#elif CASE == 34
  shared++;
#elif CASE == 35
  storage[published] = 3;
#elif CASE == 37
  pthread_mutex_lock((pthread_mutex_t*)&storage[1]);
#elif CASE == 38
  escaped_arena = storage;
#elif CASE == 39
  *((short*)&cursor) = 1;
#endif
  for (;;) touch();
  return 0;
}
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, work, 0);
  pthread_create(&b, 0, work, 0);
  return 0;
}
