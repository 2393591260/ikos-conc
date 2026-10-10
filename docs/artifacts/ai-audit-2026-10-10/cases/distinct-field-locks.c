#include <pthread.h>
struct object { pthread_mutex_t left, right; int value; };
static struct object state = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, 0 };
static void *writer_left(void *arg) { pthread_mutex_lock(&state.left); state.value=1; pthread_mutex_unlock(&state.left); return 0; }
static void *writer_right(void *arg) { pthread_mutex_lock(&state.right); state.value=2; pthread_mutex_unlock(&state.right); return 0; }
int main(void) { pthread_t a,b; pthread_create(&a,0,writer_left,0); pthread_create(&b,0,writer_right,0); pthread_join(a,0); pthread_join(b,0); return 0; }
