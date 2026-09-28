/* 测试目标: 对照 —— 普通共享变量（非 TLS）两线程写，应报 Race
 * 预期结果: Race
 */
#include <pthread.h>
int data = 0;   /* 普通全局，非 TLS */
void *thread(void *arg) { data = 1; return 0; }
int main(void) {
  pthread_t t1, t2;
  pthread_create(&t1, 0, thread, 0);
  pthread_create(&t2, 0, thread, 0);
  pthread_join(t1, 0); pthread_join(t2, 0);
  return 0;
}
