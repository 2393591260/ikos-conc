#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
int data;
int selector;
void initialize(void) {selector=0;}
void *worker(void *arg) {if(selector!=0) data=1;return 0;}
int main(void) {
 pthread_t a,b;initialize();
 pthread_create(&a,0,worker,0);pthread_create(&b,0,worker,0);
 pthread_join(a,0);pthread_join(b,0);return 0;
}
