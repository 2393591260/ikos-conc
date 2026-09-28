/* 测试目标: 仅靠 signal 排序、无锁保护 —— data 的写读只被 signal→wait 排序。
 *           signal→wait 是 MAY 边（伪唤醒），sound 建模不用于短路，故报 Race（文档化 FP，程序本身无 race）
 * 预期结果: Race（文档化误报 FP）
 */
#include <pthread.h>
int data = 0;
int ready = 0;
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t c = PTHREAD_COND_INITIALIZER;
void *producer(void *arg) {
  data = 42;                   /* 写 data（无锁），仅靠后续 signal 排序 */
  pthread_mutex_lock(&m); ready = 1; pthread_cond_signal(&c); pthread_mutex_unlock(&m);
  return 0;
}
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, producer, 0);
  pthread_mutex_lock(&m);
  while (!ready) pthread_cond_wait(&c, &m);
  pthread_mutex_unlock(&m);
  int r = data;                 /* 读 data（仅被 signal 排序，无锁） */
  (void)r;
  pthread_join(t, 0);
  return 0;
}
