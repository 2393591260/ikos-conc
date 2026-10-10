#include <pthread.h>
struct {pthread_t handle; unsigned long flag;} state={0,1};
int data;
void *reader(void *unused) { if(state.flag==1) data=1; return 0; }
void *writer(void *unused) {data=2;return 0;}
int main(void){pthread_t b;pthread_create(&state.handle,0,reader,0);pthread_create(&b,0,writer,0);return 0;}
