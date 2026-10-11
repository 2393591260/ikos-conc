#include <pthread.h>
#include <stdint.h>
#ifndef CASE
#define CASE 0
#endif
extern int __VERIFIER_nondet_int(void);
struct cell { int selector, value; };
static int *published;
#if CASE == 17
int main(int, char **);
#endif
#if CASE == 16
struct node { struct node *next; int value; };
extern void unused_sink(void *);
void unused_recursive_type(void) { struct node n; unused_sink(&n); }
#endif
static void *left(void *p) {
#if CASE == 17
  main(0, 0); return 0;
#endif
#if CASE == 8
  int local = 0;
  p = &local;
#endif
#if CASE == 4 || CASE == 5 || CASE == 14 || CASE == 15
  struct cell *s = p;
  if (s->selector == 0) s->value = 1;
#elif CASE == 6
  *(int *)p = *(int *)p + 1;
#elif CASE == 9
  *published = 1;
#elif CASE == 13
  *((unsigned char *)p + 1) = 1;
#else
  *(int *)p = 1;
#endif
  return 0;
}
static void *right(void *p) {
#if CASE == 17
  main(0, 0); return 0;
#endif
#if CASE == 8
  int local = 0;
  p = &local;
#endif
#if CASE == 4 || CASE == 5 || CASE == 14 || CASE == 15
  struct cell *s = p;
#if CASE == 5 || CASE == 15
  if (s->selector != 0) s->value = 2;
#else
  if (s->selector == 0) s->value = 2;
#endif
#elif CASE == 9
  *published = 2;
#else
  *(int *)p = 2;
#endif
  return 0;
}
static void launch(void *p) {
  pthread_t a, b;
  pthread_create(&a, 0, left, p);
  pthread_create(&b, 0, right, p);
  pthread_join(a, 0); pthread_join(b, 0);
}
static void helper(void) {
  int value = 0;
  launch(&value);
}
#if CASE == 17
int main(int argc, char **argv) {
#else
int main(void) {
#endif
  int values[2];
  values[0] = 0; values[1] = 0;
#if CASE == 17
  if (argc == 0) return 0;
#endif
  struct cell data;
  data.selector = 0; data.value = 0;
#if CASE == 14 || CASE == 15
  data.selector = __VERIFIER_nondet_int();
#endif
#if CASE == 10
  helper();
#elif CASE == 11
  for (int i = 0; i < 2; ++i) helper();
#else
  pthread_t a, b;
  void *first = values, *second = values;
#if CASE == 1
  second = values + 1;
#elif CASE == 4 || CASE == 5 || CASE == 14 || CASE == 15
  first = second = &data;
#elif CASE == 9
  published = values;
#endif
  pthread_create(&a, 0, left, first);
#if CASE == 12
  pthread_join(a, 0);
#endif
#if CASE == 2
  values[0] = 2;
#elif CASE == 3
  values[1] = 2;
#else
  pthread_create(&b, 0, right, second);
  pthread_join(b, 0);
#endif
#if CASE != 12
  pthread_join(a, 0);
#endif
#endif
  return 0;
}
