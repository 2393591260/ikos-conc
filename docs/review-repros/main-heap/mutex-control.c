#include <pthread.h>
#include <stdlib.h>
int *pointer;pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
void *worker(void *argument) {pthread_mutex_lock(&lock);pointer[0]=1;pthread_mutex_unlock(&lock);return 0;}
int main(void) {
 pthread_t a,b;
 pointer=malloc(sizeof(int));
 pthread_create(&a,0,worker,0);
 pthread_mutex_lock(&lock);pointer[0]=2;pthread_mutex_unlock(&lock);
 pthread_join(a,0);return 0;
}
