/* 测试目标: pthread_rwlock_rdlock —— 读锁是共享的（不互斥），两线程都在 rdlock 下【写】共享变量
 * 预期结果: Race
 */
#include <pthread.h>
int x = 0;
pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
void *worker(void *arg) { pthread_rwlock_rdlock(&rw); x++; pthread_rwlock_unlock(&rw); return 0; }
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  pthread_rwlock_rdlock(&rw); x++; pthread_rwlock_unlock(&rw);   /* rd-rd 共享，都写 → race */
  pthread_join(t, 0);
  return 0;
}
