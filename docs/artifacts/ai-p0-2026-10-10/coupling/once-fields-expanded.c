#include <pthread.h>
static struct { pthread_once_t a,b; } controls = {PTHREAD_ONCE_INIT,PTHREAD_ONCE_INIT};
static int data;
static void callback(void) { data = 1; }
static void *left(void *p) { (void)p; callback(); return 0; }
static void *right(void *p) { (void)p; callback(); return 0; }
int main(void) { pthread_t a,b; pthread_create(&a,0,left,0); pthread_create(&b,0,right,0); pthread_join(a,0); pthread_join(b,0); return 0; }
