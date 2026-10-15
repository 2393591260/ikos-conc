// Indexed ownership and its failure boundaries. CASE is selected by the runner.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#ifndef CASE
#define CASE 0
#endif
#define N 3
#ifdef WIDE
typedef long long datum_type;
#else
typedef int datum_type;
#endif
struct node { datum_type value; struct node *next; };
pthread_mutex_t guards[N];
struct node *roots[N];
#if CASE == 31
struct node foreign;
#elif CASE == 37
extern long long wrong_lock(pthread_mutex_t *) __asm__("pthread_mutex_lock");
#define pthread_mutex_lock wrong_lock
#endif
#if CASE == 5 || CASE == 15
extern void opaque_effect(struct node **);
#endif
#if CASE == 16
int pthread_mutex_lock(pthread_mutex_t *p) { (void)p; return 0; }
int pthread_mutex_unlock(pthread_mutex_t *p) { (void)p; return 0; }
#endif
struct node *construct(datum_type value) {
#if CASE == 14
  struct node *p = malloc(sizeof(*p) - 1);
#else
  struct node *p = malloc(sizeof(*p));
#endif
  p->value = value;
  p->next = NULL;
#if CASE == 15
  opaque_effect(&p);
#endif
  return p;
}
struct node *identity(struct node *p) { return p; }
void insert(struct node *p, struct node *head) {
  struct node *old = head->next;
  head->next = p;
  p->next = old;
}
void *worker(void *unused) {
  (void)unused;
  for (int i = 0; i < N; ++i) {
#if CASE == 1
    int lock_index = (i + 1) % N;
#elif CASE == 18
    int lock_index = N;
#else
    int lock_index = i;
#endif
#if CASE == 34
    pthread_mutex_lock((pthread_mutex_t *)((char *)&guards[lock_index] + 1));
#elif CASE != 28 && CASE != 29 && CASE != 39
    pthread_mutex_lock(&guards[lock_index]);
#endif
#if CASE == 9
    int saved = i;
    i = (i + 1) % N;
#endif
#if CASE == 39
    if (roots[i]) roots[i]->value += 1;
#elif CASE >= 25 && CASE != 40
    roots[i]->value += 1;
#elif CASE == 4
    insert(roots[(i + 1) % N], roots[i]);
#else
    struct node *p = construct(i);
#if CASE == 20
    p = identity(p);
#elif CASE == 21
    struct node *q;
    if (i & 1) q = p; else q = p;
    p = q;
#elif CASE == 22
    if (i & 1) p = roots[(i + 1) % N];
#endif
    insert(p, roots[i]);
#if CASE == 40
    struct node *alias = p;
    insert(alias, roots[(i + 1) % N]);
#endif
#endif
#if CASE == 8
    ((unsigned char *)&roots[i]->next)[1] = 0;
#elif CASE == 33
    *(long long *)&roots[i]->next = 0;
#elif CASE == 13
    free(roots[i]);
#endif
#if CASE == 9
    i = saved;
#endif
#if CASE != 28 && CASE != 29 && CASE != 39
    pthread_mutex_unlock(&guards[lock_index]);
#endif
  }
  return NULL;
}
int main(void) {
  pthread_t thread;
  for (int i = 0; i < N; ++i) {
    pthread_mutex_init(&guards[i], NULL);
#if CASE == 41
    struct node *p = construct(i), *alias = p;
    roots[i] = p;
    roots[(i + 1) % N] = alias;
#elif CASE == 39
    if (i == 1) roots[i] = NULL;
    else roots[i] = construct(i);
#elif CASE == 10
    struct node *a = construct(i), *b = construct(i);
    a->next = b;
    roots[i] = a;
#elif CASE == 11
    struct node *a = construct(i);
    a->next = a;
    roots[i] = a;
#else
    roots[i] = construct(i);
#endif
  }
#if CASE == 3
  roots[1] = roots[0];
#elif CASE == 6
  memcpy(&roots[0], &roots[1], sizeof(roots[0]));
#elif CASE == 30
  roots[0] = (struct node *)4;
#elif CASE == 31
  roots[0] = &foreign;
#endif
#if CASE == 35
  pthread_create((pthread_t *)((char *)&thread + 1), NULL, worker, NULL);
#elif CASE == 36
  pthread_create(&thread, NULL, worker, (void *)1);
#else
  pthread_create(&thread, NULL, worker, NULL);
#endif
#if CASE == 5
  opaque_effect(roots);
#elif CASE == 7
  roots[0] = roots[1];
#elif CASE == 12
  pthread_mutex_init(&guards[0], NULL);
#endif
  datum_type sum = 0;
  for (int i = 0; i < N; ++i) {
#if CASE == 25 || CASE == 39
    int lock_index = 1;
#elif CASE == 2
    int lock_index = (i + 1) % N;
#else
    int lock_index = i;
#endif
#if CASE != 26 && CASE != 29
    pthread_mutex_lock(&guards[lock_index]);
#endif
#if CASE == 40
    struct node *p = roots[i]->next;
#elif CASE < 25
    insert(construct(i), roots[i]);
    struct node *p = roots[i]->next;
#else
    struct node *p = roots[i];
#endif
#if CASE == 40
    if (p) sum += p->next != NULL;
#elif CASE == 39
    if (p) sum += p->value;
#else
    sum += p->value;
#endif
#if CASE != 26 && CASE != 29
    pthread_mutex_unlock(&guards[lock_index]);
#endif
  }
  return (int)sum;
}
