#include <pthread.h>
extern void __VERIFIER_assume(int);
extern void __VERIFIER_atomic_begin(void);
extern void __VERIFIER_atomic_end(void);
volatile int payload, flag;
void *writer(void *unused) {
 __VERIFIER_atomic_begin(); payload=7; flag=1; __VERIFIER_atomic_end(); return 0;
}
void *reader(void *unused) {
 __VERIFIER_atomic_begin(); __VERIFIER_assume(flag==1); __VERIFIER_atomic_end();
 (void)payload; return 0;
}
int main(void) { pthread_t a,b; pthread_create(&a,0,writer,0);pthread_create(&b,0,reader,0);return 0;}
