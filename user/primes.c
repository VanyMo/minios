#include "kernel/types.h"
#include "user/user.h"

int fork(void);
int pipe(int *);
int write(int, const void *, int);
int read(int, void *, int);
int getpid(void);

int main()
{
    int m;
    int n;
    int pipe1[2];
    int pid;
    int received;;
    pipe(pipe1);
    pid = fork();
    if (pid > 0)
    {
    for
        (m = 2; m <= 35; m++)
        {
    int primes= 1;
  
   
    for (n=2; n * n <= m; n++)
    {
        if (m % n == 0) {
        primes = 0;
        break;
    }
        }
        if (primes ==1)
        {
            close(pipe1[0]);
            write(pipe1[1], &m, sizeof(m));
           
        }
    }
    close(pipe1[1]);
    wait(0);
}
    else if (pid == 0)
    {
        close(pipe1[1]);
        while (read(pipe1[0], &received, sizeof(received))
               == sizeof(received))
        {
            printf("prime %d\n", received);
        }
        
    }
    close(pipe1[0]);
    exit(0);
    }

