#include <pthread.h>
int data;
void *worker(void *unused) {data=1;return 0;}
int main(void) {pthread_t t;pthread_create(&t,0,worker,0);pthread_join(t,0);data=2;return 0;}
