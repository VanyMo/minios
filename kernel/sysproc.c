#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  // argint：从用户 trapframe 的 a0 寄存器里取出第 0 个系统调用参数。
  // 如果用户传负数，把 n 改成 0 表示“立刻返回”，避免循环时死等。
  argint(0, &n);
  if(n < 0)
    n = 0;

  // acquire(&tickslock) 是 xv6 的自旋锁。
  // 锁保护两个对象：
  //   (a) 全局变量 ticks —— 每次 clockintr() 把它 +1
  //   (b) 任何在 ticks 上 sleep() 的进程链表
  // 由于这里要读取 ticks 并可能进入 sleep，必须先拿锁。
  acquire(&tickslock);
  ticks0 = ticks;

  // 实验要求：在 sys_sleep 入口处打印一次栈回溯，
  // 这样 bttest 调用 sleep(1) 时就会触发 backtrace 输出。
  backtrace();

  // 主循环：只要距离 ticks0 还不够 n 个 tick，就睡在 &ticks 这个 chan 上。
  // sleep(chan, lock) 内部会原子地：
  //   (1) 释放 lock
  //   (2) 把当前进程置为 SLEEPING 并加入 chan 的等待队列
  //   (3) 调度走
  // wakeup(chan) 被调用时，会把 chan 上所有 SLEEPING 的进程唤醒为 RUNNABLE。
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      // 进程被 kill 了，立刻退出
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

// ----------------------------------------------------------------------
// sys_sigalarm：注册一个周期性的用户态“信号”处理函数。
//
//   用户调用 sigalarm(n, fn) 后，内核每经过 n 个时钟 tick，
//   就在下一次进入 usertrap() 时（且当时不在 handler 内）把
//   用户的 PC 切换到 fn。当 fn 返回前必须调用 sigreturn()。
//
//   调用 sigalarm(0, 0) 表示取消注册。
//
// 实现要点：
//   - argint  / argaddr  分别取出第 0、1 号系统调用参数
//     （用户态通过 a0、a1 寄存器传递，trapframe 已保存好）
//   - 直接修改当前进程 p 的 alarm_* 字段即可
//   - 不需要 acquire(&p->lock)，因为只有当前进程自己会调 sigalarm
// ----------------------------------------------------------------------
uint64
sys_sigalarm(void)
{
  int   n;       // 周期：多少个 tick 触发一次 handler
  uint64 fn;     // handler 函数指针
  struct proc *p = myproc();

  argint (0, &n);    // 取出第 0 个 int 参数（ticks）
  argaddr(1, &fn);   // 取出第 1 个 64-bit 参数（handler 地址）

  // 直接写入当前进程结构。注意 p->lock 不必持有，因为：
  //   - 只有本进程会调 sigalarm，不会和其他进程并发
  //   - 本进程此刻在内核里执行，不可能在用户态同时再调一次
  p->alarm_interval = n;                   // 0 表示取消
  p->alarm_handler  = (void (*)())fn;      // 函数指针
  p->alarm_ticks    = 0;                   // 重新计数

  return 0;
}

// ----------------------------------------------------------------------
// sys_sigreturn：从用户态的 alarm handler 返回。
//
// 真正的实现就是把 trapframe 恢复成进入 handler 之前保存的那份
// (p->alarm_tf)，并清掉 alarm_on 标志位（解除“handler 进行中”状态）。
//
// 关键点：
//   *p->trapframe = p->alarm_tf;  // 整个 280 字节的 trapframe 全部恢复，
//                                 // 包括 a0~a7、s0~s11、ra、sp、epc 等
//   p->alarm_on = 0;             // 允许下次 tick 再次进入 handler
// 返回值不重要，因为恢复后 a0 也变成了用户态在被中断时 a0 的值。
// ----------------------------------------------------------------------
uint64
sys_sigreturn(void)
{
  struct proc *p = myproc();

  // 把之前保存的 trapframe 整个拷回去。
  // 这里用结构体赋值（=），编译器会展开为 memcpy，效果等价。
  *p->trapframe = p->alarm_tf;
  // 标志位清零，允许下一次 tick 再次进入 handler
  p->alarm_on = 0;
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}
