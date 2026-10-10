#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
static int x,y;
static int *left,*right;
static void *worker(void *unused) {
 int **pick=__VERIFIER_nondet_int()?&left:&right;
 int *target=*pick;
 *target=2;
 return 0;
}
int main(void) {
 left=&x;right=&y;
 pthread_t t;pthread_create(&t,0,worker,0);
#ifdef WRITE_Y
 y=1;
#else
 x=1;
#endif
 pthread_join(t,0);return 0;
}
