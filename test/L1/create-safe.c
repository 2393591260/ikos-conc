/* 测试目标: pthread_create 的 create-edge happens-before —— main 在创建线程【前】写共享变量，子线程读
 * 预期结果: Safe
 */
#include <pthread.h>
int x = 0;
void *worker(void *arg) { int r = x; (void)r; return 0; }
int main(void) {
  x = 1;                       /* 写发生在 create 之前 → HB 子线程读 */
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  pthread_join(t, 0);
  return 0;
}
