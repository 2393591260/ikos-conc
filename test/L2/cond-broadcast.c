/* 测试目标: broadcast 唤醒多线程 —— 多个 worker wait 后被 broadcast 唤醒，各自在 m 下写 data
 * 预期结果: Safe
 */
#include <pthread.h>
int data = 0;
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t c = PTHREAD_COND_INITIALIZER;
void *worker(void *arg) {
  pthread_mutex_lock(&m);
  pthread_cond_wait(&c, &m);   /* 等待 broadcast */
  data++;                       /* 持 m 写 data */
  pthread_mutex_unlock(&m);
  return 0;
}
int main(void) {
  pthread_t w1, w2;
  pthread_create(&w1, 0, worker, 0);
  pthread_create(&w2, 0, worker, 0);
  pthread_mutex_lock(&m);
  pthread_cond_broadcast(&c);   /* 唤醒全部等待者 */
  pthread_mutex_unlock(&m);
  pthread_join(w1, 0); pthread_join(w2, 0);
  return 0;
}
