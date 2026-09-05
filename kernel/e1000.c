#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"
#include "net.h"

#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *tx_mbufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *rx_mbufs[RX_RING_SIZE];

// remember where the e1000's registers live.
static volatile uint32 *regs;

struct spinlock e1000_lock;

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
void
e1000_init(uint32 *xregs)
{
  int i;

  initlock(&e1000_lock, "e1000");

  regs = xregs;

  // Reset the device
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++) {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_mbufs[i] = 0;
  }
  regs[E1000_TDBAL] = (uint64) tx_ring;
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;
  
  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_mbufs[i] = mbufalloc(0);
    if (!rx_mbufs[i])
      panic("e1000");
    rx_ring[i].addr = (uint64) rx_mbufs[i]->head;
  }
  regs[E1000_RDBAL] = (uint64) rx_ring;
  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA+1] = 0x5634 | (1<<31);
  // multicast table
  for (int i = 0; i < 4096/32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |  // enable
    E1000_TCTL_PSP |                  // pad short packets
    (0x10 << E1000_TCTL_CT_SHIFT) |   // collision stuff
    (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8<<10) | (6<<20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN | // enable receiver
    E1000_RCTL_BAM |                 // enable broadcast
    E1000_RCTL_SZ_2048 |             // 2048-byte rx buffers
    E1000_RCTL_SECRC;                // strip CRC
  
  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0; // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7); // RXDW -- Receiver Descriptor Write Back
}

// ---------------------------------------------------------------------
// e1000_transmit(m)
//   把一个 mbuf 放进 TX 描述符环，让 E1000 把它发到网络上。
//   m->head 指向 mbuf 数据的起始地址（以太网帧）；
//   m->len  是这个帧的字节数。
//
// 整体流程（参考 E1000 手册 3.3 + 3.4）：
//   1) 读 E1000_TDT 寄存器：E1000 当前期望下一个要发送的描述符下标；
//   2) 检查 tx_ring[idx] 的 status 是否带 E1000_TXD_STAT_DD。
//      DD = "Descriptor Done"：硬件已经把这一格的数据发出去了。
//      如果 DD 还没置位，说明 E1000 还没用完这一格——返回 -1。
//   3) 如果这一格之前放过一个 mbuf（tx_mbufs[idx] != 0），
//      说明已经发完了，可以把它释放（mbuffree）。
//   4) 填 tx_ring[idx]：addr = m->head，length = m->len，
//      cmd = EOP | RS  —— EOP 表示“end of packet”，RS 表示
//      “report status”，让硬件发完这一格后把 DD 置位。
//   5) 把 m 存到 tx_mbufs[idx]，等待以后回收。
//   6) 更新 E1000_TDT 为 (idx+1) % TX_RING_SIZE，
//      通知 E1000 有新包要发。
//   7) 释放锁，返回 0。
//
// 加锁：防止多进程同时 transmit，以及和 e1000_recv（也在同一线程上，
// 但中断/内核线程也可能调用）共享对 ring 的访问。
// ---------------------------------------------------------------------
int
e1000_transmit(struct mbuf *m)
{
  // 抢锁：保护对 tx_ring / tx_mbufs / E1000_TDT 的访问
  acquire(&e1000_lock);

  // 1) 读 E1000_TDT：硬件期望下一个描述符的下标
  uint32 idx = regs[E1000_TDT];

  // 2) 环形溢出检查：如果这一格的 DD 位没置位，
  //    说明硬件还没消费完这一格——返回 -1。
  if((tx_ring[idx].status & E1000_TXD_STAT_DD) == 0){
    release(&e1000_lock);
    return -1;
  }

  // 3) 释放上一次使用这一格的 mbuf（如果存在的话）
  if(tx_mbufs[idx]){
    mbuffree(tx_mbufs[idx]);
  }

  // 4) 填描述符：
  //    addr  : mbuf 数据起始地址（DMA 源）
  //    length: 帧长
  //    cmd   : EOP (last segment of packet) + RS (hardware reports status)
  tx_ring[idx].addr   = (uint64)m->head;
  tx_ring[idx].length = (uint16)m->len;
  tx_ring[idx].cmd    = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
  // status 由硬件写；清零保证下次写之前是干净状态
  tx_ring[idx].status = 0;

  // 5) 记录 mbuf 指针，等发完后再 free
  tx_mbufs[idx] = m;

  // 6) 通知 E1000 推进 tail 指针
  regs[E1000_TDT] = (idx + 1) % TX_RING_SIZE;

  release(&e1000_lock);
  return 0;
}

// ---------------------------------------------------------------------
// e1000_recv()
//   从 RX 描述符环取出硬件已经收到的 mbuf，
//   把数据交给网络栈 net_rx()，再分配新 mbuf 填到描述符中。
//
// 整体流程（参考 E1000 手册 3.2）：
//   1) 读 E1000_RDT：这是“驱动已经处理过的最后下标”。
//      下一个等待处理的描述符是 (RDT + 1) % RX_RING_SIZE。
//   2) 检查 rx_ring[idx].status 是否带 E1000_RXD_STAT_DD：
//      DD=1 表示硬件已经往这一格的 buffer DMA 完一个包。
//   3) 更新 mbuf->len = descriptor.length；调用 net_rx(m)
//      把这一帧交给上层协议栈。
//   4) 分配一个新 mbuf，把 m->head 写到 descriptor.addr，
//      并把 descriptor.status 清零。
//   5) 更新 E1000_RDT 为当前 idx，通知硬件本驱动已经处理完这一格。
// ---------------------------------------------------------------------
static void
e1000_recv(void)
{
  // 加锁：e1000_recv 可能被中断处理调用（e1000_intr），
  // 也可能和 e1000_transmit（被 net.c 在普通内核线程中调用）并发。
  // 注意：tx 和 rx 用的 ring 不同，但都要访问 regs[] 寄存器；
  // e1000_lock 保护整个驱动。
  acquire(&e1000_lock);

  // 1) 找下一个等待处理的描述符
  uint32 idx = (regs[E1000_RDT] + 1) % RX_RING_SIZE;

  // 持续取出所有已完成的包
  while(rx_ring[idx].status & E1000_RXD_STAT_DD){
    // 2) 取出 mbuf，更新长度
    struct mbuf *m = rx_mbufs[idx];
    m->len = rx_ring[idx].length;

    // 3) 把这一帧交给网络栈
    //    net_rx 内部可能阻塞（在锁外时），但当前持有 e1000_lock；
    //    本实验 xv6 的 net_rx 不会长时间阻塞。
    net_rx(m);

    // 4) 分配新的 mbuf 替换之
    rx_mbufs[idx] = mbufalloc(0);
    if(!rx_mbufs[idx])
      panic("e1000_recv");
    rx_ring[idx].addr = (uint64)rx_mbufs[idx]->head;
    rx_ring[idx].status = 0;

    // 5) 推进到下一个
    idx = (idx + 1) % RX_RING_SIZE;
  }

  // 把 E1000_RDT 更新成"驱动已经处理到的最后一格"
  // 即刚刚处理过的最后一格的 idx
  regs[E1000_RDT] = (idx - 1 + RX_RING_SIZE) % RX_RING_SIZE;

  release(&e1000_lock);
}

void
e1000_intr(void)
{
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  e1000_recv();
}
