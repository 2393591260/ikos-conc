// Range premises must include memory-controlled branches in every context.
#include <pthread.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
extern int __VERIFIER_nondet_int(void);
struct box { datum *target; };
struct box *root;
datum integer_guard;
unsigned char byte_guard;
double float_guard;
datum *pointer_guard;
datum *pointer_copy;
datum *free_pointer = &integer_guard;
struct aggregate { datum first, second; } aggregate_guard;
// Even cases contain an invalid-range pressure branch; odd cases have two leaves.
pthread_mutex_t locks[1 + (CASE % 2)];
static struct box *construct(void) {
  struct box *p = malloc(sizeof *p);
  p->target = NULL;
  return p;
}
static int read_condition(void) { return integer_guard != 0; }
#if CASE == 18
static int recurse(void) {
  return __VERIFIER_nondet_int() ? recurse() : read_condition();
}
#elif CASE == 19
extern void invoke_callback(void (*callback)(void));
static void change_guard(void) { integer_guard = 1; }
#endif
static void *writer(void *unused) {
#if CASE < 2
  *root->target = 1;
#elif CASE < 4
  byte_guard = 1;
#elif CASE < 6
  pointer_guard = &integer_guard;
#elif CASE < 8
  float_guard = 1.0;
#elif CASE < 10
  *root->target = 1;
#elif CASE < 12
  aggregate_guard.first = 1;
#elif CASE < 14
  pointer_guard = &integer_guard;
#else
  *root->target = 1;
#endif
  return NULL;
}
static void use_range(void) {
  int condition;
#if CASE < 2
  condition = integer_guard != 0;
#elif CASE < 4
  condition = byte_guard != 0;
#elif CASE < 6
  condition = pointer_guard != NULL;
#elif CASE < 8
  condition = float_guard != 0.0;
#elif CASE < 10
  condition = read_condition();
#elif CASE < 12
  struct aggregate copy = aggregate_guard;
  condition = copy.first != 0;
#elif CASE < 14
  datum *p = pointer_guard;
  pointer_copy = p;
  condition = p != NULL;
#elif CASE == 18
  condition = recurse();
#else
  condition = read_condition();
#endif
  int index;
  if (condition) { (void)__VERIFIER_nondet_int(); index = 1; }
  else index = 0;
  pthread_mutex_lock(&locks[index]);
  pthread_mutex_unlock(&locks[index]);
}
static void *reader(void *unused) { use_range(); return NULL; }
int main(void) {
  pthread_t first, second;
  root = construct();
  root->target = &integer_guard;
#if CASE == 19
  invoke_callback(change_guard);
#endif
#if CASE >= 16
  struct box *temporary = construct();
  temporary->target = malloc(sizeof(datum));
  free_pointer = temporary->target;
  free(free_pointer); // Actual value is heap or NULL, never the old static target.
#endif
#if CASE >= 14
  use_range(); // The same query is also visited before thread creation.
#endif
  pthread_create(&first, NULL, writer, NULL);
  pthread_create(&second, NULL, reader, NULL);
  return 0;
}
