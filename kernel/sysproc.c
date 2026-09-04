#include "types.h"
#include "riscv.h"
#include "param.h"
#include "defs.h"
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


  argint(0, &n);
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


#ifdef LAB_PGTBL
int
sys_pgaccess(void)
{
  // lab pgtbl: 报告用户地址区间内哪些页被访问过（PTE_A 位）。
  //
  // 系统调用签名：int pgaccess(void *base, int len, void *mask);
  //   base: 起始用户虚拟地址
  //   len : 需要检查的页数（最多 64，因为返回值是 uint64 掩码）
  //   mask: 输出缓冲区，调用者提供的 8 字节无符号整数位图
  // 返回 0 表示成功，-1 表示失败。
  uint64 base;
  int   len;
  uint64 maskaddr;
  uint64 abits;          // 用于收集访问位的位图
  pte_t *pte;
  struct proc *p = myproc();

  // 通过 argaddr/argint 取出第 0、1、2 号系统调用参数。
  // 注意：argaddr 取出的是用户态传来的地址，内核需要通过 copyout 写回去。
  argaddr(0, &base);
  argint (1, &len);
  argaddr(2, &maskaddr);

  // 限制一次最多只能检查 64 页（也即 64 位掩码的最大值）。
  if(len > 64)
    len = 64;

  abits = 0;
  // 逐页遍历 [base, base + len*PGSIZE)：
  //   - walk() 找到该虚拟地址对应的叶子 PTE；
  //   - 若 PTE 存在，则读取 PTE_A 位并把对应位写入 abits；
  //   - 读取完后由软件把 PTE_A 清零（重新“清白”），这样下一次调用
  //     就能准确报告自从上次检查以来被访问过的页。
  for(int i = 0; i < len; i++){
    uint64 va = base + i * PGSIZE;
    pte = walk(p->pagetable, va, 0);
    if(pte == 0)
      continue;                       // 该页未映射，对应位保持 0
    if((*pte & PTE_V) == 0)
      continue;                       // 同样视为未映射
    if(*pte & PTE_A){
      abits |= (1L << i);             // 第 i 页被访问过
      *pte &= ~PTE_A;                 // 清除 PTE_A，重新累积
    }
  }

  // 把结果 abits 拷贝回用户态的 mask 指针。
  if(copyout(p->pagetable, maskaddr, (char *)&abits, sizeof(abits)) < 0)
    return -1;
  return 0;
}

uint64
sys_kpgtbl(void)
{
  // 打印当前进程的用户页表。
  // 实验要求这个系统调用以 vmprint() 实现的可读格式输出页表内容，
  // 用于调试和可视化。这里没有用户态参数：直接打印当前进程的 pagetable。
  struct proc *p = myproc();
  vmprint(p->pagetable);
  return 0;
}
#endif

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
