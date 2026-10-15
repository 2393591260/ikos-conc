// SPDX-License-Identifier: MIT
// Per-instance initialization must not become a must-alias allocation proof.
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long payload;
#else
typedef int payload;
#endif
extern int __VERIFIER_nondet_int();
void require(int value) { if (!value) abort(); }
extern void opaque(void *);
struct node { payload datum; struct node *next; };
struct node *slots[4], *escaped, *victim;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t other_lock = PTHREAD_MUTEX_INITIALIZER;

struct node *make_node(int value) {
  struct node *p = malloc(sizeof *p);
#if CASE == 3
  escaped = p; // An externally observable instance precedes initialization.
#elif CASE == 4
  opaque(p);
#elif CASE == 5
  p->datum = (uintptr_t)p; // Reject pointer-to-integer escape.
#endif
  p->datum = value;
#if CASE == 6
  if (__VERIFIER_nondet_int()) p->next = 0;
#elif CASE == 7
  return (struct node *)((char *)p + sizeof(payload));
#elif CASE == 8
  p->next = victim; // Not the closed NULL-initializing constructor subset.
#else
  p->next = 0;
#endif
  return p;
}
struct node *other_node(void) {
  struct node *p = malloc(sizeof *p);
  p->datum = 0; p->next = victim;
  return p;
}
void insert(struct node *p, struct node *head) {
  struct node *old = head->next;
  head->next = p;
  p->next = old;
}
void *worker(void *arg) {
  int i = __VERIFIER_nondet_int();
  require(i >= 0 && i < 4);
  pthread_mutex_lock(&lock);
  slots[i]->next->datum++;
  insert(make_node(3), slots[i]);
  slots[i]->next->datum++;
  pthread_mutex_unlock(&lock);
  return 0;
}
int main(void) {
  pthread_t t;
  victim = malloc(sizeof *victim);
  victim->datum = 0; victim->next = 0;
  for (int k = 0; k < 4; ++k) {
    slots[k] = make_node(1);
    insert(make_node(2), slots[k]);
  }
#if CASE == 9
  slots[0] = other_node(); // One uncertified source must disable the whole read.
#endif
  pthread_create(&t, 0, worker, 0);
#if CASE == 2
  slots[0]->next = victim; // Keep post-publication pointer writes.
#elif CASE == 10
  ((unsigned char *)&slots[0]->next)[0] = 0;
#elif CASE == 11
  memcpy(&slots[0]->next, &victim, sizeof victim);
#elif CASE == 12
  opaque(&slots[0]->next);
#elif CASE == 13
  slots[0]->next = slots[1]->next;
#endif
#if CASE == 1 || CASE == 8 || CASE == 9 || CASE == 13
  pthread_mutex_lock(&other_lock);
#else
  pthread_mutex_lock(&lock);
#endif
#if CASE == 9
  victim->datum++;
#else
  int j = __VERIFIER_nondet_int();
  require(j >= 0 && j < 4);
  slots[j]->next->datum++;
#endif
#if CASE == 1 || CASE == 8 || CASE == 9 || CASE == 13
  pthread_mutex_unlock(&other_lock);
#else
  pthread_mutex_unlock(&lock);
#endif
  return 0;
}
