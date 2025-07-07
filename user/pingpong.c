#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
    int p1[2], p2[2];  // 两个管道：p1用于父进程->子进程，p2用于子进程->父进程
    int pid;
    char buf[16];
    
    // 创建第一个管道（父进程->子进程）
    if(pipe(p1) < 0){
        fprintf(2, "pipe failed\n");
        exit(1);
    }
    
    // 创建第二个管道（子进程->父进程）
    if(pipe(p2) < 0){
        fprintf(2, "pipe failed\n");
        exit(1);
    }
    
    pid = fork();
    if(pid < 0){
        fprintf(2, "fork failed\n");
        exit(1);
    }
    else if(pid == 0){
        // 子进程
        close(p1[1]);  // 关闭p1的写端
        close(p2[0]);  // 关闭p2的读端
        
        // 从p1读取父进程发送的"ping"
        if(read(p1[0], buf, 4) != 4){
            fprintf(2, "child read failed\n");
            exit(1);
        }
        
        // 打印接收到的消息
        printf("%d: received ping\n", getpid());
        
        // 通过p2向父进程发送"pong"
        if(write(p2[1], "pong", 4) != 4){
            fprintf(2, "child write failed\n");
            exit(1);
        }
        
        close(p1[0]);
        close(p2[1]);
        exit(0);
    }
    else{
        // 父进程
        close(p1[0]);  // 关闭p1的读端
        close(p2[1]);  // 关闭p2的写端
        
        // 通过p1向子进程发送"ping"
        if(write(p1[1], "ping", 4) != 4){
            fprintf(2, "parent write failed\n");
            exit(1);
        }
        
        // 从p2读取子进程发送的"pong"
        if(read(p2[0], buf, 4) != 4){
            fprintf(2, "parent read failed\n");
            exit(1);
        }
        
        // 打印接收到的消息
        printf("%d: received pong\n", getpid());
        
        close(p1[1]);
        close(p2[0]);
        wait(0);  // 等待子进程结束
        exit(0);
    }
}
