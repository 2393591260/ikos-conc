#include <pthread.h>
#include <stdlib.h>
int data;
char *p;

void *worker(void *a){
if(*(int *)(p+1)==4)data=1;
return 0;

}

int main(void){
pthread_t t;
p=malloc(2*sizeof(int));
*(int *)(p+1)=4;
pthread_create(&t,0,worker,0);
data=2;
pthread_join(t,0);
return 0;

}
