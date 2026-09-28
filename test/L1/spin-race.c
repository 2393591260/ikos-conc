/* 测试目标: pthread_spin_lock/unlock —— 两线程用【不同】自旋锁保护同一共享变量
 * 预期结果: Race
 */
#include <pthread.h>
int x = 0;
pthread_spinlock_t s1, s2;
void *worker(void *arg) { pthread_spin_lock(&s1); x++; pthread_spin_unlock(&s1); return 0; }
int main(void) {
  pthread_spin_init(&s1, PTHREAD_PROCESS_PRIVATE);
  pthread_spin_init(&s2, PTHREAD_PROCESS_PRIVATE);
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  pthread_spin_lock(&s2); x++; pthread_spin_unlock(&s2);   /* s2 != s1 */
  pthread_join(t, 0);
  return 0;
}
