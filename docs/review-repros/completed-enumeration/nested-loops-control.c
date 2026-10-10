#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
extern void __VERIFIER_assume(int);
pthread_t ids[3];
int outer, inner, data;
void *worker(void *argument) {if(outer) data=1;return 0;}
int main(void) {
 int count=__VERIFIER_nondet_int();
 __VERIFIER_assume(count>=1 && count<=3);
 for (int i=0;i<count;++i) pthread_create(&ids[i],0,worker,0);
 while (outer) {while (inner) {}}
 data=2;
 return 0;
}
