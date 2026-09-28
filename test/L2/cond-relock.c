/* 测试目标: wait 返回后锁重获 —— 消费者 wait 返回后（重获 m）在 m 下写 data，与生产者的 m 下写不冲突
 * 预期结果: Safe
 */
#include <pthread.h>
int data = 0;
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t c = PTHREAD_COND_INITIALIZER;
void *producer(void *arg) {
  pthread_mutex_lock(&m); data = 42; pthread_cond_signal(&c); pthread_mutex_unlock(&m);
  return 0;
}
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, producer, 0);
  pthread_mutex_lock(&m);
  pthread_cond_wait(&c, &m);   /* 返回时重获 m */
  data++;                       /* 持 m 写 data */
  pthread_mutex_unlock(&m);
  pthread_join(t, 0);
  return 0;
}
