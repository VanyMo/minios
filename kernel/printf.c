//
// formatted console output -- printf, panic.
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

volatile int panicked = 0;

// lock to avoid interleaving concurrent printf's.
static struct {
  struct spinlock lock;
  int locking;
} pr;

static char digits[] = "0123456789abcdef";

static void
printint(int xx, int base, int sign)
{
  char buf[16];
  int i;
  uint x;

  if(sign && (sign = xx < 0))
    x = -xx;
  else
    x = xx;

  i = 0;
  do {
    buf[i++] = digits[x % base];
  } while((x /= base) != 0);

  if(sign)
    buf[i++] = '-';

  while(--i >= 0)
    consputc(buf[i]);
}

static void
printptr(uint64 x)
{
  int i;
  consputc('0');
  consputc('x');
  for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
    consputc(digits[x >> (sizeof(uint64) * 8 - 4)]);
}

// ----------------------------------------------------------------------
// backtrace()：从当前栈帧开始，沿着 s0 帧指针链向上遍历，
//             把每个栈帧里的“返回地址”打印出来。
//
// 栈帧布局（参考实验提示）：
//   s0 指向的位置 + 8 = 当前栈帧起始（即“父帧指针”位置）
//   [s0 - 8]  = saved ra  （要打印的返回地址）
//   [s0 - 16] = saved s0  （调用者的 s0）
//
// 终止条件：走到当前 CPU 的栈底部。本实验里每个进程的内核栈都恰好
//          是一个 4KB 的页（PGROUNDDOWN(fp) 能算出所属页），
//          因此用 PGROUNDDOWN(fp) 比较，越过页边界就停止。
//
// C 语法说明：
//   uint64 —— RISC-V 上的 64 位无符号整数类型（8 字节）
//   *(uint64 *)fp  —— 先把 fp 强制转换为 (uint64 *) 指针，再解引用，
//                    等价于读取 fp 指向的 8 字节内存作为一个 uint64 值。
//   PGROUNDDOWN(fp) —— 宏定义在 riscv.h 中，把 fp 向下取整到页边界
//                    （即把低 12 位清零）。
//   r_fp()  —— 上一节里实现的内联函数，读取当前 s0 寄存器的值。
// ----------------------------------------------------------------------
void
backtrace(void)
{
  uint64 fp = r_fp();          // 当前栈帧的 s0
  uint64 top = PGROUNDDOWN(fp);  // 当前栈所在的页起始地址

  printf("backtrace:\n");
  for(;;){
    // fp - 8 位置保存的是“调用本函数的那条 call/jal 指令的下一条”
    // 也就是返回后 CPU 应该继续执行的指令地址。
    uint64 saved_ra = *(uint64 *)(fp - 8);
    printf("%p\n", saved_ra);
    // fp - 16 位置保存的是调用者的 s0 帧指针，沿着它往回走就是
    // “调用本函数的函数的栈帧”。
    fp = *(uint64 *)(fp - 16);
    // 已经走出当前内核栈所在的页，说明到了栈底，停止。
    if(PGROUNDDOWN(fp) != top)
      break;
    if(fp == 0)  // 防御性
      break;
  }
}

// Print to the console. only understands %d, %x, %p, %s.
void
printf(char *fmt, ...)
{
  va_list ap;
  int i, c, locking;
  char *s;

  locking = pr.locking;
  if(locking)
    acquire(&pr.lock);

  if (fmt == 0)
    panic("null fmt");

  va_start(ap, fmt);
  for(i = 0; (c = fmt[i] & 0xff) != 0; i++){
    if(c != '%'){
      consputc(c);
      continue;
    }
    c = fmt[++i] & 0xff;
    if(c == 0)
      break;
    switch(c){
    case 'd':
      printint(va_arg(ap, int), 10, 1);
      break;
    case 'x':
      printint(va_arg(ap, int), 16, 1);
      break;
    case 'p':
      printptr(va_arg(ap, uint64));
      break;
    case 's':
      if((s = va_arg(ap, char*)) == 0)
        s = "(null)";
      for(; *s; s++)
        consputc(*s);
      break;
    case '%':
      consputc('%');
      break;
    default:
      // Print unknown % sequence to draw attention.
      consputc('%');
      consputc(c);
      break;
    }
  }
  va_end(ap);

  if(locking)
    release(&pr.lock);
}

void
panic(char *s)
{
  pr.locking = 0;
  printf("panic: ");
  printf(s);
  printf("\n");
  // 在内核崩溃时也打印栈回溯，方便定位
  backtrace();
  panicked = 1; // freeze uart output from other CPUs
  for(;;)
    ;
}

void
printfinit(void)
{
  initlock(&pr.lock, "pr");
  pr.locking = 1;
}
