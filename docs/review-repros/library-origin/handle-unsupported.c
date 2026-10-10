#include <pthread.h>
pthread_t handle;
int data;
void *reader(void *unused) { if(handle==0) data=1; return 0; }
void *writer(void *unused) {data=2;return 0;}
int main(void){pthread_t b;pthread_create(&handle,0,reader,0);pthread_create(&b,0,writer,0);return 0;}
