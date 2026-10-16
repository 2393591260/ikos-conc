// SPDX-License-Identifier: MIT
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#ifndef CASE
#define CASE 0
#endif
#ifndef COUNT
#define COUNT 3
#endif
#ifdef WIDE
typedef long long payload;
#else
typedef int payload;
#endif
extern int __VERIFIER_nondet_int(void);
extern void opaque(void);
struct node { payload datum; struct node *next; };
struct node *head, *escaped;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void initialize(struct node *p, payload value) {
#if CASE == 5
  p = head;
#elif CASE == 6
  escaped = p;
#endif
  p->datum = value;
#if CASE == 7
  if (__VERIFIER_nondet_int())
#endif
#if CASE != 14
  p->next = 0;
#endif
#if CASE == 15
  ((unsigned short *)&p->next)[0] = 0;
#endif
}

void insert(struct node *p) {
  struct node *old = head->next;
  head->next = p;
  p->next = old;
}

void *worker(void *arg) {
  (void)arg;
  pthread_mutex_lock(&lock);
  for (int k = 0; k < 2; ++k) {
    struct node *p = malloc(sizeof *p);
#if CASE == 1
    struct node *before = p->next;
#elif CASE == 3
    escaped = p;
#elif CASE == 4
    opaque();
#elif CASE == 8
    struct node *saved = p;
    p = malloc(sizeof *p);
    initialize(saved, 1);
#elif CASE == 17
    escaped = (struct node *)(unsigned long long)(unsigned long)p;
#endif
#if CASE == 2
    if (__VERIFIER_nondet_int()) initialize(p, 1);
#elif CASE == 13
    p->datum = 1;
    p->next = 0;
#elif CASE != 8
    initialize(p, 1);
#endif
#if CASE == 21
    void *scratch = malloc(4);
    free(scratch);
#endif
#if CASE == 1
    if (before) before->datum++;
#elif CASE == 9
    ((unsigned char *)&p->next)[0] = 0;
#elif CASE == 10
    memcpy(&p->next, &head, sizeof head);
#elif CASE == 11
    p->next = escaped;
#elif CASE == 12
    p->next = p;
#elif CASE == 18
    struct node *raw = malloc(sizeof *raw);
    insert(raw);
#endif
    insert(p);
  }
  struct node *p = head;
  while (p->next) p = p->next;
#if CASE == 16
  pthread_mutex_unlock(&lock);
  p->datum++;
  pthread_mutex_lock(&lock);
#else
  p->datum++;
#endif
  pthread_mutex_unlock(&lock);
  return 0;
}

int main(void) {
#if CASE != 20
  head = malloc(sizeof *head);
#if CASE != 19
  initialize(head, 0);
#endif
#endif
  pthread_t handles[COUNT];
  for (int k = 0; k < COUNT; ++k) {
#if CASE == 20
    // Later iterations already have observers: this is not a startup relay.
    head = malloc(sizeof *head);
    initialize(head, 0);
#endif
    pthread_create(&handles[k], 0, worker, 0);
  }
#if CASE == 19
  initialize(head, 0);
#endif
  pthread_mutex_lock(&lock);
  struct node *p = head;
  while (p->next) p = p->next;
  p->datum++;
  pthread_mutex_unlock(&lock);
  for (int k = 0; k < COUNT; ++k)
    pthread_join(handles[k], 0);
  return 0;
}
