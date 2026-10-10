#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
#ifndef CASE
#define CASE 0
#endif
static int data;
static pthread_t global_left, global_right;
static void *empty(void *p) { (void)p; return 0; }
static void *work(void *p) { (void)p; data=1; return 0; }
static void create_at(pthread_t *slot) { pthread_create(slot,0,work,0); }
int main(void) {
 pthread_t local_left,local_right;
#if CASE == 6 || CASE == 7
 pthread_t *left=&global_left,*right=&global_right;
#else
 pthread_t *left=&local_left,*right=&local_right;
#endif
 pthread_create(left,0,empty,0);
 pthread_create(right,0,empty,0);
#if CASE == 2
 pthread_t *slot=left;
#elif CASE == 3
 pthread_t *slot=right;
#else
 pthread_t *slot=__VERIFIER_nondet_int()?left:right;
#endif
#if CASE == 4 || CASE == 5
 create_at(slot);
#else
 pthread_create(slot,0,work,0);
#endif
 pthread_join(*left,0);
#if CASE == 1 || CASE == 5 || CASE == 7
 pthread_join(*right,0);
#endif
 data=2;
#if CASE != 1 && CASE != 5 && CASE != 7
 pthread_join(*right,0);
#endif
 return 0;
}
