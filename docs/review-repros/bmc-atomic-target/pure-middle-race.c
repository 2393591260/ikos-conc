#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
extern void __VERIFIER_assume(int);
int data;
void __VERIFIER_atomic_write(int x) {
 int y=x+1;
 data=y;
 y++;
 __VERIFIER_assume(y<12);
}
void *a(void *arg) {
 int x=__VERIFIER_nondet_int();
 __VERIFIER_assume(x>=0 && x<10);
 __VERIFIER_atomic_write(x);
 return 0;
}
void *b(void *arg) {data=2;return 0;}
int main(void) {
 pthread_t x,y;
 pthread_create(&x,0,a,0);
 pthread_create(&y,0,b,0);
 return 0;
}
