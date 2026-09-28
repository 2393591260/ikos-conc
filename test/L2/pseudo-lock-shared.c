/* 测试目标: __VERIFIER_atomic_begin/end 单一共享伪锁（0xA7E0AD1C）—— 同一共享变量在两段内自增，
 *           伪锁提供互斥
 * 预期结果: Safe
 */
#include <pthread.h>
extern void __VERIFIER_atomic_begin(void);
extern void __VERIFIER_atomic_end(void);
int x = 0;
void *worker(void *arg) { __VERIFIER_atomic_begin(); x++; __VERIFIER_atomic_end(); return 0; }
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  __VERIFIER_atomic_begin(); x++; __VERIFIER_atomic_end();
  pthread_join(t, 0);
  return 0;
}
