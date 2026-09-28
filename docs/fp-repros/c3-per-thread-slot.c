#include <pthread.h>
#include <stdlib.h>
int *datas;
void *t(void *arg){ int i=(int)(long)arg; datas[i]=1; return 0; }
int main(){ pthread_t tid[2];
  datas=(int*)malloc(2*sizeof(int));
  pthread_create(&tid[0],0,t,(void*)0);
  pthread_create(&tid[1],0,t,(void*)1);
  pthread_join(tid[0],0); pthread_join(tid[1],0); return 0; }
