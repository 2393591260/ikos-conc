#include <pthread.h>
#include <stdatomic.h>
#ifndef CASE
#define CASE 0
#endif
extern int __VERIFIER_nondet_int(void);
#if CASE >= 10
#include <stdlib.h>
static int x,y,z;
static struct Cell { int *p[2]; } *cell;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
#if CASE == 12
#define SLOT 1
#else
#define SLOT 0
#endif
static void *publish(void *arg) {
  (void)arg;
  pthread_mutex_lock(&mutex);
#if CASE == 11
  cell->p[__VERIFIER_nondet_int() ? 0 : 1]=&y;
#elif CASE == 13
  cell->p[1]=&z;
#else
  cell->p[__VERIFIER_nondet_int() ? 0 : 1]=&z;
#endif
  pthread_mutex_unlock(&mutex);
  return 0;
}
static void *worker(void *arg) {
  (void)arg;
  pthread_mutex_lock(&mutex); cell->p[SLOT]=&x; pthread_mutex_unlock(&mutex);
  pthread_mutex_lock(&mutex);
#if CASE == 13
  int *p=cell->p[__VERIFIER_nondet_int() ? 0 : 1];
#else
  int *p=cell->p[SLOT];
#endif
#if CASE != 14
  pthread_mutex_unlock(&mutex);
#endif
  *p=1;
#if CASE == 14
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}
int main(void) {
  pthread_t a,b;
  cell=malloc(sizeof *cell); if(!cell) return 0;
  cell->p[0]=&x; cell->p[1]=&y;
  pthread_create(&a,0,publish,0); pthread_create(&b,0,worker,0);
#if CASE == 15
  pthread_join(a,0); pthread_join(b,0);
#endif
#if CASE == 14
  pthread_mutex_lock(&mutex);
#endif
  z=2;
#if CASE == 14
  pthread_mutex_unlock(&mutex);
#endif
#if CASE != 15
  pthread_join(a,0); pthread_join(b,0);
#endif
  free(cell); return 0;
}
#else

#if CASE == 2
static int y,x;
static int *right,*left;
#else
static int x,y;
static int *left,*right;
#endif
static int payload;
static _Atomic(int *) flag;
struct box { int *target; };
static struct box boxes[2];
static struct box *box_left,*box_right;
static void *worker(void *arg) {
#if CASE == 7
  atomic_store(&flag, &x);
  if (atomic_load(&flag) == 0) payload=1;
#elif CASE == 8
  _Atomic(int *) *cell = arg;
  atomic_store(cell, &x);
  if (atomic_load(cell) == 0) payload=1;
#elif CASE == 9
  _Atomic(int *) private_flag;
  atomic_store(&private_flag, &x);
  if (atomic_load(&private_flag) == 0) payload=1;
#elif CASE == 3
  struct box *box = __VERIFIER_nondet_int()?box_left:box_right;
  *box->target = 2;
#elif CASE == 4
  *right = 2;
#else
  int **pick=__VERIFIER_nondet_int()?&left:&right;
  int *target=*pick;
  *target=2;
#endif
  return 0;
}
static void *reset(void *arg) {
#if CASE == 8
  atomic_store((_Atomic(int *) *)arg, 0);
#else
  atomic_store(&flag, 0);
#endif
  payload=2;
  return 0;
}
int main(void) {
  left=&x;right=&y;
  boxes[0].target=&x;boxes[1].target=&y;
  box_left=&boxes[0];box_right=&boxes[1];
#if CASE == 6
  left=&y;
#endif
  pthread_t t,u;
#if CASE == 8
  _Atomic(int *) stack_flag=0;
  void *arg=&stack_flag;
#else
  void *arg=0;
#endif
  pthread_create(&t,0,worker,arg);
#if CASE >= 7
  pthread_create(&u,0,reset,arg);
#else
#if CASE == 5
  pthread_join(t,0);
#endif
#if CASE == 1
  y=1;
#else
  x=1;
#endif
#endif
#if CASE != 5
  pthread_join(t,0);
#endif
#if CASE >= 7
  pthread_join(u,0);
#endif
  return 0;
}

#endif
