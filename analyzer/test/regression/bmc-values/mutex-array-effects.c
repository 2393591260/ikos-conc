// Independent range certificates must not hide invalid or escaping effects.
#include <pthread.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum_type;
#else
typedef int datum_type;
#endif
#define COUNT 3
extern int __VERIFIER_nondet_int();
struct element { datum_type value; struct element *next; };
struct element *heads[COUNT];
pthread_mutex_t guards[COUNT];
int shared_index;
#if CASE == 12
volatile int control_effect;
#endif
#if CASE == 14
void *numeric_pointer = (void *)1;
#endif
#if CASE == 3
extern void opaque_effect(void);
#endif
#if CASE == 8
int pthread_mutex_lock(pthread_mutex_t *p) {
  (void)p;
  heads[0] = heads[1];
  return 0;
}
#endif
void require_condition(int value) { if (!value) abort(); }
int index_value(void) {
  int value = __VERIFIER_nondet_int();
  require_condition(value >= 0 && value < COUNT);
  return value;
}
struct element *make_element(datum_type value) {
  struct element *p = malloc(sizeof(*p));
  p->value = value;
  p->next = NULL;
  return p;
}
void append(struct element *p, struct element *head) {
  struct element *old = head->next;
  head->next = p;
  p->next = old;
}
void *entry(void *unused) {
  (void)unused;
  int key = __VERIFIER_nondet_int();
#if CASE != 4
  require_condition(key >= 0 && key < COUNT);
#endif
#if CASE == 5
  key = shared_index;
  require_condition(key >= 0 && key < COUNT);
#endif
#if CASE == 10
  // Control-dependent indices must include both branches in the range proof.
  if (heads[0]->next) key = COUNT; else key = 0;
#elif CASE == 11
  if (__VERIFIER_nondet_int()) {
    heads[key]->next->value = 9;
    abort();
  }
#endif
#if CASE == 16
  // The same site first sees valid iterations and then an invalid leaf.
  for (key = 0; key <= COUNT; ++key) {
#endif
#if CASE == 6
  pthread_mutex_t *lock = (pthread_mutex_t *)((char *)guards + 1);
#else
  pthread_mutex_t *lock = &guards[key];
#endif
  pthread_mutex_lock(lock);
#if CASE == 2
  heads[key]->next = heads[(key + 1) % COUNT];
#endif
  append(make_element(3), heads[key]);
  pthread_mutex_unlock(lock);
#if CASE == 16
  }
#endif
  return NULL;
}
int main(void) {
#if CASE == 1
  int first = index_value();
#else
  int first = __VERIFIER_nondet_int();
  require_condition(first >= 0 && first < COUNT);
#endif
  for (int i = 0; i < COUNT; ++i)
    pthread_mutex_init(&guards[i], NULL);
  for (int i = 0; i < COUNT; ++i) {
    heads[i] = make_element(1);
    append(make_element(2), heads[i]);
  }
#if CASE == 3
  opaque_effect();
#elif CASE == 7
  // One wrong-context use invalidates the storage certificate globally.
  pthread_mutex_init((pthread_mutex_t *)&heads[0], NULL);
#endif
  pthread_t child;
#if CASE == 13
  int status = pthread_create(&child, NULL, entry, NULL);
  if (status) first = COUNT;
#else
  pthread_create(&child, NULL, entry, NULL);
#endif
#if CASE == 15
  pthread_t another;
  pthread_create(&another, NULL, entry, NULL);
#endif
#if CASE == 12
  // Keep a PHI selected by an unsupported native handle representation.
  if ((unsigned)child == 1) { control_effect = 1; first = 0; }
  else { control_effect = 2; first = COUNT; }
#endif
#if CASE != 9
  pthread_mutex_lock(&guards[first]);
#endif
  datum_type result = heads[first]->next->value;
#if CASE != 9
  pthread_mutex_unlock(&guards[first]);
#endif
  return (int)result;
}
