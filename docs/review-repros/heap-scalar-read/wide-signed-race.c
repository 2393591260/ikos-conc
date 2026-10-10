#include <pthread.h>
#include <stdlib.h>
int data;

long long *p;
void *worker(void *a){
if(*p==-1234567890123LL)data=1;
return 0;

}
int main(void){
pthread_t t;
p=malloc(sizeof(long long));
*p=-1234567890123LL;
pthread_create(&t,0,worker,0);
data=2;
pthread_join(t,0);
return 0;

}
