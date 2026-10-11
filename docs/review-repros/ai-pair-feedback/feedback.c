#include <pthread.h>
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>

extern int __VERIFIER_nondet_int(void);
extern void opaque(int *);
#ifndef CASE
#define CASE 0
#endif

#if CASE == 8
static __thread int data[64];
#else
static int data[64];
#endif
static int other[64];
static int shared_index;
static int *heap;
union word { uint64_t wide; uint32_t part[2]; };
static union word words[8];

static void *worker(void *unused) {
  int i = __VERIFIER_nondet_int();
#if CASE == 1 || CASE == 3
  if (i < 0 || i >= 2) return 0;
  data[4*i] = 1;
#elif CASE == 4
  if (i < 0 || i >= 8) return 0;
  words[i].wide = 1;
#elif CASE == 5
  if (i < 0 || i >= 8) return 0;
  shared_index = i;
  data[2*i] = 1;
#elif CASE == 7
  if (i < 0 || i >= 8) return 0;
  heap[2*i] = 1;
#elif CASE == 13
  unsigned x = UINT_MAX;
  data[x + 1u] = 1;
#else
  if (i < 0 || i >= 8) return 0;
  data[2*i] = 1;
#endif
  return 0;
}

int main(void) {
#if CASE == 7
  heap = malloc(64 * sizeof(*heap));
  if (!heap) return 0;
#endif
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
#if CASE == 10
  pthread_t second;
  pthread_create(&second, 0, worker, 0);
#endif
  int j = __VERIFIER_nondet_int();
#if CASE == 1
  if (j >= 2 && j < 4) data[j] = 2;
#elif CASE == 3
  if (j >= 2 && j < 5) data[j] = 2;
#elif CASE == 2
  if (j >= 0 && j < 8) data[2*j] = 2;
#elif CASE == 4
  if (j >= 0 && j < 8) words[j].part[1] = 2;
#elif CASE == 5
  j = shared_index;
  if (j >= 0 && j < 8) data[2*j] = 2;
#elif CASE == 6
  if (j >= 0 && j < 8) other[2*j+1] = 2;
#elif CASE == 7
  if (j >= 0 && j < 8) heap[2*j+1] = 2;
#elif CASE == 11
  opaque(data);
  if (j >= 0 && j < 8) data[2*j+1] = 2;
#elif CASE == 13
  data[0] = 2;
#else
  if (j >= 0 && j < 8) data[2*j+1] = 2;
#endif
  pthread_join(t, 0);
#if CASE == 10
  pthread_join(second, 0);
#endif
#if CASE == 7
  free(heap);
#endif
  return 0;
}
