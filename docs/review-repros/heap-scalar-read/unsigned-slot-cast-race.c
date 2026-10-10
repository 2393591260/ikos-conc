#include <pthread.h>
#include <stdlib.h>
int data;

unsigned *different;
unsigned read_helper(unsigned *p){
return *p;

}
void *worker(void *a){
if(read_helper(different)==4294967295U)data=1;
return 0;

}
int main(void){
pthread_t t;
different=malloc(sizeof(unsigned));
*different=4294967295U;
pthread_create(&t,0,worker,0);
data=2;
pthread_join(t,0);
return 0;

}
