/* 测试目标: pthread_join —— main 在 join【前】读共享变量，与子线程写并发
 * 预期结果: Race
 */
#include <pthread.h>
int x = 0;
void *worker(void *arg) { x = 1; return 0; }
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  int r = x; (void)r;          /* 未 join 就读 → 与 worker 写并发 */
  pthread_join(t, 0);
  return 0;
}
