#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/param.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
    if(argc < 2){
        fprintf(2, "usage: xargs command [args...]\n");
        exit(1);
    }

    char buf[512];
    char *exec_argv[MAXARG];
    int base_argc = argc - 1;

  
    for(int i = 0; i < base_argc; i++){
        exec_argv[i] = argv[i + 1];
    }

    int n = 0;
    char c;

    while(read(0, &c, 1) == 1){
        if(c == '\n'){
            buf[n] = 0;

           
            exec_argv[base_argc] = buf;
            exec_argv[base_argc + 1] = 0;

            int pid = fork();

            if(pid < 0){
                fprintf(2, "xargs: fork failed\n");
                exit(1);
            }

            if(pid == 0){
                exec(exec_argv[0], exec_argv);
                fprintf(2, "xargs: exec failed\n");
                exit(1);
            }

            wait(0);
            n = 0;
        }
        else{
            if(n < sizeof(buf) - 1)
                buf[n++] = c;
        }
    }

   
    if(n > 0){
        buf[n] = 0;

        exec_argv[base_argc] = buf;
        exec_argv[base_argc + 1] = 0;

        int pid = fork();

        if(pid < 0){
            fprintf(2, "xargs: fork failed\n");
            exit(1);
        }

        if(pid == 0){
            exec(exec_argv[0], exec_argv);
            fprintf(2, "xargs: exec failed\n");
            exit(1);
        }

        wait(0);
    }

    exit(0);
}