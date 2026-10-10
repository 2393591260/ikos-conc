#include <pthread.h>
int data;
pthread_key_t key;
void cleanup(void *p) {data=3;}
void __VERIFIER_atomic_write(void) {data=1;}
void *a(void *arg) {pthread_key_create(&key,cleanup);pthread_setspecific(key,&data);__VERIFIER_atomic_write();pthread_exit(0);}
void *b(void *arg) {data=2;return 0;}
int main(void) {
 pthread_t x,y;
 pthread_create(&x,0,a,0);
 pthread_create(&y,0,b,0);
 return 0;
}
