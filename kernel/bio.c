// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKETS 13  // 使用质数以减少哈希冲突

struct {
  struct buf buf[NBUF]; // 所有的缓冲区
  struct spinlock lock; // 用于分配新缓冲区的全局锁
} bcache;

// 哈希桶结构
struct bucket {
  struct spinlock lock;       // 每个桶的锁
  struct buf head;           // 桶中缓冲区的链表头
} bcache_buckets[NBUCKETS];

// 计算哈希值的辅助函数
static uint
hash(uint dev, uint blockno)
{
  return blockno % NBUCKETS;
}


void
binit(void)
{
  struct buf *b;
  char lockname[16];

  // 初始化全局锁
  initlock(&bcache.lock, "bcache");

  // 初始化每个桶的锁和链表
  for(int i = 0; i < NBUCKETS; i++) {
    snprintf(lockname, sizeof(lockname), "bcache%d", i);
    initlock(&bcache_buckets[i].lock, lockname);
    
    // 初始化桶的头节点
    bcache_buckets[i].head.prev = &bcache_buckets[i].head;
    bcache_buckets[i].head.next = &bcache_buckets[i].head;
  }

  // 初始化所有缓冲区
  for(b = bcache.buf; b < bcache.buf + NBUF; b++) {
    b->refcnt = 0;
    
    // 将所有缓冲区放入第一个桶中（后续会根据需要移动）
    b->next = bcache_buckets[0].head.next;
    b->prev = &bcache_buckets[0].head;
    initsleeplock(&b->lock, "buffer");
    bcache_buckets[0].head.next->prev = b;
    bcache_buckets[0].head.next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  uint bucket_idx = hash(dev, blockno);
  struct bucket *bucket = &bcache_buckets[bucket_idx];
  
  // 尝试在对应的哈希桶中查找块
  acquire(&bucket->lock);
  for(b = bucket->head.next; b != &bucket->head; b = b->next) {
    if(b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bucket->lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bucket->lock);

  // 如果没有找到，需要分配一个新的缓冲区
  // 获取全局锁，确保分配过程的原子性
  acquire(&bcache.lock);
  
  // 再次检查是否存在缓冲区（在获取全局锁期间可能已经被其他进程分配）
  acquire(&bucket->lock);
  for(b = bucket->head.next; b != &bucket->head; b = b->next) {
    if(b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bucket->lock);
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  
  // 仍然没有找到，尝试查找未使用的缓冲区
  struct buf *best_buf = 0;
  struct bucket *best_bucket = 0;
  int best_bucket_idx = 0;
  
  // 在所有桶中查找引用计数为0的缓冲区
  for(int i = 0; i < NBUCKETS; i++) {
    struct bucket *curr_bucket = &bcache_buckets[i];
    if(i != bucket_idx) {
      acquire(&curr_bucket->lock);
    }
    
    for(b = curr_bucket->head.next; b != &curr_bucket->head; b = b->next) {
      if(b->refcnt == 0) {
        best_buf = b;
        best_bucket = curr_bucket;
        best_bucket_idx = i;
        break;
      }
    }
    
    if(best_buf) {
      break;  // 找到了可用缓冲区，可以停止搜索
    }
    
    if(i != bucket_idx) {
      release(&curr_bucket->lock);
    }
  }
  
  if(best_buf == 0) {
    // 没有找到可用的缓冲区
    if(bucket_idx != best_bucket_idx) {
      release(&bucket->lock);
    }
    release(&bcache.lock);
    panic("bget: no buffers");
  }
  
  // 找到了可用的缓冲区
  if(best_bucket_idx != bucket_idx) {
    // 从旧桶中移除缓冲区
    best_buf->next->prev = best_buf->prev;
    best_buf->prev->next = best_buf->next;
    
    // 添加到新桶中
    best_buf->next = bucket->head.next;
    best_buf->prev = &bucket->head;
    bucket->head.next->prev = best_buf;
    bucket->head.next = best_buf;
    
    // 释放旧桶的锁
    release(&best_bucket->lock);
  }
  
  // 设置缓冲区信息
  best_buf->dev = dev;
  best_buf->blockno = blockno;
  best_buf->valid = 0;
  best_buf->refcnt = 1;
  
  release(&bucket->lock);
  release(&bcache.lock);
  
  acquiresleep(&best_buf->lock);
  return best_buf;
}

// Release a locked buffer.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  uint bucket_idx = hash(b->dev, b->blockno);
  struct bucket *bucket = &bcache_buckets[bucket_idx];
  
  acquire(&bucket->lock);
  b->refcnt--;
  release(&bucket->lock);
}

void
bpin(struct buf *b) {
  uint bucket_idx = hash(b->dev, b->blockno);
  struct bucket *bucket = &bcache_buckets[bucket_idx];
  
  acquire(&bucket->lock);
  b->refcnt++;
  release(&bucket->lock);
}

void
bunpin(struct buf *b) {
  uint bucket_idx = hash(b->dev, b->blockno);
  struct bucket *bucket = &bcache_buckets[bucket_idx];
  
  acquire(&bucket->lock);
  b->refcnt--;
  release(&bucket->lock);
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

