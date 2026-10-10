#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
int data; int initialized[6];
void *worker(void *unused) { data++; return 0; }
int main(void) {
 int count=__VERIFIER_nondet_int();
 if(count<5 || count>10) return 0;
 for(int i=0;i<6;i++) initialized[i]=i;
 pthread_t handles[10];
 for(int i=0;i<count;i++) pthread_create(&handles[i],0,worker,0);
 for(int i=0;i<count;i++) pthread_join(handles[i],0);
 return 0;
}
