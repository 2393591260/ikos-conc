#include <pthread.h>
#include <stdatomic.h>
static atomic_int flag;
static int value;
static void *a_set(void *arg) { atomic_store(&flag,1); return 0; }
static void *b_reader(void *arg) { if(atomic_load(&flag)==1) value=1; return 0; }
static void *z_reset(void *arg) { atomic_store(&flag,0); value=2; return 0; }
int main(void) { pthread_t a,b,c; pthread_create(&a,0,a_set,0); pthread_create(&b,0,b_reader,0); pthread_create(&c,0,z_reset,0); pthread_join(a,0); pthread_join(b,0); pthread_join(c,0); return 0; }
