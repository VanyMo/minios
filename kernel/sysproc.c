#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sysinfo.h"   // [新增] struct sysinfo 的定义（freemem/nproc 两个字段）

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

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
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

// [新增] trace 系统调用的内核态实现。
//
// 流程逻辑：
//   用户态 trace(mask)
//     -> usys.S 桩: li a7, SYS_trace; ecall      （陷入内核）
//     -> usertrap() 发现是 ecall，调用 syscall() （kernel/trap.c）
//     -> syscall() 查 syscalls[] 表得到本函数并调用
//     -> 本函数取出参数、保存掩码，返回 0 表示成功
//     -> syscall() 把返回值写回 trapframe->a0，用户态拿到 0
//
// 实现要点：trace 是"进程属性设置"类系统调用（类似 sys_exit 取参数的方式），
// 只需把掩码记到当前进程的 proc 结构中即可，真正的打印发生在
// kernel/syscall.c 的 syscall() 里——每次系统调用返回前检查掩码。
uint64
sys_trace(void)
{
  int mask;                    // [新增] 存放用户传入的掩码参数

  argint(0, &mask);            // [新增] 取第 0 个 int 型系统调用参数（在 a0 寄存器中），
                               // 与 sys_exit()/sys_kill() 的取参方式一致
  myproc()->trace_mask = mask; // [新增] 把掩码保存到当前进程的 proc 结构。
                               // 之后该进程的每一次系统调用都会先经过 syscall() 检查此掩码
  return 0;                    // [新增] 返回 0 表示设置成功（user/trace.c 检查 <0 才报错）
}

// [新增] sysinfo 系统调用的内核态实现。
//
// 功能：把"系统当前状态"（空闲内存字节数、活动进程数）填进一个
// struct sysinfo，再安全地拷贝到用户程序提供的内存地址里。
//
// 流程逻辑（对照 sys_fstat + filestat 的经典写法）：
//   用户态 sysinfo(&info)
//     -> usys.S 桩: li a7, SYS_sysinfo; ecall   （陷入内核）
//     -> syscall() 查表调用本函数
//     -> 本函数：① 取用户指针 ② 内核里填好结构体 ③ copyout 拷回用户空间
//     -> 返回 0 成功；返回 -1 失败（通常是用户传了非法指针）
//
// 为什么必须用 copyout()：用户传来的 addr 是用户虚拟地址，
// 内核不能直接解引用（用户页表和内核页表映射不同，直接写会写错地方
// 甚至崩溃）。copyout() 会借助当前进程的页表逐页安全地搬运数据，
// 遇到未映射/越界地址会返回 -1 而不是崩溃——这正是测试里
// "sysinfo 传坏指针必须失败" 所要求的行为。
uint64
sys_sysinfo(void)
{
  uint64 addr;                 // [新增] 用户传来的指针（指向用户空间的 struct sysinfo）
  struct sysinfo info;         // [新增] 先在内核栈上准备一份结构体，填好后再拷出去
  struct proc *p = myproc();   // [新增] 当前进程，copyout 需要它的用户页表

  argaddr(0, &addr);           // [新增] 取第 0 个指针型系统调用参数（用户传的 &info）

  info.freemem = kfreemem();   // [新增] 调 kalloc.c 的新函数：空闲物理内存字节数
  info.nproc = nproc_active(); // [新增] 调 proc.c 的新函数：状态非 UNUSED 的进程数

  // [新增] 把填好的结构体从内核内存拷贝到用户内存 addr 处。
  // 拷贝失败（坏指针/越界）时返回 -1，用户态会看到 sysinfo() 返回 -1。
  if(copyout(p->pagetable, addr, (char *)&info, sizeof(info)) < 0)
    return -1;

  return 0;                    // [新增] 返回 0 表示成功
}
