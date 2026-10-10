#include <pthread.h>
int data;
void __VERIFIER_atomic_write(void) {data=1;}
void *a(void *arg) {__VERIFIER_atomic_write();pthread_exit(0);}
void *b(void *arg) {data=2;return 0;}
int main(void) {
 pthread_t x,y;
 pthread_create(&x,0,a,0);
 pthread_create(&y,0,b,0);
 return 0;
}
