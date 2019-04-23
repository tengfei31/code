#include <stdio.h>
#include "./util/c/syslib/string.h"

#define MAXLINE 1024

int main()
{
    char buf[MAXLINE];
    int buf_len;
    while (fgets(buf, MAXLINE, stdin))
    {
        buf_len = strlen(buf);
        if (buf[buf_len - 1] == '\n') {
            if (buf_len == 1) {
                break;
            }
            buf[buf_len - 1] = '\0';
        }
        printf("from stdin : %s\t buf length:%d\n", buf, strlen(buf));
    }
    
    printf("hello world!!!\n");
    getchar();
    return 0;
}
