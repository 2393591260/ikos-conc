#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
static int capacity = CASE == 1 ? 3 : 2;
static datum value;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static void *escaped;
extern void opaque(int *);
extern void *save_stack(void) __asm__("llvm.stacksave");
extern void restore_stack(void *) __asm__("llvm.stackrestore");
static void *worker(void *arg) {
#if CASE == 5
  pthread_mutex_lock(&gate);
#elif CASE == 12
  capacity = 3;
#endif
  value = 1;
#if CASE == 5
  pthread_mutex_unlock(&gate);
#endif
  return 0;
}
#if CASE == 3
int main(void) {
#else
int main(int argc, char **argv) {
#if CASE == 15
  argc = 2;
#endif
#if CASE != 18
  if (argc != 1) abort();
#endif
#endif
#if CASE == 2
  capacity = 3;
#elif CASE == 6
  capacity = 0;
#elif CASE == 8
  capacity = 1;
#elif CASE == 13
  opaque(&capacity);
#elif CASE == 14
  capacity = -1;
#elif CASE == 16
  capacity = 1073741824;
#endif
#if CASE != 6 && CASE != 14 && CASE != 16
  if (capacity < 1 || capacity > 8) abort();
#endif
  int count = capacity;
#if CASE == 9
  for (int pass = 0; pass < 2; ++pass) {
#endif
#if CASE == 18
  pthread_t handles[2];
  pthread_create(&handles[argc], 0, worker, 0);
  pthread_create(&handles[argc], 0, worker, 0);
  return 0;
#else
  pthread_t handles[count];
#endif
#if CASE == 4
  capacity = 8;
#elif CASE == 10
  void *token = save_stack();
  restore_stack(token);
#elif CASE == 11
  escaped = save_stack();
#endif
#if CASE == 9
  pthread_create(&handles[0], 0, worker, 0);
#else
  for (int i = 0; i <
#if CASE == 8
       2
#else
       count
#endif
       ; ++i) {
#if CASE == 7
    pthread_create(i ? &handles[count] : &handles[0], 0, worker, 0);
#elif CASE == 17
    pthread_t *out = &handles[i] + count + 1;
    out = out - count - 1;
    pthread_create(out, 0, worker, 0);
#else
    pthread_create(&handles[i], 0, worker, 0);
#endif
  }
#endif
#if CASE == 9
  }
#else
  for (int i = 0; i < count; ++i) pthread_join(handles[i], 0);
#endif
  return 0;
}
