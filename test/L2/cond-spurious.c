/* 测试目标: 无 signal 伪唤醒 —— wait 无 signal 也会返回（可达性 stub），data 无锁写读为真竞态
 * 预期结果: Race
 */
#include <pthread.h>
int data = 0;
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t c = PTHREAD_COND_INITIALIZER;
void *worker(void *arg) {
  pthread_mutex_lock(&m); pthread_cond_wait(&c, &m); pthread_mutex_unlock(&m);
  data++;                       /* 无 signal 伪唤醒后无锁写 data */
  return 0;
}
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  data++;                       /* 无锁写 data（并发） */
  pthread_join(t, 0);
  return 0;
}
