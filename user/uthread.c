// =====================================================================
// user/uthread.c —— 用户级线程（协程）库
// =====================================================================
//
// 整体思路：
//   * 一组 struct thread，每个有自己的栈和寄存器快照；
//   * thread_schedule() 选出一个 RUNNABLE 的 next_thread，然后调用
//     thread_switch(old, new) 切换；
//   * thread_switch 用纯汇编做：把当前线程的 callee-save 寄存器
//     存进 old->context，把 new->context 的寄存器恢复出来，最后
//     ret 跳到 new 线程上次暂停时的指令位置。
//
// 与 xv6 内核 swtch() 的关键区别：这里是用户态，纯 C + 内联汇编；
// 不能用 sret/csrr 等特权指令。
//
// C 基础语法提示：
//   struct thread all_thread[MAX_THREAD];
//     定义一个全局数组，含 MAX_THREAD (=4) 个 struct thread 元素。
//   struct thread *current_thread;
//     定义一个指向 struct thread 的指针变量。
//   t->state    等价于  (*t).state   —— "->" 是结构体指针的成员访问。
//   all_thread[i].stack    数组下标访问，".stack" 成员访问。

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

/* Possible states of a thread: */
#define FREE        0x0
#define RUNNING     0x1
#define RUNNABLE    0x2

#define STACK_SIZE  8192
#define MAX_THREAD  4

// 一个用户级线程的完整状态：栈 + 寄存器快照。
//
// 为什么需要保存这些寄存器？
//   当我们从线程 A 切换到 B 时，CPU 的所有寄存器都是 A 最后留下的值，
//   里面包括 A 的局部变量、栈指针、返回地址等。再次调度到 A 时，必须
//   把这些寄存器“还原”到 A 上次离开时的样子，A 才能从断点继续。
//
// RISC-V 调用约定（calling convention）规定：被调用者（callee）必须
// 保存/恢复 ra 和 s0..s11 这 13 个寄存器。其它（a0..a7, t0..t6, gp, tp）
// 是 caller-saved 的，由调用者负责，我们不需要保存。
//
// 也就是说，只要保存 ra + sp + s0..s11，就足够覆盖一次切换需要恢复
// 的所有 callee-save 寄存器。
struct thread_context {
  uint64 ra;     // 返回地址：线程函数返回到这里（即 thread_switch 之后的那条指令）
  uint64 sp;     // 栈指针
  uint64 s0;
  uint64 s1;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
};

struct thread {
  char       stack[STACK_SIZE];     // 线程私有栈
  int        state;                  // FREE, RUNNING, RUNNABLE
  struct thread_context context;     // 线程的寄存器快照
};
struct thread all_thread[MAX_THREAD];
struct thread *current_thread;
extern void thread_switch(uint64, uint64);

// ---------------------------------------------------------------------
// thread_init：把主线程（main 所在的）登记为 RUNNING。
// ---------------------------------------------------------------------
void
thread_init(void)
{
  // main() is thread 0, which will make the first invocation to
  // thread_schedule(). It needs a stack so that the first thread_switch() can
  // save thread 0's state.
  current_thread = &all_thread[0];
  current_thread->state = RUNNING;
}

// ---------------------------------------------------------------------
// thread_schedule：选一个 RUNNABLE 的线程并切换过去。
// ---------------------------------------------------------------------
void
thread_schedule(void)
{
  struct thread *t, *next_thread;

  /* Find another runnable thread. */
  next_thread = 0;
  // 从 current_thread + 1 开始环形搜索
  t = current_thread + 1;
  for(int i = 0; i < MAX_THREAD; i++){
    if(t >= all_thread + MAX_THREAD)
      t = all_thread;
    if(t->state == RUNNABLE) {
      next_thread = t;
      break;
    }
    t = t + 1;
  }

  if (next_thread == 0) {
    printf("thread_schedule: no runnable threads\n");
    exit(-1);
  }

  if (current_thread != next_thread) {         /* switch threads?  */
    // 注意顺序：先把 next_thread 标记成 RUNNING，再切换 current_thread，
    // 这样进入 thread_switch 之后两边的 state 看起来是一致的：
    //   current_thread（被换下的）已经不再 RUNNING；
    //   next_thread（即将跑上的）已经 RUNNING。
    next_thread->state = RUNNING;
    t = current_thread;
    current_thread = next_thread;

    // 真正做上下文切换：把 t->context 写出去，把 current_thread->context
    // 读回来。thread_switch 是 user/uthread_switch.S 中的汇编函数。
    // 传 &t->context 和 &current_thread->context 两个指针，
    // 它会把 callee-save 寄存器压栈到 t->context，再从
    // current_thread->context 恢复出来，最后 ret。
    //
    // 这次调用不会再像普通函数那样"返回"——它会跳到目标线程
    // 上次离开时的 ra 位置。我们自己这一侧会在被再次切回时
    // 才“返回”，返回点就是这里的下一条语句。
    thread_switch((uint64)&t->context, (uint64)&current_thread->context);
  } else
    next_thread = 0;
}

// ---------------------------------------------------------------------
// thread_create：新建一个 RUNNABLE 线程，线程函数是 func。
// ---------------------------------------------------------------------
void
thread_create(void (*func)())
{
  struct thread *t;

  // 在 all_thread 数组里找一个 FREE 槽位
  for (t = all_thread; t < all_thread + MAX_THREAD; t++) {
    if (t->state == FREE) break;
  }
  t->state = RUNNABLE;  // 标记成 RUNNABLE，等待 thread_schedule 调度

  // === 设置初始上下文 ===
  // 关键点：这个线程第一次被调度执行时，会从 thread_switch 切到它，
  // 然后 thread_switch 中的 ret 指令会跳到 context.ra 指向的地址。
  // 因此 context.ra 必须指向我们希望线程开始执行的位置 —— 即
  // 用户提供的线程函数 func 本身（它的原型是 void func(void)，
  // 第一次执行时 ret 直接跳过去即可）。
  //
  // sp 指向 thread->stack 顶部（栈从高地址往低地址长）。
  // C 基础语法：函数指针 (void (*)(void)) 表示"指向无参数无返回值函数的指针"。
  void (*entry)(void) = (void (*)(void))func;
  t->context.ra = (uint64)entry;   // ret 跳到这里
  t->context.sp = (uint64)t->stack + STACK_SIZE;  // 栈顶
  // 其它 s0..s11 默认是 0，但显式清零便于调试。
  t->context.s0 = t->context.s1 = t->context.s2 = t->context.s3 = 0;
  t->context.s4 = t->context.s5 = t->context.s6 = t->context.s7 = 0;
  t->context.s8 = t->context.s9 = t->context.s10 = t->context.s11 = 0;
}

// thread_yield：把当前线程设为 RUNNABLE，然后调度下一个。
void
thread_yield(void)
{
  current_thread->state = RUNNABLE;
  thread_schedule();
}

// ---------------------- 测试用线程 ----------------------
volatile int a_started, b_started, c_started;
volatile int a_n, b_n, c_n;

void
thread_a(void)
{
  int i;
  printf("thread_a started\n");
  a_started = 1;
  while(b_started == 0 || c_started == 0)
    thread_yield();

  for (i = 0; i < 100; i++) {
    printf("thread_a %d\n", i);
    a_n += 1;
    thread_yield();
  }
  printf("thread_a: exit after %d\n", a_n);

  current_thread->state = FREE;
  thread_schedule();
}

void
thread_b(void)
{
  int i;
  printf("thread_b started\n");
  b_started = 1;
  while(a_started == 0 || c_started == 0)
    thread_yield();

  for (i = 0; i < 100; i++) {
    printf("thread_b %d\n", i);
    b_n += 1;
    thread_yield();
  }
  printf("thread_b: exit after %d\n", b_n);

  current_thread->state = FREE;
  thread_schedule();
}

void
thread_c(void)
{
  int i;
  printf("thread_c started\n");
  c_started = 1;
  while(a_started == 0 || b_started == 0)
    thread_yield();

  for (i = 0; i < 100; i++) {
    printf("thread_c %d\n", i);
    c_n += 1;
    thread_yield();
  }
  printf("thread_c: exit after %d\n", c_n);

  current_thread->state = FREE;
  thread_schedule();
}

int
main(int argc, char *argv[])
{
  a_started = b_started = c_started = 0;
  a_n = b_n = c_n = 0;
  thread_init();
  thread_create(thread_a);
  thread_create(thread_b);
  thread_create(thread_c);
  current_thread->state = FREE;
  thread_schedule();
  exit(0);
}
