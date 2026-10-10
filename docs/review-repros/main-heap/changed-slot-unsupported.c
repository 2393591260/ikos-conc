#include <pthread.h>
#include <stdlib.h>
int *pointer;
void *worker(void *argument) {pointer[0]=1;return 0;}
int main(void) {
 pthread_t a,b;
 pointer=malloc(sizeof(int));
 pthread_create(&a,0,worker,0);
 pointer=malloc(sizeof(int));pointer[0]=2;
 pthread_join(a,0);return 0;
}
