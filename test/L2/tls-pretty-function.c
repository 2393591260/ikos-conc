/* 测试目标: 编译器常量字符串（__PRETTY_FUNCTION__）只读排除 —— 多线程都触达 reach_error，
 *           字符串常量本不可写，不应报竞态
 * 预期结果: Safe
 */
#include <pthread.h>
extern void __assert_fail(const char *, const char *, unsigned, const char *) __attribute__((__noreturn__));
void reach_error(void) {
  __assert_fail("0", "tls-pretty-function.c", 1, __PRETTY_FUNCTION__);
}
void __VERIFIER_assert(int c) { if (!c) { reach_error(); } }
void *thread(void *arg) {
  __VERIFIER_assert(0);   /* 恒假 → 触达 reach_error → 字符串常量被多线程同触 */
  return 0;
}
int main(void) {
  pthread_t t1, t2;
  pthread_create(&t1, 0, thread, 0);
  pthread_create(&t2, 0, thread, 0);
  pthread_join(t1, 0); pthread_join(t2, 0);
  return 0;
}
