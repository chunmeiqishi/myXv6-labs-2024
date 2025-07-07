#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
    // 检查命令行参数数量
    if (argc != 2) {
        fprintf(2, "Usage: %s <ticks>\n", argv[0]);
        exit(1);
    }
    
    // 将字符串参数转换为整数
    int ticks = atoi(argv[1]);
    
    // 检查参数是否为有效的正整数
    if (ticks < 0) {
        fprintf(2, "sleep: invalid number of ticks: %s\n", argv[1]);
        exit(1);
    }
    
    // 调用系统调用sleep，暂停指定的时间片数
    if (sleep(ticks) < 0) {
        fprintf(2, "sleep: system call failed\n");
        exit(1);
    }
    
    // 正常退出
    exit(0);
}