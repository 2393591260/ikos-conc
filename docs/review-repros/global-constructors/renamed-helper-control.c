#include <pthread.h>
int state=1, payload;
pthread_mutex_t m=PTHREAD_MUTEX_INITIALIZER;
static void set_state(void) { state=0; }
__attribute__((constructor(201))) static void boot(void) { set_state(); }
void *writer(void *x){ pthread_mutex_lock(&m); state=2; pthread_mutex_unlock(&m); payload=2; return 0; }
void *reader(void *x){ pthread_mutex_lock(&m); int active=state; pthread_mutex_unlock(&m); if(active==1) payload=1; return 0; }
int main(void){ pthread_t a,b; pthread_create(&a,0,writer,0); pthread_create(&b,0,reader,0); pthread_join(a,0); pthread_join(b,0); }
