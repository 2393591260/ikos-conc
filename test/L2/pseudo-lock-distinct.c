/* 测试目标: 伪锁作用域只在段内 —— 两线程用原子段保护【不同】变量 x/y，但对共享 data 的无锁访问
 *           仍应报 Race（伪锁不误保护无关数据）
 * 预期结果: Race
 */
#include <pthread.h>
extern void __VERIFIER_atomic_begin(void);
extern void __VERIFIER_atomic_end(void);
int x = 0, y = 0, data = 0;
void *t1(void *arg) { __VERIFIER_atomic_begin(); x++; __VERIFIER_atomic_end(); data++; return 0; }
void *t2(void *arg) { __VERIFIER_atomic_begin(); y++; __VERIFIER_atomic_end(); data++; return 0; }
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, t1, 0);
  pthread_create(&b, 0, t2, 0);
  pthread_join(a, 0); pthread_join(b, 0);
  return 0;
}
