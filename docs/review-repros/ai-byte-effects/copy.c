#include <pthread.h>
#include <string.h>
extern unsigned __VERIFIER_nondet_uint(void);
#ifdef RENAMED
#define flag alternate_guard
#define payload alternate_data
#define mutex gate
#define copy_flag transfer
#define reader consumer
#define writer producer
#endif
#if WIDE
typedef long long Scalar;
#else
typedef int Scalar;
#endif
static Scalar flag;
static int payload;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
__attribute__((noinline)) static void copy_flag(Scalar *dst, Scalar *src, unsigned len) {
#if CASE == 2
  memmove(dst,src,len);
#elif CASE == 3
  memset(dst,1,len);
#else
  memcpy(dst,src,len);
#endif
}
static void *reader(void *arg) {
  (void)arg;
  pthread_mutex_lock(&mutex); flag=0; pthread_mutex_unlock(&mutex);
  pthread_mutex_lock(&mutex);
#if CASE == 5
  Scalar go=0; memcpy(&go,&flag,sizeof(go));
#else
  Scalar go=flag;
#endif
#if CASE == 8
  if(go==1) payload=1;
  pthread_mutex_unlock(&mutex);
#else
  pthread_mutex_unlock(&mutex);
#if CASE == 3
  if(go!=0) payload=1;
#else
  if(go==1) payload=1;
#endif
#endif
  return 0;
}
static void *writer(void *arg) {
  (void)arg; Scalar one=1;
#if CASE == 1
  one=0;
#endif
  unsigned n=__VERIFIER_nondet_uint(); if(n!=sizeof(flag)) return 0;
  pthread_mutex_lock(&mutex);
#if CASE == 4
  unsigned char byte=1; memcpy(&flag,&byte,1);
#elif CASE == 5
  flag=1;
#elif CASE == 9
  memcpy(&flag,&one,0);
#else
  copy_flag(&flag,&one,n);
#endif
#if CASE == 8
  payload=2;
  pthread_mutex_unlock(&mutex);
#else
  pthread_mutex_unlock(&mutex);
  payload=2;
#endif
  return 0;
}
static int *pointer;
static void *pointer_writer(void *arg) { (void)arg; *pointer=2; return 0; }
static void *private_copy(void *arg) { (void)arg; int src=1,dst=0; memcpy(&dst,&src,sizeof dst); return 0; }
int main(void) {
  pthread_t a,b;
#if CASE == 6
  int local; int *source=&local; memcpy(&pointer,&source,sizeof pointer);
  pthread_create(&a,0,pointer_writer,0); local=1; pthread_join(a,0);
#elif CASE == 7
  pthread_create(&a,0,private_copy,0); pthread_join(a,0);
#else
  pthread_create(&a,0,reader,0); pthread_create(&b,0,writer,0);
  pthread_join(a,0); pthread_join(b,0);
#endif
  return 0;
}
