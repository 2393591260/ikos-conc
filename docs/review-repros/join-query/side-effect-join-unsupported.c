#include <pthread.h>
int data, auxiliary;
void *worker(void *unused) {data=1;return 0;}
void *idle(void *unused) {auxiliary=1;return 0;}
int main(void) {pthread_t t,other;pthread_create(&t,0,worker,0);pthread_create(&other,0,idle,0);pthread_join(other,0);data=2;return 0;}
