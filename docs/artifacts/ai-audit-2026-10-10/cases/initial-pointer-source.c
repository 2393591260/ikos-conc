#include <pthread.h>
static int x,y;
static int *p=&x;
static pthread_mutex_t m=PTHREAD_MUTEX_INITIALIZER;
static void *reader(void *arg) { pthread_mutex_lock(&m); int *q=p; pthread_mutex_unlock(&m); *q=1; return 0; }
static void *writer(void *arg) { pthread_mutex_lock(&m); p=&y; pthread_mutex_unlock(&m); x=2; return 0; }
int main(void) { pthread_t a,b; pthread_create(&a,0,reader,0); pthread_create(&b,0,writer,0); pthread_join(a,0); pthread_join(b,0); return 0; }
