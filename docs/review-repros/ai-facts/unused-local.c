#include <pthread.h>
extern unsigned char __VERIFIER_nondet_uchar(void);
int data;
void *worker(void *unused) {volatile int sink=(int)__VERIFIER_nondet_uchar()+1;data=1;return 0;}
int main(void) {pthread_t t;pthread_create(&t,0,worker,0);data=2;return 0;}
