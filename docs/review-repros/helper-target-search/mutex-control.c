#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
int data;
pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
void inner(void) {data=1;}
void *worker(void *arg) {pthread_mutex_lock(&lock);inner();pthread_mutex_unlock(&lock);return 0;}
int main(void) {
 pthread_t a,b;
 pthread_create(&a,0,worker,0);pthread_create(&b,0,worker,0);
 pthread_join(a,0);pthread_join(b,0);return 0;
}
