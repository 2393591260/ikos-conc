#include <pthread.h>
#include <stdlib.h>
int data;

int **p;
int x;
void *worker(void *a){
**p=1;
return 0;

}
int main(void){
pthread_t t;
p=malloc(sizeof(int*));
*p=&x;
pthread_create(&t,0,worker,0);
x=2;
pthread_join(t,0);
return 0;

}
