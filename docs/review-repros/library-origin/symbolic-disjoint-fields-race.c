#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
struct record { int payload; pthread_mutex_t guard; } records[3];
void *writer(void *unused) {
 int i=__VERIFIER_nondet_int();
 if(i<0 || i>=2) return 0;
 pthread_mutex_lock(&records[i].guard);
 records[i+1].payload++;
 pthread_mutex_unlock(&records[i].guard);
 return 0;
}
int main(void) {
 for(int i=0;i<3;i++) pthread_mutex_init(&records[i].guard,0);
 pthread_t t;pthread_create(&t,0,writer,0);
 int j=__VERIFIER_nondet_int();
 if(j<0 || j>=3) return 0;
 pthread_mutex_lock(&records[j].guard);
 records[j].payload++;
 pthread_mutex_unlock(&records[j].guard);
 return 0;
}
