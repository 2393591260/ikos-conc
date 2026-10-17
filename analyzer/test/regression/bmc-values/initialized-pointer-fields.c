// Startup pointer fields retain their actual last complete store occurrence.
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
struct cell { datum value; struct cell *next; };
struct cell *left, *right;
pthread_mutex_t first = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t second = PTHREAD_MUTEX_INITIALIZER;
void initialize(struct cell *p, int value) {
  p->value = value;
  p->next = NULL;
}
void *worker(void *ignored) {
  pthread_mutex_lock(&first);
#if CASE == 4
  left->next = right->next;
#endif
#if CASE == 5
  ((unsigned char *)&left->next)[0] = 0;
#endif
#if CASE == 8
  extern void opaque(void *);
  opaque(&left->next);
#endif
#if CASE == 13
  left->next->next->value++;
#else
  left->next->value++;
#endif
  pthread_mutex_unlock(&first);
  return NULL;
}
int main(void) {
  pthread_t thread;
  struct cell *p = malloc(sizeof(*p));
  struct cell *q = malloc(sizeof(*q));
  left = malloc(sizeof(*left));
  right = malloc(sizeof(*right));
  initialize(p, 0);
  initialize(q, 0);
  initialize(left, 0);
  left->next = p;
#if CASE == 2 || CASE == 6
  initialize(right, 0);
  right->next = q;
#else
  initialize(right, 0);
  right->next = p;
#endif
#if CASE == 3
  left->next = NULL;
#endif
#if CASE == 7
  struct cell *saved = left->next;
  left->next = q;
  right->next = saved;
#endif
#if CASE == 10
  ((unsigned char *)&left->next)[0] = 0;
#endif
#if CASE == 11
  left->next = q;
  left->next = p;
#endif
#if CASE == 13
  left->next = right;
#endif
#if CASE == 14
  left->next = (struct cell *)((char *)p + 1);
#endif
#if CASE == 15
  pthread_create((pthread_t *)&left->next, NULL, worker, NULL);
#else
  pthread_create(&thread, NULL, worker, NULL);
#endif
#if CASE == 16
  pthread_join(thread, (void **)&left->next);
#endif
#if CASE == 12
  left->next = p;
#endif
#if CASE == 1
  pthread_mutex_lock(&first);
#else
  pthread_mutex_lock(&second);
#endif
#if CASE == 6
  right->next = p;
#endif
#if CASE == 9
  free(p);
#endif
  right->next->value++;
#if CASE == 1
  pthread_mutex_unlock(&first);
#else
  pthread_mutex_unlock(&second);
#endif
  return 0;
}
