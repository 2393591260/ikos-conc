/* 测试目标: pthread_rwlock_wrlock —— 两线程都用写锁（互斥）保护共享变量
 * 预期结果: Safe
 */
#include <pthread.h>
int x = 0;
pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
void *worker(void *arg) { pthread_rwlock_wrlock(&rw); x++; pthread_rwlock_unlock(&rw); return 0; }
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  pthread_rwlock_wrlock(&rw); x++; pthread_rwlock_unlock(&rw);   /* 写锁互斥 */
  pthread_join(t, 0);
  return 0;
}
