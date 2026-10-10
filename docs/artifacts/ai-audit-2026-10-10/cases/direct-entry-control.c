#include <pthread.h>
static int x;
static void *worker(void *arg) { x=1; return 0; }
static void *(*volatile entry)(void *)=worker;
int main(void) { pthread_t t; pthread_create(&t,0,worker,0); x=2; pthread_join(t,0); return 0; }
