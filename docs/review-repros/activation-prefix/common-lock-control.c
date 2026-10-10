#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
int data;
pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
void *worker(void *unused) { pthread_mutex_lock(&mutex); data++; pthread_mutex_unlock(&mutex); return 0; }
int main(void) {
 int count=__VERIFIER_nondet_int();
 if(count<5 || count>10) return 0;
 pthread_t handles[10];
 for(int i=0;i<count;i++) pthread_create(&handles[i],0,worker,0);
 for(int i=0;i<count;i++) pthread_join(handles[i],0);
 return 0;
}
