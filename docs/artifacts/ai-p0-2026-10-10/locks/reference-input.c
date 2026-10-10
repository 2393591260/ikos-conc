#include <pthread.h>
#include <stdlib.h>
static char *base;
static int data;
#define DISTANCE 4294967296ULL
static void *first(void *p) { (void)p; pthread_mutex_lock((pthread_mutex_t*)base); data=1; pthread_mutex_unlock((pthread_mutex_t*)base); return 0; }
static void *second(void *p) { (void)p; pthread_mutex_lock((pthread_mutex_t*)(base+DISTANCE)); data=2; pthread_mutex_unlock((pthread_mutex_t*)(base+DISTANCE)); return 0; }
int main(void) { pthread_t a,b; base=malloc(DISTANCE+sizeof(pthread_mutex_t)); if(!base) return 0; pthread_mutex_init((pthread_mutex_t*)base,0); pthread_mutex_init((pthread_mutex_t*)(base+DISTANCE),0); pthread_create(&a,0,first,0); pthread_create(&b,0,second,0); pthread_join(a,0); pthread_join(b,0); free(base); return 0; }
