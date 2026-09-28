/* 测试目标: 无锁保护的 cond 场景 —— data 的写读都不持锁，条件变量不保护数据
 * 预期结果: Race
 */
#include <pthread.h>
int data = 0;
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t c = PTHREAD_COND_INITIALIZER;
void *worker(void *arg) { data = 1; return 0; }   /* 无锁写 data */
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  pthread_mutex_lock(&m);
  pthread_cond_wait(&c, &m);   /* 无 signal，stub 仅 mem_forget_all */
  pthread_mutex_unlock(&m);
  int r = data;                 /* 无锁读 data */
  (void)r;
  pthread_join(t, 0);
  return 0;
}
