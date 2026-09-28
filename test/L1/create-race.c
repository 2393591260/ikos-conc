/* 测试目标: pthread_create —— main 在创建线程【后】写共享变量，与子线程读并发
 * 预期结果: Race
 */
#include <pthread.h>
int x = 0;
void *worker(void *arg) { int r = x; (void)r; return 0; }
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  x = 1;                       /* 写发生在 create 之后 → 与 worker 读并发 */
  pthread_join(t, 0);
  return 0;
}
