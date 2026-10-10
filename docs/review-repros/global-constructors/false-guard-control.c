#include <pthread.h>
int flag=1, data;
pthread_mutex_t m=PTHREAD_MUTEX_INITIALIZER;
__attribute__((constructor)) static void initialize(void) { flag=0; }
void *writer(void *x){ pthread_mutex_lock(&m); flag=2; pthread_mutex_unlock(&m); data=2; return 0; }
void *reader(void *x){ pthread_mutex_lock(&m); int active=flag; pthread_mutex_unlock(&m); if(active==1) data=1; return 0; }
int main(void){ pthread_t a,b; pthread_create(&a,0,writer,0); pthread_create(&b,0,reader,0); pthread_join(a,0); pthread_join(b,0); }
