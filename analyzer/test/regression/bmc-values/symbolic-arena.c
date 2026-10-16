#include <pthread.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
#ifndef WIDE
#define WIDE 0
#endif
#ifndef CHUNK
#define CHUNK 7
#endif
#if WIDE
typedef long long datum;
#else
typedef int datum;
#endif
extern int __VERIFIER_nondet_int();
extern void opaque(void *);
static volatile int limit, cursor;
static datum *storage;
static pthread_mutex_t serializer = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t alternate = PTHREAD_MUTEX_INITIALIZER;
static void require(int c) {
#if CASE != 20
  if (!c) abort();
#endif
}
#if CASE == 36
static void hidden_reset(void) { cursor = 0; }
#endif
static void *worker(void *unused) {
  int begin = 0, end = 0;
#if CASE == 36
  hidden_reset();
#endif
#if CASE == 31
  int saved_cursor = cursor;
#endif
#if CASE != 1
#if CASE == 5
  if (__VERIFIER_nondet_int()) pthread_mutex_lock(&alternate);
  else pthread_mutex_lock(&serializer);
#else
  pthread_mutex_lock(&serializer);
#endif
#endif
  if (cursor + CHUNK <= limit) {
#if CASE == 31
    begin = saved_cursor;
#elif CASE == 3
    begin = 0;
#elif CASE == 24
    begin = cursor + 1;
#else
    begin = cursor;
#endif
    end = cursor + CHUNK;
#if CASE == 2
    cursor = 0;
#else
    cursor = end;
#endif
  }
#if CASE == 6
  limit = limit + 1;
#endif
#if CASE == 16
  *(volatile unsigned char *)&cursor = 0;
#endif
#if CASE != 1
#if CASE == 5
  if (__VERIFIER_nondet_int()) pthread_mutex_unlock(&alternate);
  else pthread_mutex_unlock(&serializer);
#else
  pthread_mutex_unlock(&serializer);
#endif
#endif
#if CASE == 4
  cursor = 0;
#endif
#if CASE == 14
  opaque(storage);
#endif
#if CASE == 32
  if (__VERIFIER_nondet_int()) return 0;
  int stale = cursor;
  begin = stale;
#endif
#if CASE == 10
  for (int i = begin; i <= end; ++i) {
#else
  for (int i = begin; i < end; ++i) {
#endif
#if CASE == 9
    storage[i + CHUNK] = 1;
#else
    storage[i] = 1;
#endif
#if CASE == 25
    storage[i] = 2;
#endif
#if CASE == 26
    (void)*(volatile datum *)&storage[i];
#endif
#if CASE == 35
    --end;
#endif
  }
#if CASE == 12
  storage[0] = 2;
#endif
  return 0;
}
#if CASE == 19
int main(int argc, char **argv) {
#else
int main(void) {
#endif
  pthread_t private_output;
  cursor = 0;
  limit = __VERIFIER_nondet_int();
#if CASE == 7
  require(limit > 0);
#else
  require(limit > 0 && (unsigned long long)limit < 4294967296ULL / sizeof(datum));
#endif
#if CASE == 8
  storage = malloc(sizeof(datum) * limit - 1);
#else
  storage = malloc(sizeof(datum) * limit);
#endif
#if CASE == 33
  storage = malloc(sizeof(datum) * limit);
#endif
#if CASE == 15
  opaque(&storage);
#endif
  while (1) {
#if CASE == 29
    pthread_create(&private_output, 0, worker, (void *)1);
#else
    pthread_create(&private_output, 0, worker, 0);
#endif
#if CASE == 21
    limit = 1;
#endif
#if CASE == 22
    storage = 0;
#endif
#if CASE == 34
    storage[0] = 2;
#endif
  }
}
