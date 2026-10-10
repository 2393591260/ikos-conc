#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
unsigned short selector;
int data;
void *a(void *p){ if(selector==0) data=1; return 0; }
void *b(void *p){ if(selector==1) data=2; return 0; }
int main(void){
 *(unsigned char *)&selector=(unsigned char)__VERIFIER_nondet_int();
 pthread_t x,y;pthread_create(&x,0,a,0);pthread_create(&y,0,b,0);return 0;
}
