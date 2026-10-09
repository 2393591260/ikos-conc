#include <pthread.h>
#include <time.h>
clockid_t output;
pthread_condattr_t attr;
void *getter(void *unused) {pthread_condattr_getclock(&attr, &output);return 0;}
void *writer(void *unused) {output=CLOCK_REALTIME;return 0;}
int main(void) {pthread_t a,b;pthread_condattr_init(&attr);pthread_create(&a,0,getter,0);pthread_create(&b,0,writer,0);return 0;}
