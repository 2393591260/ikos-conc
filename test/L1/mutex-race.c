/* 测试目标: pthread_mutex_lock/unlock —— 两线程用【不同】互斥锁保护同一共享变量
 * 预期结果: Race
 */
#include <pthread.h>
int x = 0;
pthread_mutex_t m1 = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t m2 = PTHREAD_MUTEX_INITIALIZER;
void *worker(void *arg) { pthread_mutex_lock(&m1); x++; pthread_mutex_unlock(&m1); return 0; }
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  pthread_mutex_lock(&m2); x++; pthread_mutex_unlock(&m2);   /* m2 != m1 → 无公共锁 */
  pthread_join(t, 0);
  return 0;
}
