// System call numbers
#define SYS_fork    1
#define SYS_exit    2
#define SYS_wait    3
#define SYS_pipe    4
#define SYS_read    5
#define SYS_kill    6
#define SYS_exec    7
#define SYS_fstat   8
#define SYS_chdir   9
#define SYS_dup    10
#define SYS_getpid 11
#define SYS_sbrk   12
#define SYS_sleep  13
#define SYS_uptime 14
#define SYS_open   15
#define SYS_write  16
#define SYS_mknod  17
#define SYS_unlink 18
#define SYS_link   19
#define SYS_mkdir  20
#define SYS_close  21
// [新增] 为 trace 分配新的系统调用号 22（紧接现有最大号 21，不与旧号冲突，
// 保证已有二进制程序的调用号语义不变）。
// 用户态用法：trace(1 << SYS_fork) 表示追踪 fork；
// 掩码 2147483647（低 31 位全 1）表示追踪全部系统调用。
#define SYS_trace  22
