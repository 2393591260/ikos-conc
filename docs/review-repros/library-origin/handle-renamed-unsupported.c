#include <pthread.h>
pthread_t identity;
int payload;
void *reader(void *unused) { if(identity==0) payload=1; return 0; }
void *writer(void *unused) {payload=2;return 0;}
int main(void){pthread_t b;pthread_create(&identity,0,reader,0);pthread_create(&b,0,writer,0);return 0;}
