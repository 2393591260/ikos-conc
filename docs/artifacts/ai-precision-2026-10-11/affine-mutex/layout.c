#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <assert.h>
struct item { int data; pthread_mutex_t mutex, other; };
int main(void) {
  struct item a[4];
  for (int i=0;i<4;i++) assert((char *)&a[i].mutex == (char *)&a[i]+offsetof(struct item,mutex));
  printf("pointer=%zu item=%zu mutex_offset=%zu\n",sizeof(void*)*8,sizeof(struct item),offsetof(struct item,mutex));
}
