#include <pthread.h>
int output;
pthread_mutexattr_t attr;
void *getter(void *unused) {pthread_mutexattr_gettype(&attr, &output);return 0;}
void *writer(void *unused) {output=1;return 0;}
int main(void) {pthread_t a,b;pthread_mutexattr_init(&attr);pthread_create(&a,0,getter,0);pthread_create(&b,0,writer,0);return 0;}
