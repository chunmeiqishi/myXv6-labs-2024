#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
    int p[2];
    int pid;
    int prime, n;
    int current_read_fd;
    
    // 创建第一个管道
    if(pipe(p) < 0){
        fprintf(2, "pipe failed\n");
        exit(1);
    }
    
    // 创建生成器进程
    pid = fork();
    if(pid < 0){
        fprintf(2, "fork failed\n");
        exit(1);
    }
    else if(pid == 0){
        // 子进程：数字生成器
        close(p[0]);  // 关闭读端
        
        // 将数字2到280写入管道
        for(int i = 2; i <= 280; i++){
            if(write(p[1], &i, sizeof(int)) != sizeof(int)){
                fprintf(2, "write failed\n");
                exit(1);
            }
        }
        
        close(p[1]);  // 关闭写端
        exit(0);
    }
    else{
        // 父进程：筛选器链
        close(p[1]);  // 关闭写端
        current_read_fd = p[0];
        
        // 持续创建筛选器进程
        while(1){
            // 读取素数
            if(read(current_read_fd, &prime, sizeof(int)) != sizeof(int)){
                // 没有更多数据
                break;
            }
            
            printf("prime %d\n", prime);
            
            // 创建新的管道用于下一级筛选
            int new_p[2];
            if(pipe(new_p) < 0){
                fprintf(2, "pipe failed\n");
                exit(1);
            }
            
            pid = fork();
            if(pid < 0){
                fprintf(2, "fork failed\n");
                exit(1);
            }
            else if(pid == 0){
                // 子进程：筛选器
                close(new_p[0]);  // 关闭新管道的读端
                
                // 筛选数据：读取当前管道的数据，过滤后写入新管道
                while(read(current_read_fd, &n, sizeof(int)) == sizeof(int)){
                    if(n % prime != 0){
                        if(write(new_p[1], &n, sizeof(int)) != sizeof(int)){
                            fprintf(2, "write failed\n");
                            exit(1);
                        }
                    }
                }
                
                close(current_read_fd);
                close(new_p[1]);
                exit(0);
            }
            else{
                // 父进程：继续处理下一级
                close(current_read_fd);  // 关闭当前读端
                close(new_p[1]);  // 关闭新管道的写端
                current_read_fd = new_p[0];  // 使用新管道的读端
            }
        }
        
        close(current_read_fd);
        
        // 等待所有子进程
        while(wait(0) > 0)
            ;
    }
    
    exit(0);
}
