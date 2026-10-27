// SPDX-License-Identifier: MIT
// Each calloc zero source belongs to its actual pre-spawn allocation occurrence.
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#ifndef MODE
#define MODE 0
#endif
#ifdef WIDE
typedef long long element;
#else
typedef int element;
#endif
extern int __VERIFIER_nondet_int(void);
static void assume_abort_if_not(int condition) { if (!condition) abort(); }
static int shared;
uintptr_t escaped;
static void *checked_array(size_t count, size_t width) {
  void *p = calloc(count, width);
  if (!p) abort();
  return p;
}
static void *checked_bytes(size_t extent) {
  void *p = malloc(extent);
  if (!p) abort();
  return p;
}
#if MODE == 17
void *calloc(size_t count, size_t width) {
  static element different[2] = {7, 7};
  (void)count; (void)width;
  return different;
}
#endif
static void *worker(void *argument) {
  element *p = argument;
#if MODE == 12
  p = calloc(1, sizeof(element));
  if (!p) abort();
#endif
#if MODE == 5 || MODE == 8 || MODE == 9 || MODE == 17
  if (*p == 0) shared = 1;
#elif MODE == 6 || MODE == 20
  if (*p == 7) shared = 1;
#elif MODE == 7
  if (*p != 0) shared = 1;
#elif MODE == 19
  shared = 1;
#else
  element value = *p;
  (void)value;
#endif
  return 0;
}
#if MODE == 27 || MODE == 28 || MODE == 29
int main(void) {
  pthread_t *handles = checked_bytes(2 * sizeof(pthread_t));
  element *p = calloc(3, sizeof(element));
  if (!p) abort();
  for (int i = 0; i < 2; ++i)
    pthread_create(&handles[i], 0, worker, &p[i]);
#if MODE == 28
  pthread_join(handles[0], 0);
#endif
#if MODE == 29
  p[2] = 9;
#else
  p[0] = 9;
#endif
#if MODE != 28
  pthread_join(handles[0], 0);
#endif
  pthread_join(handles[1], 0);
  return 0;
}
#elif MODE == 24 || MODE == 25 || MODE == 26
static void *writer(void *argument) { *(element *)argument = 1; return 0; }
int main(void) {
  pthread_t *handles = checked_bytes(2 * sizeof(pthread_t));
  element *first = 0, *selected = 0;
  for (unsigned generation = 0; generation < 2; ++generation) {
    element *p = calloc(1, sizeof(element));
    if (!p) abort();
    if (generation == 0) {
      first = p;
      *p = 7;
#if MODE == 26
      free(p);
#endif
    }
    selected = p;
  }
#if MODE == 25
  pthread_create(&handles[0], 0, writer, first);
#else
  pthread_create(&handles[0], 0, writer, selected);
#endif
  pthread_create(&handles[1], 0, worker, selected);
  pthread_join(handles[0], 0);
  pthread_join(handles[1], 0);
  return 0;
}
#else
int main(void) {
#if MODE == 0
  int count = __VERIFIER_nondet_int();
  assume_abort_if_not(count >= 0);
  assume_abort_if_not((size_t)count <= UINT32_MAX / sizeof(pthread_t));
#else
  int count = 1;
#endif
  pthread_t *handles = checked_bytes((size_t)count * sizeof(pthread_t));
  assume_abort_if_not((size_t)count <= UINT32_MAX / sizeof(element));
#if MODE == 0 || MODE == 2
  element *p = checked_array((size_t)count, sizeof(element));
#elif MODE == 3
  element *p = calloc(sizeof(element), 2);
#elif MODE == 9
  element *p = malloc(sizeof(element));
#elif MODE == 13
  element *p = calloc(0, sizeof(element));
  (void)p;
  return 0;
#elif MODE == 14
  element *p = calloc(1, 0);
#elif MODE == 15
  element *p = calloc(SIZE_MAX / 2 + 1, 2);
#elif MODE == 16
  element *p = calloc((size_t)-1, sizeof(element));
#elif MODE == 20
  element *p = calloc(2, sizeof(element));
#else
  element *p = calloc((size_t)count, sizeof(element));
#endif
#if MODE == 19
  if (p) return 0;
#else
  if (!p) abort();
#endif
#if MODE == 6 || MODE == 8
  *p = 7;
#elif MODE == 20
  p[1] = 7;
  p = p + 1;
#elif MODE == 11
  free(p);
#elif MODE == 18
  escaped = (uintptr_t)p;
#endif
#if MODE == 10
  element *other = calloc(1, sizeof(element));
  if (!other) abort();
#elif MODE == 22
  element *other = calloc(1, sizeof(element));
  if (!other) abort();
  *other = 7;
#endif
  for (int i = 0; i < count; ++i) {
    pthread_create(&handles[i], 0, worker, &p[i]);
#if MODE == 23
    pthread_join(handles[i], 0);
#endif
#if MODE == 5 || MODE == 6 || MODE == 7 || MODE == 8 || MODE == 9 || MODE == 17 || MODE == 19 || MODE == 20
    shared = 2;
#elif MODE == 10
    *other = i;
#else
    p[i] = i;
#endif
  }
#if MODE != 23
  for (int i = 0; i < count; ++i) pthread_join(handles[i], 0);
#endif
#if MODE == 0
  free(handles);
  free(p);
#endif
  return 0;
}

#endif
