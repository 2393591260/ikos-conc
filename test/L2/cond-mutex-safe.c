/* 测试目标: pthread_cond_wait 标准纪律 —— 生产者在 m 下写 data 并 signal，消费者 wait 后（重获 m）在 m 下读 data
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
  pthread_cond_wait(&c, &m);   /* wait 返回时重获 m */
  int r = data;                 /* 读 data 时仍持 m */
  pthread_mutex_unlock(&m);
  (void)r;
  pthread_join(t, 0);
  return 0;
}
