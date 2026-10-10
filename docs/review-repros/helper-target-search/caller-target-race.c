#include <pthread.h>
#include <stdio.h>
extern int __VERIFIER_nondet_int(void);
int data;
void choose(void) { if(__VERIFIER_nondet_int()) puts("left"); else puts("right"); }
void *worker(void *arg) {choose();data=2;return 0;}
int main(void) {
 pthread_t a,b;
 pthread_create(&a,0,worker,0);pthread_create(&b,0,worker,0);
 pthread_join(a,0);pthread_join(b,0);return 0;
}
