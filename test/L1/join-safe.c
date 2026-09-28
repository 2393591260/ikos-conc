/* 测试目标: pthread_join 的 join-edge happens-before —— 子线程写，main join 后再读
 * 预期结果: Safe
 */
#include <pthread.h>
int x = 0;
void *worker(void *arg) { x = 1; return 0; }
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  pthread_join(t, 0);          /* join → worker 写 HB main 读 */
  int r = x; (void)r;
  return 0;
}
