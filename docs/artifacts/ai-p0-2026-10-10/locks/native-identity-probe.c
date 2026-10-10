#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
int main(void) {
  size_t distance=4294967296ULL;
  char *base=malloc(distance+sizeof(pthread_mutex_t));
  if(!base) return 77;
  pthread_mutex_t *first=(pthread_mutex_t*)base;
  pthread_mutex_t *second=(pthread_mutex_t*)(base+distance);
  int a=pthread_mutex_init(first,0), b=pthread_mutex_init(second,0);
  if(a||b) return 78;
  if(pthread_mutex_lock(first)) return 79;
  int result=pthread_mutex_trylock(second);
  printf("distance=%zu second_trylock=%d\n",(size_t)((char*)second-(char*)first),result);
  if(result) return 80;
  pthread_mutex_unlock(second);pthread_mutex_unlock(first);
  pthread_mutex_destroy(second);pthread_mutex_destroy(first);free(base);return 0;
}
