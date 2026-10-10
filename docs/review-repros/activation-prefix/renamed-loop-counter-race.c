#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
int payload;
void *job(void *unused) { payload++; return 0; }
int main(void) {
 int jobs=__VERIFIER_nondet_int();
 if(jobs<5 || jobs>10) return 0;
 pthread_t ids[10];
 for(int i=0;i<jobs;i++) pthread_create(&ids[i],0,job,0);
 for(int i=0;i<jobs;i++) pthread_join(ids[i],0);
 return 0;
}
