// SPDX-License-Identifier: MIT
// Pointer initialization is distinct from the summary of all possible writes.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long payload;
#else
typedef int payload;
#endif
extern int __VERIFIER_nondet_int(void);
extern void opaque(void *);
struct node { payload datum; struct node *next; };
struct node *first, *second;
pthread_mutex_t first_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t second_lock = PTHREAD_MUTEX_INITIALIZER;

void initialize(struct node *p) { p->datum = 0; p->next = 0; }
void redirect(struct node *p, struct node *target) { p->next = target; }

#if CASE == 10
void variadic(int count, ...) {
  va_list list;
  va_start(list, count);
  va_end(list);
}
#endif

void *worker(void *unused) {
  pthread_mutex_lock(&first_lock);
#if CASE == 3
  first->next = second->next;
#elif CASE == 13
  redirect(first, second->next);
#endif
  first->next->datum++;
  pthread_mutex_unlock(&first_lock);
  pthread_mutex_lock(&second_lock);
  second->next->datum++;
  pthread_mutex_unlock(&second_lock);
  return 0;
}

int main(void) {
  pthread_t thread;
#if CASE == 18
  struct node *nodes[2];
  for (int i = 0; i < 2; ++i) nodes[i] = malloc(sizeof *nodes[i]);
  struct node *left = nodes[0], *right = nodes[1];
#else
  struct node *left = malloc(sizeof *left);
  struct node *right = malloc(sizeof *right);
#endif
  first = malloc(sizeof *first);
  second = malloc(sizeof *second);
  initialize(left); initialize(right);
  initialize(first); initialize(second);
#if CASE != 16 && CASE != 17
  first->next = left;
#elif CASE == 17
  if (__VERIFIER_nondet_int()) first->next = left;
  else first->next = right;
#endif
  second->next = right;
#if CASE == 1
  second->next = left;
#elif CASE == 7
  if (__VERIFIER_nondet_int()) second->next = left;
#elif CASE == 8
  worker(0);
#elif CASE == 10
  variadic(0);
#elif CASE == 11
  first->datum = 3; second->datum = 4;
#elif CASE == 12
  pthread_create(&thread, 0, worker, 0);
#endif
  pthread_create(&thread, 0, worker, 0);
#if CASE == 4
  first->next = right;
#elif CASE == 5
  ((unsigned char *)&first->next)[0] = 0;
#elif CASE == 6
  opaque(&first->next);
#elif CASE == 14
  memcpy(&first->next, &right, sizeof right);
#elif CASE == 15
  pthread_create((pthread_t *)&first->next, 0, worker, 0);
#elif CASE == 16
  first->next = left;
#endif
#if CASE == 2
  pthread_mutex_lock(&second_lock);
  first->next->datum++;
  pthread_mutex_unlock(&second_lock);
#else
  pthread_mutex_lock(&first_lock);
  first->next->datum++;
  pthread_mutex_unlock(&first_lock);
#endif
  pthread_mutex_lock(&second_lock);
  second->next->datum++;
  pthread_mutex_unlock(&second_lock);
  return 0;
}
