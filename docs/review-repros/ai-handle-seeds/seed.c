#include <pthread.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#ifndef CASE
#define CASE 0
#endif
extern unsigned long __VERIFIER_nondet_ulong(void);
extern void *__VERIFIER_nondet_pointer(void);
struct node { struct node *next; int value; };
static struct node *head;
static pthread_mutex_t protection = PTHREAD_MUTEX_INITIALIZER;
static int shared;
static void initialize(struct node *p, int n) { p->value=n; p->next=0; }
static void *worker(void *arg) {
#if CASE == 4
  volatile unsigned long raw = *(pthread_t *)arg;
  (void)raw;
#else
  (void)arg;
#endif
  struct node *p=malloc(sizeof *p);
  initialize(p,7);
  int *member=&p->value;
  struct node *owner=(struct node *)((char *)member - offsetof(struct node,value));
#if CASE != 1 && CASE != 2
  pthread_mutex_lock(&protection);
#endif
  owner->next=head->next;
  head->next=owner;
  shared=1;
#if CASE != 1 && CASE != 2
  pthread_mutex_unlock(&protection);
#endif
  return 0;
}
int main(void) {
  pthread_t handle;
  struct node *p=malloc(sizeof *p);
  initialize(p,9);
  head=malloc(sizeof *head);
  initialize(head,3);
  head->next=p;
  pthread_create(&handle,0,worker,
#if CASE == 4
    &handle
#else
    0
#endif
  );
#if CASE == 2
  if(handle!=(pthread_t)1)shared=2;
#elif CASE == 3
  handle=(pthread_t)1;
#elif CASE == 5
  pthread_t copy; memcpy(&copy,&handle,sizeof copy);
  volatile unsigned long observed=copy;
  (void)observed;
#elif CASE == 6
  volatile unsigned long observed=*(volatile unsigned long *)__VERIFIER_nondet_pointer();
  (void)observed;
#elif CASE == 7
  volatile union { uintptr_t bits; void *pointer; } forged;
  forged.bits=(uintptr_t)__VERIFIER_nondet_ulong();
  volatile unsigned long observed=*(volatile unsigned long *)forged.pointer;
  (void)observed;
#elif CASE == 8
  uintptr_t bits=(uintptr_t)__VERIFIER_nondet_ulong(); void *forged;
  for(unsigned k=0;k<sizeof forged;++k)((unsigned char *)&forged)[k]=((unsigned char *)&bits)[k];
  volatile unsigned long observed=*(volatile unsigned long *)forged;
  (void)observed;
#elif CASE == 9
  struct holder { void *pointer; } *forged=malloc(sizeof *forged);
  *(volatile uintptr_t *)&forged->pointer=(uintptr_t)__VERIFIER_nondet_ulong();
  volatile unsigned long observed=*(volatile unsigned long *)forged->pointer;
  (void)observed;
#elif CASE == 10
  void *forged; read(0,&forged,sizeof forged);
  volatile unsigned long observed=*(volatile unsigned long *)forged;
  (void)observed;
#elif CASE == 11
  *(volatile unsigned char *)&handle=1;
#endif
#if CASE != 1 && CASE != 2
  pthread_mutex_lock(&protection);
#endif
  shared=2;
  volatile int data_observed=p->value;
  (void)data_observed;
#if CASE != 1 && CASE != 2
  pthread_mutex_unlock(&protection);
#endif
  return 0;
}
