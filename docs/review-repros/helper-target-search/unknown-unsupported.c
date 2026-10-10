#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
int data;
extern void unknown_effect(void);
void initialize(void) {unknown_effect();}
void *worker(void *arg) {initialize();data=1;return 0;}
int main(void) {
 pthread_t a,b;
 pthread_create(&a,0,worker,0);pthread_create(&b,0,worker,0);
 pthread_join(a,0);pthread_join(b,0);return 0;
}
