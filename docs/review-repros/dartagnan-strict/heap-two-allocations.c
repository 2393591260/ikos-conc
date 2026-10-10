#include <pthread.h>
#include <stdlib.h>
int *pointer, *other;
void *worker(void *argument) {pointer[0]=1;return 0;}
int main(void) {
 pthread_t thread;
 pointer=malloc(sizeof(int));
 other=malloc(sizeof(int));
 other[0]=3;
 pthread_create(&thread,0,worker,0);
 pointer[0]=2;
 pthread_join(thread,0);return 0;
}
