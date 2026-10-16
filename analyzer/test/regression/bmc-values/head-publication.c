// Old nodes may be shared; root heads and new worker publications may not.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#ifndef CASE
#define CASE 0
#endif
#if CASE == 18
#define N 11
#else
#define N 3
#endif
#ifdef WIDE
typedef long long datum_type;
#else
typedef int datum_type;
#endif
struct element { datum_type datum; struct element *next; };
struct element *heads[N];
pthread_mutex_t guards[N];
extern int __VERIFIER_nondet_int(void);
extern void opaque(struct element *);
static void bounded(int condition) { if (!condition) abort(); }
static struct element *make(datum_type x) {
#if CASE == 14
  struct element *p = malloc(sizeof(*p) - 1);
#else
  struct element *p = malloc(sizeof(*p));
#endif
  p->datum = x;
  p->next = NULL;
  return p;
}
static struct element *identity(struct element *p) { return p; }
static void insert(struct element *node, struct element *head) {
  struct element *old = head->next;
  head->next = node;
  node->next = old;
}
static void *worker(void *unused) {
  (void)unused;
  int i = __VERIFIER_nondet_int(); bounded(0 <= i && i < N);
#if CASE == 2
  int k = (i + 1) % N;
#else
  int k = i;
#endif
#if CASE != 3
  pthread_mutex_lock(&guards[k]);
#endif
#if CASE == 6
  // This may still be an old node shared by differently locked heads.
  heads[i]->next->datum += 1;
#endif
  struct element *p = make(3);
#if CASE == 17
  p = identity(p);
#elif CASE == 9
  opaque(p);
#endif
  insert(p, heads[i]);
#if CASE == 5
  insert(p, heads[(i + 1) % N]);
#elif CASE == 8
  ((unsigned char *)&heads[i]->next)[1] = 0;
#elif CASE == 10
  free(p);
#elif CASE == 12
  heads[i] = p;
#elif CASE == 16
  pthread_mutex_unlock(&guards[k]);
  pthread_mutex_lock(&guards[(i + 1) % N]);
  p->datum += 1;
  pthread_mutex_unlock(&guards[(i + 1) % N]);
  return NULL;
#endif
#if CASE != 3
  pthread_mutex_unlock(&guards[k]);
#endif
  return NULL;
}
int main(void) {
  pthread_t t;
  for (int i = 0; i < N; ++i) {
    pthread_mutex_init(&guards[i], NULL);
    heads[i] = make(1);
    insert(make(2), heads[i]);
  }
  int j = __VERIFIER_nondet_int(); bounded(0 <= j && j < N);
  int k = __VERIFIER_nondet_int(); bounded(0 <= k && k < N);
  struct element *p = make(3);
  insert(p, heads[j]);
  insert(p, heads[k]);
#if CASE == 4
  heads[1] = heads[0];
#elif CASE == 11
  insert(heads[0], heads[1]);
#elif CASE == 13
  heads[0] = (struct element *)((char *)heads[1] + 4);
#elif CASE == 19
  struct element *q = make(4);
  q->next = heads[0];
  insert(q, heads[1]);
#endif
  pthread_create(&t, NULL, worker, NULL);
#if CASE == 1
  pthread_mutex_lock(&guards[(j + 1) % N]);
#else
  pthread_mutex_lock(&guards[j]);
#endif
  p = heads[j]->next;
#if CASE == 7
  p = p->next;
#elif CASE == 15
  insert(make(9), heads[j]);
#elif CASE == 20
  p->next = heads[k];
#endif
  datum_type value = p->datum;
#if CASE == 1
  pthread_mutex_unlock(&guards[(j + 1) % N]);
#else
  pthread_mutex_unlock(&guards[j]);
#endif
  return (int)value;
}
