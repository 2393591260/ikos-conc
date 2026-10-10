#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#ifndef CASE
#define CASE 0
#endif
#ifndef CONTROL
#define CONTROL 0
#endif
#ifdef RENAMED
#define flag another_flag
#define payload another_payload
#define reader another_reader
#define writer another_writer
#endif
#if CASE == 3
static unsigned char flag;
#else
static int flag;
#endif
extern int raw_sscanf(const char *, const char *, ...) __asm__("sscanf");
static int payload;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static void *reader(void *arg) {
  (void)arg;
  pthread_mutex_lock(&mutex);
  flag=0;
  pthread_mutex_unlock(&mutex);
  pthread_mutex_lock(&mutex);
  int x=flag;
#if CONTROL == 1
  if(x==1) payload=1;
#endif
  pthread_mutex_unlock(&mutex);
#if CONTROL != 1
  if(x==1) payload=1;
#endif
  return 0;
}
static void *writer(void *arg) {
  (void)arg;
#if CASE == 3
  unsigned char private_flag=0;
#else
  int private_flag=0;
#endif
#if CONTROL == 2
  void *target=&private_flag;
#else
  void *target=&flag;
#endif
  pthread_mutex_lock(&mutex);
#if CASE == 0
  read(0,target,sizeof flag);
#elif CASE == 1
  raw_sscanf("1","%d",(int *)target);
#elif CASE == 2
  printf("x%n",(int *)target);
#elif CASE == 3
  strncpy((char *)target,"\1",1);
#elif CASE == 4
  read(0,0,0);
#elif CASE == 5
  *(int *)target=1; /* covered typed write control */
#endif
#if CONTROL == 1
  payload=2;
#endif
  pthread_mutex_unlock(&mutex);
#if CONTROL != 1
  payload=2;
#endif
  return 0;
}
int main(void) {
  pthread_t a,b;
  pthread_create(&a,0,reader,0);
  pthread_create(&b,0,writer,0);
  pthread_join(a,0);
  pthread_join(b,0);
  return 0;
}
