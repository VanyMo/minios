#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"

void
find(char *path, char *target, char **cmd_argv, int cmd_argc)
{
    char buf[512], *p;
    int fd;
    struct dirent de;
    struct stat st;

    if ((fd = open(path, O_RDONLY)) < 0)
    {
        fprintf(2, "ls: cannot open %s\n", path);
        return;
    }

    if (fstat(fd, &st) < 0)
    {
        fprintf(2, "ls: cannot stat %s\n", path);
        close(fd);
        return;
    }

    if (strlen(path) + 1 + DIRSIZ + 1 > sizeof buf)
    {
        printf("ls: path too long\n");
        close(fd);
        return;
    }

    strcpy(buf, path);
    p = buf + strlen(buf);
    *p++ = '/';

    while (read(fd, &de, sizeof(de)) == sizeof(de))
    {
        if (de.inum == 0)
            continue;

        memmove(p, de.name, DIRSIZ);
        p[DIRSIZ] = 0;

        if (strcmp(p, ".") == 0 || strcmp(p, "..") == 0)
            continue;
        if (stat(buf, &st) < 0)
        {
            printf("ls: cannot stat %s\n", buf);
            continue;
        }

        if (strcmp(p, target) == 0)
        {
            if (cmd_argv == 0)
            {
                printf("%s\n", buf);
            }
            else
            {
                char *exec_argv[MAXARG];

                int i;

                for (i = 0; i < cmd_argc; i++)
                {
                    exec_argv[i] = cmd_argv[i];
                }

                exec_argv[i] = buf;
                exec_argv[i + 1] = 0;

                int pid = fork();
                if(pid < 0)
                {
                    fprintf(2,"find: fork failed\n");
                    exit(1);
                }
                if (pid == 0)
                {
                    exec(exec_argv[0], exec_argv);
                    fprintf(2, "find: exec failed\n");
                    exit(1);
                }
                else
                {
                    wait(0);
                }
            }
        }

        if (st.type == T_DIR)
        find(buf, target, cmd_argv, cmd_argc);
    }

    close(fd);
}

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        fprintf(2, "usage: find path target [-exec cmd ...]\n");
        exit(1);
    }

    if (argc == 3)
    {
        find(argv[1], argv[2], 0, 0);
    }
    else if (argc >= 5 && strcmp(argv[3], "-exec") == 0)
    {
        find(argv[1], argv[2], &argv[4], argc - 4);
    }
    else
    {
        fprintf(2, "usage: find path target [-exec cmd ...]\n");
        exit(1);
    }

    exit(0);
}