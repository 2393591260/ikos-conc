#include <pthread.h>
#include <stdlib.h>
int data;

int *p;
 void *worker(void *a){
if(*p==4) data=1;
return 0;

}
 int main(void){
pthread_t t;
p=malloc(sizeof(int));
*p=4;
pthread_create(&t,0,worker,0);
data=2;
pthread_join(t,0);
return 0;

}
