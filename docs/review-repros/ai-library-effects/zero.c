#include <pthread.h>
#include <unistd.h>
static int data;
static void *worker(void *p) {(void)p;data=1;return 0;}
int main(void) {pthread_t t;pthread_create(&t,0,worker,0);read(0,0,0);data=2;pthread_join(t,0);return 0;}
