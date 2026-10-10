#include <pthread.h>
#include <stdlib.h>
void *worker(void *argument) {
 int *private_object=malloc(sizeof(int));
 private_object[0]=1;
 return 0;
}
int main(void) {
 pthread_t a,b;
 pthread_create(&a,0,worker,0);pthread_create(&b,0,worker,0);
 pthread_join(a,0);pthread_join(b,0);return 0;
}
