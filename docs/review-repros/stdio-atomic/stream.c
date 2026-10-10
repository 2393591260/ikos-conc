#include <pthread.h>
#include <stdio.h>
extern void __VERIFIER_atomic_begin(void);
extern void __VERIFIER_atomic_end(void);
int data;
void *writer(void *u) {__VERIFIER_atomic_begin();data=1;puts("first");__VERIFIER_atomic_end();return 0;}
void *reader(void *u) {puts("second");volatile int sink=data; (void)sink;return 0;}
int main(void) {pthread_t a,b;pthread_create(&a,0,writer,0);pthread_create(&b,0,reader,0);return 0;}
