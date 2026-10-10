#include <pthread.h>
int data;
void *worker(void *unused) {data=1;return 0;}
void *idle(void *unused) {return 0;}
int main(void) {pthread_t t,other,copy;pthread_create(&t,0,worker,0);pthread_create(&other,0,idle,0);copy=other;pthread_join(copy,0);data=2;return 0;}
