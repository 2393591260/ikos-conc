/* 测试目标: Unknown 边界反例 —— 纯指针别名模糊（多态锁）致锁被丢弃，数据访问 points-to 具体，
 *           应报 Race 而非 Unknown
 * 预期结果: Race
 */
#include <pthread.h>
int x = 0;
pthread_mutex_t m1 = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t m2 = PTHREAD_MUTEX_INITIALIZER;
extern int __VERIFIER_nondet_int(void);
void *worker(void *arg) {
  pthread_mutex_t *mp = __VERIFIER_nondet_int() ? &m1 : &m2;   /* 多态锁 {m1,m2} */
  pthread_mutex_lock(mp); x++; pthread_mutex_unlock(mp);
  return 0;
}
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  pthread_mutex_t *mp = __VERIFIER_nondet_int() ? &m1 : &m2;
  pthread_mutex_lock(mp); x++; pthread_mutex_unlock(mp);
  pthread_join(t, 0);
  return 0;
}
