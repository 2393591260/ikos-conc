#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
static int value;
static void *inert(void *arg) { return 0; }
static void *writer(void *arg) { value=1; return 0; }
int main(void) { pthread_t a,b,chosen=0; pthread_create(&a,0,inert,0); pthread_create(&b,0,writer,0); if(__VERIFIER_nondet_int()) chosen=a; else if(__VERIFIER_nondet_int()) chosen=b; pthread_join(chosen,0); value=2; return 0; }
