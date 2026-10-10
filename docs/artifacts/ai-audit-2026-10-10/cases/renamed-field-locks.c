#include <pthread.h>
struct object { pthread_mutex_t alpha, beta; int payload; };
static struct object shared_box = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, 0 };
static void *first_task(void *arg) { pthread_mutex_lock(&shared_box.alpha); shared_box.payload=1; pthread_mutex_unlock(&shared_box.alpha); return 0; }
static void *second_task(void *arg) { pthread_mutex_lock(&shared_box.beta); shared_box.payload=2; pthread_mutex_unlock(&shared_box.beta); return 0; }
int main(void) { pthread_t a,b; pthread_create(&a,0,first_task,0); pthread_create(&b,0,second_task,0); pthread_join(a,0); pthread_join(b,0); return 0; }
