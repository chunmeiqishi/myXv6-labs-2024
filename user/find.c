#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"

// 正则表达式匹配函数（来自grep.c）
int match(char*, char*);
int matchhere(char*, char*);
int matchstar(int, char*, char*);

// 从文件路径中提取文件名
char*
basename(char *path)
{
    char *p;
    
    // 找到最后一个斜杠后的字符
    for(p = path + strlen(path); p >= path && *p != '/'; p--)
        ;
    p++;
    
    return p;
}

// 递归查找文件
void
find(char *path, char *pattern)
{
    char buf[512], *p;
    int fd;
    struct dirent de;
    struct stat st;
    
    if((fd = open(path, O_RDONLY)) < 0){
        fprintf(2, "find: cannot open %s\n", path);
        return;
    }
    
    if(fstat(fd, &st) < 0){
        fprintf(2, "find: cannot stat %s\n", path);
        close(fd);
        return;
    }
    
    switch(st.type){
    case T_FILE:
        // 检查文件名是否匹配模式
        if(match(pattern, basename(path))){
            printf("%s\n", path);
        }
        break;
        
    case T_DIR:
        // 检查路径长度
        if(strlen(path) + 1 + DIRSIZ + 1 > sizeof buf){
            printf("find: path too long\n");
            break;
        }
        
        strcpy(buf, path);
        p = buf + strlen(buf);
        *p++ = '/';
        
        // 遍历目录条目
        while(read(fd, &de, sizeof(de)) == sizeof(de)){
            if(de.inum == 0)
                continue;
            
            // 跳过 "." 和 ".." 目录
            if(strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
                continue;
                
            // 复制目录条目名称
            memmove(p, de.name, DIRSIZ);
            p[DIRSIZ] = 0;
            
            // 递归搜索子目录/文件
            find(buf, pattern);
        }
        break;
    }
    
    close(fd);
}

int
main(int argc, char *argv[])
{
    if(argc != 3){
        fprintf(2, "Usage: find <directory> <pattern>\n");
        exit(1);
    }
    
    find(argv[1], argv[2]);
    exit(0);
}

// 正则表达式匹配器（来自grep.c）
// Regexp matcher from Kernighan & Pike,
// The Practice of Programming, Chapter 9.

// match: search for regexp anywhere in text
int
match(char *re, char *text)
{
    if(re[0] == '^')
        return matchhere(re+1, text);
    do{  // must look even if string is empty
        if(matchhere(re, text))
            return 1;
    }while(*text++ != '\0');
    return 0;
}

// matchhere: search for regexp at beginning of text
int matchhere(char *re, char *text)
{
    if(re[0] == '\0')
        return 1;
    if(re[1] == '*')
        return matchstar(re[0], re+2, text);
    if(re[0] == '$' && re[1] == '\0')
        return *text == '\0';
    if(*text!='\0' && (re[0]=='.' || re[0]==*text))
        return matchhere(re+1, text+1);
    return 0;
}

// matchstar: search for c*re at beginning of text
int matchstar(int c, char *re, char *text)
{
    do{  // a * matches zero or more instances
        if(matchhere(re, text))
            return 1;
    }while(*text != '\0' && (*text++ == c || c == '.'));
    return 0;
}
