#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
struct record { int pad; pthread_mutex_t gate; } records[3];
int data;
void *reader(void *unused) {
  int i=__VERIFIER_nondet_int();
  if(i<0 || i>=3) return 0;
  pthread_mutex_lock(&records[i].gate);
  int state=records[i].gate.__data.__owner;
  pthread_mutex_unlock(&records[i].gate);
  if(state==0) data=1;
  return 0;
}
void *writer(void *unused) {data=2; return 0;}
int main(void){for(int j=0;j<3;j++)pthread_mutex_init(&records[j].gate,0);pthread_t a,b;pthread_create(&a,0,reader,0);pthread_create(&b,0,writer,0);return 0;}
