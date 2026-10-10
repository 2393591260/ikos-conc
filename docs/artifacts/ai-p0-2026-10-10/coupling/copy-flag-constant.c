#include <pthread.h>
#include <string.h>
static int flag, payload;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
extern unsigned __VERIFIER_nondet_uint(void);
__attribute__((noinline)) static void copy_flag(int *dst, int *src, unsigned len) { memcpy(dst,src,len); }
static void *reader(void *p) {
  (void)p;
  pthread_mutex_lock(&mutex); flag=0; pthread_mutex_unlock(&mutex);
  pthread_mutex_lock(&mutex); int go=flag; pthread_mutex_unlock(&mutex);
  if (go==1) payload=1;
  return 0;
}
static void *writer(void *p) {
  (void)p; int one=1;
  unsigned n=__VERIFIER_nondet_uint(); if(n!=sizeof(flag)) return 0;
  pthread_mutex_lock(&mutex); memcpy(&flag,&one,sizeof(flag)); pthread_mutex_unlock(&mutex);
  payload=2; return 0;
}
int main(void) { pthread_t a,b; pthread_create(&a,0,reader,0); pthread_create(&b,0,writer,0); pthread_join(a,0); pthread_join(b,0); return 0; }
