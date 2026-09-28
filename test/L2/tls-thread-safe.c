/* 测试目标: __thread/thread_local 识别 —— 各线程写自己的 TLS 副本，永不竞态
 * 预期结果: Safe
 */
#include <pthread.h>
__thread int data = 0;
void *thread(void *arg) { data = 1; return 0; }   /* 写本线程自己的副本 */
int main(void) {
  pthread_t t1, t2;
  pthread_create(&t1, 0, thread, 0);
  pthread_create(&t2, 0, thread, 0);
  pthread_join(t1, 0); pthread_join(t2, 0);
  return 0;
}
