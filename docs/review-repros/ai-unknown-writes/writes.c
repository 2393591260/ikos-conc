#include <assert.h>
#include <pthread.h>
#include <stdlib.h>

#ifndef CASE
#define CASE 0
#endif

static int left, right;
static int *selected = &left;
static int enabled;
static unsigned char byte_enabled;
static double double_enabled;
static pthread_mutex_t protection = PTHREAD_MUTEX_INITIALIZER;
struct node { int *value; };
static struct node *object;
static void left_write(void) { left = 1; }
static void right_write(void) { right = 1; }
static void (*dispatch)(void) = left_write;

#ifdef NATIVE_PREFIX
void *__VERIFIER_nondet_pointer(void) {
#if CASE == 3
  return &enabled;
#elif CASE == 4
  return &byte_enabled;
#elif CASE == 5
  return &object->value;
#elif CASE == 6
  return &dispatch;
#elif CASE == 9
  return &double_enabled;
#else
  return &selected;
#endif
}
#else
extern void *__VERIFIER_nondet_pointer(void);
#endif

static void rewrite(int **destination, int *value) { *destination = value; }
static void *first(void *unused) {
  (void)unused;
#if CASE == 3
  if (enabled) right = 1;
#elif CASE == 4
  if (byte_enabled) right = 1;
#elif CASE == 5
  *object->value = 1;
#elif CASE == 6
  dispatch();
#elif CASE == 9
  if (double_enabled == 1.0) right = 1;
#else
  *selected = 1;
#endif
  return 0;
}
static void *second(void *unused) {
  (void)unused;
  right = 2;
  return 0;
}

int main(void) {
  pthread_t a, b;
#if CASE == 0
  int **destination = __VERIFIER_nondet_pointer();
  *destination = &right;
#elif CASE == 1
  selected = &right;
#elif CASE == 3
  int *destination = __VERIFIER_nondet_pointer();
  *destination = 1;
#elif CASE == 4
  unsigned char *destination = __VERIFIER_nondet_pointer();
  *destination = 1;
#elif CASE == 5
  object = malloc(sizeof(*object));
  if (!object) return 0;
  object->value = &left;
  int **destination = __VERIFIER_nondet_pointer();
  *destination = &right;
#elif CASE == 6
  void (**destination)(void) = __VERIFIER_nondet_pointer();
  *destination = right_write;
#elif CASE == 7
  pthread_mutex_lock(&protection);
  int **destination = __VERIFIER_nondet_pointer();
  *destination = &right;
  pthread_mutex_unlock(&protection);
#elif CASE == 8
  rewrite(__VERIFIER_nondet_pointer(), &right);
#elif CASE == 9
  double *destination = __VERIFIER_nondet_pointer();
  *destination = 1.0;
#endif

#ifdef NATIVE_PREFIX
  // Execute a valid alias choice and check the prefix, without running races.
#if CASE == 3
  assert(enabled == 1);
#elif CASE == 4
  assert(byte_enabled == 1);
#elif CASE == 5
  assert(object->value == &right);
  free(object);
#elif CASE == 6
  assert(dispatch == right_write);
#elif CASE == 9
  assert(double_enabled == 1.0);
#elif CASE == 2
  assert(selected == &left);
#else
  assert(selected == &right);
#endif
  return 0;
#else
  pthread_create(&a, 0, first, 0);
  pthread_create(&b, 0, second, 0);
  pthread_join(a, 0);
  pthread_join(b, 0);
  return 0;
#endif
}
