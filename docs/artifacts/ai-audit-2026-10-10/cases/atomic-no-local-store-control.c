#include <pthread.h>
#include <stdatomic.h>
static atomic_int flag;
static int value;
static void *reader(void *arg) { if(atomic_load(&flag)==1) value=1; return 0; }
static void *writer(void *arg) { atomic_store(&flag,1); value=2; return 0; }
int main(void) { pthread_t a,b; pthread_create(&a,0,reader,0); pthread_create(&b,0,writer,0); pthread_join(a,0); pthread_join(b,0); return 0; }
