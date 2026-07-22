#include "kernel/types.h"
#include "user/user.h"

int fork(void);
int pipe(int *);
int write(int, const void *, int);
int read(int, void *, int);
int getpid(void);


int main()
{
    int pipe1[2];
    int pipe2[2];
    char buf[10];
    int pid;
    pipe(pipe1);
    pipe(pipe2);
    pid = fork();
    if (pid > 0)
    {
        write(pipe1[1], "ping", 4);
        read(pipe2[0], buf, 4);
        printf("%d: received pong\n", getpid());
    }
    else
    {
        read(pipe1[0], buf, 4);
        printf("%d: received ping\n", getpid());
        write(pipe2[1], "pong", 4);
    }
    exit(0);
}
