/* 测试目标: pthread_mutex_lock/unlock —— 两线程用【同一把】互斥锁保护共享变量
 * 预期结果: Safe
 */
#include <pthread.h>
int x = 0;
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
void *worker(void *arg) { pthread_mutex_lock(&m); x++; pthread_mutex_unlock(&m); return 0; }
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  pthread_mutex_lock(&m); x++; pthread_mutex_unlock(&m);
  pthread_join(t, 0);
  return 0;
}
