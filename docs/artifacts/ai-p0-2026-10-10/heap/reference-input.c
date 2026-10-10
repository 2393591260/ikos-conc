#include <pthread.h>
#include <stdlib.h>
static int x,y,z;
static struct Cell { int *p[2]; } *cell;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
extern int __VERIFIER_nondet_int(void);
static void *publish(void *arg) { (void)arg; pthread_mutex_lock(&mutex); cell->p[__VERIFIER_nondet_int() ? 0 : 1]=&z; pthread_mutex_unlock(&mutex); return 0; }
static void *write(void *arg) { (void)arg; pthread_mutex_lock(&mutex); cell->p[0]=&x; pthread_mutex_unlock(&mutex); pthread_mutex_lock(&mutex); int *p=cell->p[0]; pthread_mutex_unlock(&mutex); *p=1; return 0; }
int main(void) { pthread_t a,b; cell=malloc(sizeof *cell); if(!cell) return 0; cell->p[0]=&x; cell->p[1]=&y; pthread_create(&a,0,publish,0); pthread_create(&b,0,write,0); z=2; pthread_join(a,0); pthread_join(b,0); free(cell); return 0; }
