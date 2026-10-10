#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
static int flag,value;
static pthread_mutex_t m=PTHREAD_MUTEX_INITIALIZER;
static void *a_set(void *arg) { pthread_mutex_lock(&m); if(__VERIFIER_nondet_int()) flag=1; else flag=0; pthread_mutex_unlock(&m); value=2; return 0; }
static void *b_reader(void *arg) { pthread_mutex_lock(&m); if(flag==1) value=1; pthread_mutex_unlock(&m); return 0; }
int main(void) { pthread_t a,b; pthread_create(&a,0,a_set,0); pthread_create(&b,0,b_reader,0); pthread_join(a,0); pthread_join(b,0); return 0; }
