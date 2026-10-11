#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifndef CASE
#define CASE 0
#endif
static uintptr_t address;
#if CASE == 11
int *encoded_global = (int *)(uintptr_t)1;
#endif
static int *direct;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int tls;
static void *worker(void *arg) {
 (void)arg;
#if CASE != 1 && CASE != 2 && CASE != 8 && CASE != 10 && CASE != 11
#if CASE == 4
 pthread_mutex_lock(&mutex);
#endif
#if CASE == 7
 *direct=1;
#else
 *(int *)address=1;
#endif
#if CASE == 4
 pthread_mutex_unlock(&mutex);
#endif
#endif
 return 0;
}
int main(void) {
 pthread_t thread;
 int local=0;
#if CASE == 5
 int *pointer=&tls;
#elif CASE == 6
 int *pointer=malloc(sizeof *pointer);
 if (!pointer) return 0;
#else
 int *pointer=&local;
#endif
 uintptr_t token=(uintptr_t)pointer;
#if CASE == 9
 memcpy(&address,&token,sizeof address);
#elif CASE == 7
 memcpy(&direct,&token,sizeof direct);
#else
 address=token;
#endif
 pthread_create(&thread,0,worker,0);
#if CASE == 3
 pthread_join(thread,0);
#endif
#if CASE == 4
 pthread_mutex_lock(&mutex);
#endif
#if CASE == 2
 *(int *)token=2;
#elif CASE == 8
 if (token==(uintptr_t)pointer) *pointer=2;
#elif CASE == 10
 int *zero=(int *)(uintptr_t)0;
 if (zero) *zero=2;
#else
 *pointer=2;
#endif
#if CASE == 4
 pthread_mutex_unlock(&mutex);
#endif
#if CASE != 3
 pthread_join(thread,0);
#endif
#if CASE == 6
 free(pointer);
#endif
 return local;
}
