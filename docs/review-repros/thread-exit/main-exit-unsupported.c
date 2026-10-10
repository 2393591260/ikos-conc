#include <pthread.h>
extern void __VERIFIER_atomic_begin(void);
extern void __VERIFIER_atomic_end(void);
int data;
void *worker(void *arg) {data=2;return 0;}
int main(void) {
 pthread_t x;
 pthread_create(&x,0,worker,0);
 __VERIFIER_atomic_begin();
 data=1;
 __VERIFIER_atomic_end();
 pthread_exit(0);
}
