#include <pthread.h>
#include <stdlib.h>
int data;
int *different;

int read_helper(int *p){
return *p;

}

void *worker(void *a){
if(read_helper(different)==42)data=1;
return 0;

}

int main(void){
pthread_t t;
different=malloc(sizeof(int));
*different=42;
pthread_create(&t,0,worker,0);
data=2;
pthread_join(t,0);
return 0;

}
