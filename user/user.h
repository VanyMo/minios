struct stat;
struct sysinfo;   // [新增] 前向声明：告诉编译器存在 struct sysinfo 类型。
                  // 下面 sysinfo() 原型只用到它的指针，不需要完整定义
                  // （完整定义在 kernel/sysinfo.h，由用户程序自己 include）。

// system calls
int fork(void);
int exit(int) __attribute__((noreturn));
int wait(int*);
int pipe(int*);
int write(int, const void*, int);
int read(int, void*, int);
int close(int);
int kill(int);
int exec(const char*, char**);
int open(const char*, int);
int mknod(const char*, short, short);
int unlink(const char*);
int fstat(int fd, struct stat*);
int link(const char*, const char*);
int mkdir(const char*);
int chdir(const char*);
int dup(int);
int getpid(void);
char* sbrk(int);
int sleep(int);
int uptime(void);
// [新增] trace 系统调用的用户态原型：
// 用户程序（如 user/trace.c）调用 trace(mask) 时，链接器需要在这里找到函数声明，
// 其实现位于由 user/usys.pl 生成的 user/usys.S 汇编桩中。
// 参数 mask 是整数掩码，第 n 位为 1 表示追踪系统调用号 n；返回 0 表示成功。
int trace(int);
// [新增] sysinfo 系统调用的用户态原型：
// 用户程序（如 user/sysinfotest.c）调用 sysinfo(&info) 时链接器需要此声明，
// 实现同样位于 usys.pl 生成的 usys.S 汇编桩中。
// 参数是用户空间一个 struct sysinfo 的地址，内核负责把统计结果填进去；
// 返回 0 表示成功，-1 表示失败（如传了非法指针）。
int sysinfo(struct sysinfo *);

// ulib.c
int stat(const char*, struct stat*);
char* strcpy(char*, const char*);
void *memmove(void*, const void*, int);
char* strchr(const char*, char c);
int strcmp(const char*, const char*);
void fprintf(int, const char*, ...);
void printf(const char*, ...);
char* gets(char*, int max);
uint strlen(const char*);
void* memset(void*, int, uint);
void* malloc(uint);
void free(void*);
int atoi(const char*);
int memcmp(const void *, const void *, uint);
void *memcpy(void *, const void *, uint);
