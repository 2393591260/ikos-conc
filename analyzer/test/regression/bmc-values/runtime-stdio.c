// SPDX-License-Identifier: MIT
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <locale.h>
#ifndef CASE
#define CASE 0
#endif
#ifndef LIMIT
#define LIMIT 12
#endif
#ifndef INITIAL
#define INITIAL 2
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
extern void __VERIFIER_atomic_begin(void);
extern void __VERIFIER_atomic_end(void);
#if CASE == 36
// Wide protocol counters are outside this fixed-i32 certificate.
static long long population = INITIAL;
static int scanned_population = INITIAL;
#else
static int population = INITIAL;
#endif
#if CASE == 33
#define SLOT_INDEX(i) ((uint64_t)(i) + (UINT64_C(1) << 32) + 1)
#elif CASE == 37
#define SLOT_INDEX(i) ((unsigned short)(i))
#elif defined(UNSIGNED_INDEX)
#define SLOT_INDEX(i) ((unsigned)(i))
#else
#define SLOT_INDEX(i) (i)
#endif
static datum payload;
#if CASE == 13
static FILE *saved_stream;
#endif
#if CASE == 17
extern void opaque(int *);
#endif
static void *worker(void *unused) {
  (void)unused;
#if CASE == 30
  population = 3;
#endif
#if CASE == 29
  ((char *)&payload)[1] = 1;
#endif
#if CASE != 1
  __VERIFIER_atomic_begin();
#endif
  datum observed = payload;
  payload = 1;
#if CASE != 1
  __VERIFIER_atomic_end();
#endif
#if CASE == 15
  int local;
  (void)sscanf("3", "%d", &local);
#endif
  if (observed != 0) {
#if CASE == 11
    fprintf(stderr, "%s", "diagnostic");
#elif CASE == 12
    fprintf(stderr, "%n", &population);
#elif CASE == 13
    fprintf(saved_stream, "diagnostic\n");
#else
    fprintf(stderr, "diagnostic %% %d\n", (int)observed);
#endif
  }
  return 0;
}
int main(int argc, char **argv) {
  int status;
#if CASE == 13
  saved_stream = stderr;
#endif
#if CASE == 14
  stderr = stdout;
#endif
#if CASE == 36
#define SCAN_DEST scanned_population
#else
#define SCAN_DEST population
#endif
#ifdef MAPPED
  extern int raw_scan(const char *, const char *, ...) __asm__("sscanf");
  if (argc == 2) (void)raw_scan(argv[1], "%d", &SCAN_DEST);
#else
  if (argc == 2) (void)sscanf(argv[1], "%d", &SCAN_DEST);
#endif
#if CASE == 36
  population = scanned_population;
#endif
#if CASE != 2
  if (population > LIMIT) exit(1);
#endif
#if CASE == 3
  int snapshot = population;
  population = LIMIT;
  pthread_t handles[snapshot];
#elif CASE == 32
  pthread_t handles[population - 1];
#elif defined(SIGNED_EXTENT)
  pthread_t handles[(intptr_t)population];
#else
  pthread_t handles[population];
#endif
#if CASE == 4
  population = LIMIT;
#endif
#if CASE == 16
  (void)setlocale(0, "C");
#endif
#if CASE == 17
  opaque(&population);
#endif
#if CASE == 18
  *((int *)(uintptr_t)population) = 1;
#endif
#if CASE == 19
  FILE *stream = (FILE *)(void *)&population;
  fprintf(stream, "outside\n");
#endif
  for (int index = CASE == 5 ? 1 : 0; index < population; index += CASE == 6 ? 2 : 1) {
#if CASE == 7
    if (index == 1) continue;
#endif
#if CASE == 8
    if (index == 1) break;
#endif
#if CASE == 27
    status = pthread_create(&handles[index], 0, worker, &population);
#elif CASE == 9
    status = pthread_create(&handles[index + 1], 0, worker, 0);
#elif CASE == 38
    status = (unsigned char)pthread_create(&handles[SLOT_INDEX(index)], 0, worker, 0);
#else
    status = pthread_create(&handles[SLOT_INDEX(index)], 0, worker, 0);
#endif
#if CASE == 39
    if (status != 0) { fprintf(stderr, "create %d\n", status); continue; }
#elif CASE != 10
    if (status != 0) { fprintf(stderr, "create %d\n", status); exit(1); }
#endif
  }
#if CASE == 28
  fprintf(stderr, argv[1], population);
#endif
#if CASE == 20
  handles[0] = 0;
#elif CASE == 35
  ((unsigned char *)handles)[sizeof(pthread_t) - 1] = 0;
#endif
#if CASE == 31
  pthread_t chosen;
  for (int index = 0; index < population; ++index) {
    chosen = handles[index];
    if (argc > 1) goto outside_consumer;
  }
  goto final_return;
outside_consumer:
  status = pthread_join(chosen, 0);
  if (argc > 1) goto outside_consumer;
final_return:
  return 0;
#else
#if CASE == 26
restart_consumer:
#endif
  for (int index = 0; index < population; ++index) {
#if CASE == 24
    pthread_t handle = handles[index];
    if (argc & 1) goto consume_a;
    goto consume_b;
consume_a:
    status = pthread_join(handle, 0);
    if (argc & 2) goto consume_b;
    goto done_consumer;
consume_b:
    if (argc & 4) goto consume_a;
done_consumer:
#elif CASE == 25
    pthread_t handle = handles[index];
repeat_consumer:
    status = pthread_join(handle, 0);
    if (argc & 1) goto repeat_consumer;
#elif CASE == 34
    status = pthread_join((pthread_t)(unsigned int)handles[index], 0);
#else
    status = pthread_join(handles[SLOT_INDEX(index)], 0);
#endif
    if (status != 0) { fprintf(stderr, "join %d\n", status); exit(1); }
#if CASE == 21
    pthread_join(handles[index], 0);
#endif
  }
#if CASE == 26
  if (argc & 1) goto restart_consumer;
#endif
#if CASE == 22
  pthread_join((pthread_t)population, 0);
#endif
#if CASE == 23
  (void)sscanf(argv[1], "%d", &SCAN_DEST);
#endif
  return 0;
#endif
}
