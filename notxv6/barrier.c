// =====================================================================
// notxv6/barrier.c —— pthread barrier 的实现
// =====================================================================
//
// 题目要求：所有线程都调用 barrier() 之后才一起继续；
// 还要支持多轮（每轮都重新同步）。
//
// 关键 pthread 原语：
//   pthread_mutex_lock(&m)   互斥进入临界区
//   pthread_mutex_unlock(&m) 离开临界区
//   pthread_cond_wait(&c, &m)
//     释放 m 并阻塞在条件变量 c 上；
//     当被 broadcast/signal 唤醒后，重新拿回 m 并返回。
//     返回时 m 是被加锁的，调用者可以安全访问受保护的数据。
//   pthread_cond_broadcast(&c)
//     唤醒所有等待在 c 上的线程。
//
// 实现思路：
//   bstate.nthread 记录“本轮已经调用 barrier() 的线程数”。
//   每个线程进入 barrier 时：
//     1) 加锁 mutex
//     2) nthread++
//     3) 如果 nthread == 总线程数，自己是最后一个到的：
//           把 nthread 清零
//           把 round + 1
//           broadcast 唤醒其他所有线程
//           解锁、返回
//        否则还没到齐：
//           cond_wait 释放 mutex，阻塞
//           醒来后已经重新拿到 mutex，直接解锁、返回
//
// 关键陷阱：必须先 ++nthread 再判断；cond_wait 必须在持锁状态下调用；
// 唤醒后必须重新检查条件（用 while，不是 if）。
//
// C 基础语法提醒：
//   pthread_mutex_t / pthread_cond_t  分别是 mutex、条件变量的类型。
//   pthread_mutex_init(&m, NULL) 用默认属性初始化。
//   pthread_cond_init(&c, NULL)    同上。

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>

// 用户从命令行传入的“总线程数”
static int nthread = 1;
// 当前轮次（每轮 barrier() 完成都自增）
static int round = 0;

// 共享 barrier 状态
struct barrier {
  pthread_mutex_t barrier_mutex;   // 保护下面两个字段
  pthread_cond_t  barrier_cond;    // 用于让线程睡眠 / 唤醒
  int nthread;     // 本轮已到达 barrier() 的线程数
  int round;       // 当前的轮次
} bstate;

static void
barrier_init(void)
{
  // 初始化 mutex（NULL 表示用默认属性）
  assert(pthread_mutex_init(&bstate.barrier_mutex, NULL) == 0);
  // 初始化条件变量
  assert(pthread_cond_init(&bstate.barrier_cond, NULL) == 0);
  bstate.nthread = 0;
}

// barrier() —— 所有 nthread 线程都调用一次 barrier 之后才放行
static void
barrier()
{
  // 1) 抢锁
  pthread_mutex_lock(&bstate.barrier_mutex);

  // 2) 记录自己已经到达
  bstate.nthread++;

  // 3) 判断自己是不是最后一个到的
  if (bstate.nthread == nthread) {
    // 是最后一个：进入下一轮 + 唤醒其他线程
    bstate.round++;          // 这一轮 barrier 完成，轮次 +1
    bstate.nthread = 0;      // 下一轮重新计数
    pthread_cond_broadcast(&bstate.barrier_cond);
    // 最后一个到达的线程直接解锁、返回，不需要再等
    pthread_mutex_unlock(&bstate.barrier_mutex);
  } else {
    // 还没到齐：把自己挂起到 cond 上
    // cond_wait 内部会原子地：
    //   - 释放 mutex
    //   - 阻塞当前线程
    // 被 broadcast 唤醒后会：
    //   - 重新获得 mutex
    //   - 返回
    pthread_cond_wait(&bstate.barrier_cond, &bstate.barrier_mutex);
    // 醒来时 mutex 已经重新加锁，我们释放它即可
    pthread_mutex_unlock(&bstate.barrier_mutex);
  }
}

static void *
thread(void *xa)
{
  long n = (long) xa;
  long delay;
  int i;

  for (i = 0; i < 20000; i++) {
    int t = bstate.round;   // 抓取当前轮次
    assert (i == t);        // 应当等于自己循环变量（因为 barrier 同步了轮次）
    barrier();
    usleep(random() % 100);
  }

  return 0;
}

int
main(int argc, char *argv[])
{
  pthread_t *tha;
  void *value;
  long i;
  double t1, t0;

  if (argc < 2) {
    fprintf(stderr, "%s: %s nthread\n", argv[0], argv[0]);
    exit(-1);
  }
  nthread = atoi(argv[1]);
  tha = malloc(sizeof(pthread_t) * nthread);
  srandom(0);

  barrier_init();

  for(i = 0; i < nthread; i++) {
    assert(pthread_create(&tha[i], NULL, thread, (void *) i) == 0);
  }
  for(i = 0; i < nthread; i++) {
    assert(pthread_join(tha[i], &value) == 0);
  }
  printf("OK; passed\n");
}
