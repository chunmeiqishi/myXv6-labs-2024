#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/param.h"
#include "user/user.h"

#define MAXLINE 1024  // 每行最大字符数

/**
 * 字符串复制函数 - xv6版本
 * @param dest 目标字符串
 * @param src 源字符串
 */
void my_strcpy(char *dest, const char *src) {
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

/**
 * 字符串连接函数 - xv6版本
 * @param dest 目标字符串
 * @param src 源字符串
 */
void my_strcat(char *dest, const char *src) {
    // 找到目标字符串的末尾
    while (*dest) {
        dest++;
    }
    // 复制源字符串到目标字符串末尾
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

/**
 * 从标准输入读取一行数据
 * @param buf 存储读取数据的缓冲区
 * @param maxlen 缓冲区最大长度
 * @return 读取的字符数，如果到达文件末尾返回0，出错返回-1
 */
int
readline(char *buf, int maxlen)
{
    int i = 0;
    char c;
    
    // 逐个字符读取，直到遇到换行符或文件结束
    while(i < maxlen - 1) {
        // 从标准输入读取一个字符
        int n = read(0, &c, 1);
        
        if(n <= 0) {
            // 文件结束或读取错误
            break;
        }
        
        if(c == '\n') {
            // 遇到换行符，停止读取
            break;
        }
        
        // 将字符存入缓冲区
        buf[i++] = c;
    }
    
    // 添加字符串结束符
    buf[i] = '\0';
    
    return i;
}

/**
 * 将字符串按空格分割成参数数组
 * @param line 要分割的字符串
 * @param argv 存储分割结果的数组
 * @param maxargs 最大参数个数
 * @return 实际参数个数
 */
int
split_args(char *line, char *argv[], int maxargs)
{
    int argc = 0;
    char *p = line;
    
    // 跳过开头的空格
    while(*p == ' ' || *p == '\t') {
        p++;
    }
    
    while(*p != '\0' && argc < maxargs - 1) {
        // 记录当前参数的开始位置
        argv[argc++] = p;
        
        // 找到当前参数的结束位置（空格或字符串结束）
        while(*p != '\0' && *p != ' ' && *p != '\t') {
            p++;
        }
        
        // 如果遇到空格，用'\0'替换以分割参数
        if(*p != '\0') {
            *p = '\0';
            p++;
            
            // 跳过多余的空格
            while(*p == ' ' || *p == '\t') {
                p++;
            }
        }
    }
    
    // 参数数组最后一个元素设为NULL（exec要求）
    argv[argc] = 0;
    
    return argc;
}

/**
 * 执行命令
 * @param cmd 要执行的命令
 * @param args 从标准输入读取的参数
 */
void
run_command(char *cmd, char *args)
{
    char *argv[MAXARG];  // 参数数组
    int argc = 0;
    
    // 第一个参数是命令本身
    argv[argc++] = cmd;
    
    // 如果有从标准输入读取的参数，进行分割
    if(strlen(args) > 0) {
        // 临时保存args，因为split_args会修改字符串
        char temp_args[MAXLINE];
        my_strcpy(temp_args, args);
        
        // 分割参数并添加到argv数组
        char *split_argv[MAXARG];
        int split_argc = split_args(temp_args, split_argv, MAXARG - 1);
        
        // 将分割的参数复制到主argv数组
        for(int i = 0; i < split_argc && argc < MAXARG - 1; i++) {
            argv[argc++] = split_argv[i];
        }
    }
    
    // 参数数组最后一个元素设为NULL
    argv[argc] = 0;
    
    // 创建子进程执行命令
    int pid = fork();
    
    if(pid < 0) {
        // fork失败
        fprintf(2, "xargs: fork failed\n");
        exit(1);
    }
    else if(pid == 0) {
        // 子进程：执行命令
        exec(cmd, argv);
        
        // 如果exec返回，说明执行失败
        fprintf(2, "xargs: exec %s failed\n", cmd);
        exit(1);
    }
    else {
        // 父进程：等待子进程完成
        int status;
        wait(&status);
        
        // 检查子进程是否正常退出
        if(status != 0) {
            fprintf(2, "xargs: command %s failed with status %d\n", cmd, status);
        }
    }
}

int
main(int argc, char *argv[])
{
    // 检查命令行参数
    if(argc < 2) {
        fprintf(2, "Usage: xargs <command> [initial-args...]\n");
        fprintf(2, "Example: echo \"file1\\nfile2\\nfile3\" | xargs rm\n");
        exit(1);
    }
    
    // 获取要执行的命令
    char *command = argv[1];
    
    // 构建初始参数字符串（如果有的话）
    char initial_args[MAXLINE] = "";
    for(int i = 2; i < argc; i++) {
        if(strlen(initial_args) > 0) {
            my_strcat(initial_args, " ");
        }
        my_strcat(initial_args, argv[i]);
    }
    
    
    
    char line[MAXLINE];  // 存储从标准输入读取的每一行
    
    // 不断从标准输入读取行，直到文件结束
    while(1) {
        int len = readline(line, MAXLINE);
        
        if(len <= 0) {
            // 没有更多输入，退出循环
            break;
        }
        
        // 构建完整的参数字符串
        char full_args[MAXLINE];
        my_strcpy(full_args, initial_args);
        
        // 如果有初始参数且当前行不为空，添加空格分隔符
        if(strlen(initial_args) > 0 && strlen(line) > 0) {
            my_strcat(full_args, " ");
        }
        
        // 添加从标准输入读取的参数
        my_strcat(full_args, line);
        
        // 执行命令
        run_command(command, full_args);
    }
    
    exit(0);
}
