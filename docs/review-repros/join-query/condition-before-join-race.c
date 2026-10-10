#include <pthread.h>
int data;
pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
void *worker(void *unused) {data=1;return 0;}
void *idle(void *unused) {return 0;}
int main(void) {pthread_t t,other;pthread_mutex_lock(&lock);pthread_cond_wait(&condition,&lock);pthread_mutex_unlock(&lock);pthread_create(&t,0,worker,0);pthread_create(&other,0,idle,0);pthread_join(other,0);data=2;return 0;}
