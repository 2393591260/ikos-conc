#include <pthread.h>
int data;
void *worker(void *unused) {data=1;return 0;}
void *idle(void *unused) {return 0;}
int main(void) {pthread_t t,other;pthread_create(&t,0,worker,0);pthread_create(&other,0,idle,0);pthread_join(other,0);data=2;pthread_join(t,0);return 0;}
