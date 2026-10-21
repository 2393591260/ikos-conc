#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef CASE
#define CASE 0
#endif
int shared;

#if CASE == 13
__attribute__((optnone))
#endif
void *obtain(size_t bytes) {
#if CASE == 5
  if (bytes == 0) return 0;
#endif
#if CASE == 4
  shared = 3;
#endif
#if CASE == 12
  void *p = calloc(bytes, 1);
#else
  void *p = malloc(bytes);
#endif
#if CASE != 1
  if (!p) abort();
#endif
  return p;
}

void *worker(void *unused) {
#if CASE == 14
  void *p = obtain(16);
  if (!p) return 0;
#endif
  shared = 1;
  return 0;
}

#if CASE >= 24
int main(void) {
  pthread_t t;
#if CASE == 26
  void *p = 0;
#else
  void *p = malloc(16);
  if (!p) return 0;
#endif
#if CASE == 25
  void *q = realloc(p, 0);
#else
  void *q = realloc(p, 32);
#endif
#if CASE != 27
  if (q) return 0;
#endif
  pthread_create(&t, 0, worker, 0);
  shared = 2;
  pthread_join(t, 0);
  return 0;
}
#elif CASE >= 22
struct Box { int *target; long long value; };
int main(void) {
  struct Box *saved = 0;
  for (int i = 0; i < 2; ++i) {
#if CASE == 22
    struct Box *p = malloc(sizeof(struct Box));
#else
    struct Box *p = calloc(1, sizeof(struct Box));
#endif
    if (i == 0) {
      if (!p) return 0;
      p->target = &shared;
      p->value = 1;
      saved = p;
    } else {
      if (p) { free(p); free(saved); return 0; }
      if (saved->value != 1) return 0;
      pthread_t t;
      if (pthread_create(&t, 0, worker, 0) != 0) {
        free(saved);
        return 0;
      }
      *saved->target = 2;
      pthread_join(t, 0);
      free(saved);
    }
  }
  return 0;
}
#else
int main(void) {
  pthread_t t;
#if CASE == 7
  pthread_create(&t, 0, worker, 0);
#endif
  void *p;
#if CASE == 18 || CASE == 21
  p = malloc(16);
#elif CASE == 19
  p = malloc(0);
#elif CASE == 20
  extern unsigned __VERIFIER_nondet_uint(void);
  unsigned bytes = __VERIFIER_nondet_uint();
  if (bytes > 1024) return 0;
  p = malloc(bytes);
#elif CASE == 3 || CASE == 5
  p = obtain(0);
#elif CASE == 6
  for (int i = 0; i < 2; ++i) p = obtain(16);
#elif CASE == 10
  p = obtain((size_t)((1ULL << 32) + 4));
#elif CASE == 11
  size_t count = 2;
  if (count > SIZE_MAX / sizeof(long long)) abort();
  p = obtain(count * sizeof(long long));
#else
  p = obtain(16);
#endif
#if CASE == 21
  *(int *)p = 1;
  if (p) return 0;
#elif CASE == 1 || CASE == 2 || CASE == 18 || CASE == 19 || CASE == 20
  if (p) return 0;
#elif CASE == 8
  free(p);
  if (!p) return 0;
#elif CASE == 9
  if ((char *)p + 1 == 0) return 0;
#elif CASE == 15
  if (*(int *)p) return 0;
#elif CASE == 16
  p = 0;
  if (p) return 0;
#elif CASE == 17
  void *copy = p;
  p = 0;
  if (!copy) return 0;
#endif
#if CASE != 7
  pthread_create(&t, 0, worker, 0);
#endif
  shared = 2;
  pthread_join(t, 0);
  return 0;
}

#endif
