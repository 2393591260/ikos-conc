#include <pthread.h>
#include <stdlib.h>
int data;

int *p;
pthread_mutex_t m=PTHREAD_MUTEX_INITIALIZER;
void *worker(void *a){
pthread_mutex_lock(&m);
*p=4;
pthread_mutex_unlock(&m);
*p=8;
return 0;

}
int main(void){
pthread_t t;
p=malloc(sizeof(int));
pthread_create(&t,0,worker,0);
pthread_mutex_lock(&m);
int v=*p;
pthread_mutex_unlock(&m);
if(v==4)*p=6;
pthread_join(t,0);
return 0;

}
