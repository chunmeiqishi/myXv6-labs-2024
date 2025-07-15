//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;
  
  argaddr(1, &p);
  argint(2, &n);
  if(argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);
  if(argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0){
    iunlockput(dp);
    return 0;
  }

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  if(dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if(type == T_DIR){
    // now that success is guaranteed:
    dp->nlink++;  // for ".."
    iupdate(dp);
  }

  iunlockput(dp);

  return ip;

 fail:
  // something went wrong. de-allocate ip.
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  argint(1, &major);
  argint(2, &minor);
  if((argstr(0, path, MAXPATH)) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if(argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}
#ifdef LAB_MMAP
uint64
sys_mmap(void)
{
  uint64 addr;
  int length, prot, flags, fd, offset;
  struct file *f;
  struct proc *p = myproc();
  
  // Get arguments
  argaddr(0, &addr);
  argint(1, &length);
  argint(2, &prot);
  argint(3, &flags);
  argint(4, &fd);
  argint(5, &offset);
  
  // Validate arguments
  if(addr != 0) // We require addr to be 0
    return -1;
  if(length <= 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f = p->ofile[fd]) == 0)
    return -1;
  if((prot & PROT_READ) && !f->readable)
    return -1;
  if((prot & PROT_WRITE) && !f->writable && (flags & MAP_SHARED))
    return -1;
    
  // Find an unused VMA slot
  struct vma *vma = 0;
  for(int i = 0; i < NVMA; i++) {
    if(!p->vmas[i].used) {
      vma = &p->vmas[i];
      break;
    }
  }
  if(!vma)
    return -1; // No free VMA slots
    
  // Find a free virtual address range
  uint64 va = PGROUNDUP(p->sz);
  
  // Make sure we don't overlap with existing VMAs
  int found_space = 0;
  while(!found_space && va + length < MAXVA) {
    found_space = 1;
    // Check if this range overlaps with any existing VMA
    for(int i = 0; i < NVMA; i++) {
      if(p->vmas[i].used) {
        uint64 vma_start = p->vmas[i].addr;
        uint64 vma_end = vma_start + p->vmas[i].len;
        // Check for overlap
        if(!(va + length <= vma_start || va >= vma_end)) {
          found_space = 0;
          va = PGROUNDUP(vma_end);
          break;
        }
      }
    }
  }
  
  if(!found_space)
    return -1; // No space available
  
  // Initialize VMA
  vma->used = 1;
  vma->addr = va;
  vma->len = length;
  vma->prot = prot;
  vma->flags = flags;
  vma->f = f;
  vma->offset = offset;
  
  // Increase file reference count
  filedup(f);
  
  return va;
}

uint64
sys_munmap(void)
{
  uint64 addr;
  int length;
  struct proc *p = myproc();
  
  argaddr(0, &addr);
  argint(1, &length);
  
  // Find the VMA that contains this address
  struct vma *vma = 0;
  for(int i = 0; i < NVMA; i++) {
    if(p->vmas[i].used && p->vmas[i].addr <= addr && 
       addr < p->vmas[i].addr + p->vmas[i].len) {
      vma = &p->vmas[i];
      break;
    }
  }
  
  if(!vma)
    return -1; // No VMA found
    
  // Check if this is a valid unmap operation
  if(addr < vma->addr || addr + length > vma->addr + vma->len)
    return -1;
  
  // Ensure addr and length are page-aligned
  if(addr % PGSIZE != 0 || length % PGSIZE != 0)
    return -1;
    
  // Unmap pages and write back if needed
  for(uint64 va = addr; va < addr + length; va += PGSIZE) {
    pte_t *pte = walk(p->pagetable, va, 0);
    if(pte && (*pte & PTE_V)) {
      // If it's a shared mapping, write it back
      if((vma->flags & MAP_SHARED)) {
        // Write the page back to the file within a transaction
        uint64 pa = PTE2PA(*pte);
        int off = vma->offset + (va - vma->addr);
        begin_op();
        ilock(vma->f->ip);
        // Don't extend the file beyond its original size
        int write_size = PGSIZE;
        if(off + PGSIZE > vma->f->ip->size) {
          if(off >= vma->f->ip->size) {
            write_size = 0;
          } else {
            write_size = vma->f->ip->size - off;
          }
        }
        if(write_size > 0) {
          if(writei(vma->f->ip, 0, pa, off, write_size) != write_size) {
            // Write failed, but continue unmapping
          }
        }
        iunlock(vma->f->ip);
        end_op();
      }
      // Unmap the single page
      uvmunmap(p->pagetable, va, 1, 1);
    }
  }
  
  // Update or remove VMA
  if(addr == vma->addr && length == vma->len) {
    // Unmapping entire VMA
    fileclose(vma->f);
    vma->used = 0;
  } else if(addr == vma->addr) {
    // Partial unmap from the beginning
    vma->addr += length;
    vma->len -= length;
    vma->offset += length;
    // Don't close the file for partial unmap
  } else if(addr + length == vma->addr + vma->len) {
    // Partial unmap from the end
    vma->len -= length;
    // Don't close the file for partial unmap
  } else {
    // Unmap from the middle - split VMA
    // Find a free VMA slot for the second part
    struct vma *new_vma = 0;
    for(int i = 0; i < NVMA; i++) {
      if(!p->vmas[i].used) {
        new_vma = &p->vmas[i];
        break;
      }
    }
    if(!new_vma)
      return -1; // No free VMA slots
      
    // Set up new VMA for the part after the unmapped region
    new_vma->used = 1;
    new_vma->addr = addr + length;
    new_vma->len = vma->addr + vma->len - (addr + length);
    new_vma->prot = vma->prot;
    new_vma->flags = vma->flags;
    new_vma->f = vma->f;
    new_vma->offset = vma->offset + (addr + length - vma->addr);
    filedup(vma->f); // Increase reference count for the new VMA
    
    // Update original VMA to represent the part before the unmapped region
    vma->len = addr - vma->addr;
    // Don't close the file for partial unmap
  }
  
  return 0;
}

// Handle page fault for mmap regions
int
mmap_handler(uint64 va)
{
  struct proc *p = myproc();
  
  // Find the VMA that contains this address
  struct vma *vma = 0;
  for(int i = 0; i < NVMA; i++) {
    if(p->vmas[i].used && p->vmas[i].addr <= va && 
       va < p->vmas[i].addr + p->vmas[i].len) {
      vma = &p->vmas[i];
      break;
    }
  }
  
  if(!vma)
    return -1; // Not in any mmap region
    
  // Round down to page boundary
  va = PGROUNDDOWN(va);
  
  // Check if the page is already mapped
  pte_t *pte = walk(p->pagetable, va, 0);
  if(pte && (*pte & PTE_V)) {
    // Page is already mapped, this might be a permission fault
    // Return -1 to let the normal page fault handler deal with it
    return -1;
  }
  
  // Allocate a physical page
  char *mem = kalloc();
  if(mem == 0)
    return -1;
  memset(mem, 0, PGSIZE);
  
  // Read the page from the file
  int offset = vma->offset + (va - vma->addr);
  ilock(vma->f->ip);
  int n = readi(vma->f->ip, 0, (uint64)mem, offset, PGSIZE);
  iunlock(vma->f->ip);
  
  if(n < 0) {
    kfree(mem);
    return -1;
  }
  
  // Set up page table entry permissions
  int perm = PTE_U;
  if(vma->prot & PROT_READ)
    perm |= PTE_R;
  if(vma->prot & PROT_WRITE)
    perm |= PTE_W;
  if(vma->prot & PROT_EXEC)
    perm |= PTE_X;
  
  // Map the page
  if(mappages(p->pagetable, va, PGSIZE, (uint64)mem, perm) != 0) {
    kfree(mem);
    return -1;
  }
  
  return 0;
}
#endif
