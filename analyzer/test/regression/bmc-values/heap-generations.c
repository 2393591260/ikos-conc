// SPDX-License-Identifier: MIT
// One actual main malloc site, multiple executed allocation generations.
#include <pthread.h>
#include <stdlib.h>
#ifndef MODE
#define MODE 0
#endif
#if MODE == 4
#define GENERATIONS 3
#else
#define GENERATIONS 2
#endif
#ifdef WIDE
typedef long long element;
#else
typedef int element;
#endif
static void update(element *p, element v) { *p = v; }
static void *writer(void *argument) {
  element *p = argument;
#if MODE == 5
  update(p, 1); update(p, 2);
#else
  *p = 1;
#endif
  return 0;
}
static void *reader(void *argument) {
  element value = *(element *)argument;
  if (value == 99) abort();
  return 0;
}
#if MODE == 13
int main(void) {
  typedef struct { pthread_t handles[2]; element value; } bundle;
  bundle *selected = 0;
  for (unsigned generation = 0; generation < 2; ++generation) {
    bundle *p = malloc(sizeof(bundle));
    if (!p) abort();
    p->value = 0;
    selected = p;
  }
  // The actual create outputs live in the second dynamic allocation.
  pthread_create(&selected->handles[0], 0, writer, &selected->value);
  pthread_create(&selected->handles[1], 0, reader, &selected->value);
  pthread_join(selected->handles[0], 0);
  pthread_join(selected->handles[1], 0);
  return 0;
}
#else
int main(void) {
  element *first = 0, *selected = 0;
  for (unsigned i = 0; i < GENERATIONS; ++i) {
    element *p = malloc((i + 1) * sizeof(element));
    if (!p) abort();
    p[0] = 0;
    p[i] = 0;
    if (i == 0) {
      first = p;
#if MODE == 1 || MODE == 3
      free(p);
#elif MODE == 6
      free(p + 1);
#elif MODE == 7
      free(p); free(p);
#endif
    }
    selected = p;
  }
#if MODE == 9
  selected = realloc(selected, GENERATIONS * sizeof(element));
  if (!selected) abort();
#endif
  pthread_t a, b;
#if MODE == 12
  pthread_create(&a, 0, writer, selected);
#else
  pthread_create(&a, 0, writer, selected + GENERATIONS - 1);
#endif
#if MODE == 8
  free(selected);
#endif
#if MODE == 2 || MODE == 3
  pthread_create(&b, 0, reader, first);
#elif MODE == 10 || MODE == 12
  pthread_create(&b, 0, reader, selected);
#else
  pthread_create(&b, 0, reader, selected + GENERATIONS - 1);
#endif
  pthread_join(a, 0);
  pthread_join(b, 0);
  return 0;
}

#endif
