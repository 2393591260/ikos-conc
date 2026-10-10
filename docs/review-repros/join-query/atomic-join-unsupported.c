#include <pthread.h>
extern void __VERIFIER_atomic_begin(void);
extern void __VERIFIER_atomic_end(void);
int data;
void *worker(void *unused) {data=1;return 0;}
void *idle(void *unused) {return 0;}
int main(void) {pthread_t t,other;pthread_create(&t,0,worker,0);__VERIFIER_atomic_begin();pthread_create(&other,0,idle,0);pthread_join(other,0);data=2;__VERIFIER_atomic_end();return 0;}
