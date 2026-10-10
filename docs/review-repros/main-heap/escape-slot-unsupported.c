#include <pthread.h>
#include <stdlib.h>
int *pointer;int **published;
void *worker(void *argument) {pointer[0]=1;return 0;}
int main(void) {
 pthread_t a,b;
 published=&pointer;pointer=malloc(sizeof(int));
 pthread_create(&a,0,worker,0);
 pointer[0]=2;
 pthread_join(a,0);return 0;
}
