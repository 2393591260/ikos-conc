// Bounds from normal-return guards feed the independent ownership certificate.
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
#if CASE == 7
extern long long wrong_nondet(void) __asm__("__VERIFIER_nondet_int");
#define choose() ((int)wrong_nondet())
#elif CASE == 8
extern int wrong_nondet(int) __asm__("__VERIFIER_nondet_int");
#define choose() wrong_nondet(1)
#else
#if CASE == 10
extern int __VERIFIER_nondet_int(void);
#else
extern int __VERIFIER_nondet_int();
#endif
#define choose() __VERIFIER_nondet_int()
#endif
#if CASE == 9
extern int wrong_abort(void) __asm__("abort");
#define stop() ((void)wrong_abort())
#else
#define stop() abort()
#endif
struct element { datum_type value; struct element *next; };
struct element *heads[COUNT];
pthread_mutex_t guards[COUNT];
#if CASE == 6
extern void opaque_effect(struct element **);
#endif
#if CASE == 14
extern int __ikos_nondet_int(void);
int __VERIFIER_nondet_int() {
  heads[0] = heads[1];
  return __ikos_nondet_int();
}
#endif
int copy_condition(int value) { return value; }
void require_condition(int condition) {
  if (!condition) {
#if CASE == 2
    return;
#else
    stop();
#endif
  }
}
#if CASE == 12
#define CONDITION(index) (index)
void bounded(int index) {
  require_condition(index >= 0 && index < COUNT);
}
#else
#if CASE == 1
#define CONDITION(index) ((index) >= 0 || (index) < COUNT)
#else
#define CONDITION(index) ((index) >= 0 && (index) < COUNT)
#endif
void bounded(int condition) {
#if CASE == 11
  require_condition(copy_condition(condition));
#else
  require_condition(condition);
#endif
}
#endif
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
  int key = choose();
  bounded(CONDITION(key));
#if CASE == 3
  key = choose();
#elif CASE == 13
  heads[key]->next->value = 9;
  abort();
#elif CASE == 15
  if (choose()) {
    heads[key]->next->value = 9;
    abort();
  }
#endif
#if CASE == 4
  int lock_key = (key + 1) % COUNT;
#else
  int lock_key = key;
#endif
  pthread_mutex_lock(&guards[lock_key]);
  append(make_element(key), heads[key]);
  pthread_mutex_unlock(&guards[lock_key]);
  return NULL;
}
int main(void) {
  for (int i = 0; i < COUNT; ++i) {
    pthread_mutex_init(&guards[i], NULL);
    heads[i] = make_element(i);
    append(make_element(i + 1), heads[i]);
  }
#if CASE == 5
  heads[1] = heads[0];
#elif CASE == 6
  opaque_effect(heads);
#endif
  int first = choose(), second = choose();
  bounded(CONDITION(first));
  bounded(CONDITION(second));
  append(make_element(4), heads[first]);
  append(make_element(5), heads[second]);
  pthread_t child;
  pthread_create(&child, NULL, entry, NULL);
  pthread_mutex_lock(&guards[first]);
  datum_type result = heads[first]->next->value;
  pthread_mutex_unlock(&guards[first]);
  return (int)result;
}
