/* 测试目标: pthread_spin_lock/unlock —— 两线程用【同一把】自旋锁保护共享变量
 * 预期结果: Safe
 */
#include <pthread.h>
int x = 0;
pthread_spinlock_t s;
void *worker(void *arg) { pthread_spin_lock(&s); x++; pthread_spin_unlock(&s); return 0; }
int main(void) {
  pthread_spin_init(&s, PTHREAD_PROCESS_PRIVATE);
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  pthread_spin_lock(&s); x++; pthread_spin_unlock(&s);
  pthread_join(t, 0);
  return 0;
}
