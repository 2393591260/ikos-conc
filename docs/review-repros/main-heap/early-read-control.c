#include <pthread.h>
#include <stdlib.h>
int *pointer;
void *worker(void *argument) {pointer[0]=1;return 0;}
int main(void) {
 pthread_t a,b;
 int *early=pointer;pointer=malloc(sizeof(int));
 pthread_create(&a,0,worker,0);
 if(early) early[0]=2;
 pthread_join(a,0);return 0;
}
