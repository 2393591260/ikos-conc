#include <pthread.h>
static pthread_t handle; static int data;
static void *work(void *p) { data=1; return 0; }
int main(void) {pthread_create(&handle,0,work,0); pthread_join(handle,0); data=2; return 0;}
