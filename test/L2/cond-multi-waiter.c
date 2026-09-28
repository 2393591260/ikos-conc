/* 测试目标: 多等待者 + 单 signal —— 一个 signal 只唤醒一个，且 data 无锁访问，保守报 Race
 * 预期结果: Race
 */
#include <pthread.h>
int data = 0;
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t c = PTHREAD_COND_INITIALIZER;
void *worker(void *arg) {
  pthread_mutex_lock(&m); pthread_cond_wait(&c, &m); pthread_mutex_unlock(&m);
  data++;                       /* 无锁写 data（在 m 之外） */
  return 0;
}
int main(void) {
  pthread_t w1, w2;
  pthread_create(&w1, 0, worker, 0);
  pthread_create(&w2, 0, worker, 0);
  pthread_mutex_lock(&m); pthread_cond_signal(&c); pthread_mutex_unlock(&m);  /* 只唤醒一个 */
  pthread_join(w1, 0); pthread_join(w2, 0);
  return 0;
}
