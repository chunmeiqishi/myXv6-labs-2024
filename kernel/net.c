#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "net.h"

// xv6's ethernet and IP addresses
static uint8 local_mac[ETHADDR_LEN] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static uint32 local_ip = MAKE_IP_ADDR(10, 0, 2, 15);

// qemu host's ethernet address.
static uint8 host_mac[ETHADDR_LEN] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };

static struct spinlock netlock;


//新增数据结构
#define NPORT 16         // 最大绑定端口数
#define NPKT 16          // 每个端口最大缓存包数

// 用于保存 UDP 数据包的结构
struct udp_pkt {
  char *buf;             // 数据包内容
  int len;               // 数据包长度
  uint32 src;            // 源 IP 地址
  uint16 sport;          // 源端口
};

// 绑定端口的结构
struct port_info {
  int used;              // 端口是否被绑定
  uint16 port;           // 绑定的端口号
  struct spinlock lock;  // 队列锁
  struct udp_pkt pkts[NPKT]; // 数据包队列
  int head;              // 队列头（取数据）
  int tail;              // 队列尾（放数据）
  int count;             // 当前队列中的数据包数量
  struct sleeplock sleeplock; // 睡眠锁，用于等待数据包
};

// 绑定端口的数组
static struct port_info ports[NPORT];

void
netinit(void)
{
  initlock(&netlock, "netlock");

   // 初始化所有端口信息
  for(int i = 0; i < NPORT; i++) {
    ports[i].used = 0;
    initlock(&ports[i].lock, "port_lock");
    initsleeplock(&ports[i].sleeplock, "port_sleeplock");
  }
}


//
// bind(int port)
// prepare to receive UDP packets address to the port,
// i.e. allocate any queues &c needed.
//
uint64
sys_bind(void)
{
  //
  // Your code here.
  //

 int port;
  
  // 获取用户传递的端口参数
  argint(0, &port);
  
  // 检查端口是否有效
  if(port < 0 || port > 65535) {
    return -1;
  }
  
  acquire(&netlock);
  
  // 查找可用的端口槽位
  int i;
  for(i = 0; i < NPORT; i++) {
    if(ports[i].used && ports[i].port == port) {
      // 端口已经被绑定
      release(&netlock);
      return -1;
    }
  }
  
  // 查找空闲的槽位
  for(i = 0; i < NPORT; i++) {
    if(!ports[i].used) {
      ports[i].used = 1;
      ports[i].port = port;
      ports[i].head = 0;
      ports[i].tail = 0;
      ports[i].count = 0;
      release(&netlock);
      return 0;
    }
  }
  
  release(&netlock);
  return -1;  // 没有可用槽位
}

//
// unbind(int port)
// release any resources previously created by bind(port);
// from now on UDP packets addressed to port should be dropped.
//
uint64
sys_unbind(void)
{
  //
  // Optional: Your code here.
  //

  return 0;
}

//
// recv(int dport, int *src, short *sport, char *buf, int maxlen)
// if there's a received UDP packet already queued that was
// addressed to dport, then return it.
// otherwise wait for such a packet.
//
// sets *src to the IP source address.
// sets *sport to the UDP source port.
// copies up to maxlen bytes of UDP payload to buf.
// returns the number of bytes copied,
// and -1 if there was an error.
//
// dport, *src, and *sport are host byte order.
// bind(dport) must previously have been called.
//
uint64
sys_recv(void)
{
  //
  // Your code here.
  //

  struct proc *p = myproc();
  int dport;
  uint64 src_addr, sport_addr, buf_addr;
  int maxlen;

  // 获取用户传递的参数
  argint(0, &dport);
  argaddr(1, &src_addr);
  argaddr(2, &sport_addr);
  argaddr(3, &buf_addr);
  argint(4, &maxlen);

  // 查找绑定的端口
  int i;
  int found = 0;
  acquire(&netlock);
  for(i = 0; i < NPORT; i++) {
    if(ports[i].used && ports[i].port == dport) {
      found = 1;
      break;
    }
  }
  release(&netlock);

  if(!found)
    return -1;  // 端口未绑定

  acquire(&ports[i].lock);
  
  // 如果没有等待的数据包，等待
  while(ports[i].count == 0) {
    // 释放锁并休眠，等待数据包到达
    sleep(&ports[i], &ports[i].lock);
    
    // 醒来后检查端口是否仍然有效
    if(!ports[i].used || ports[i].port != dport) {
      release(&ports[i].lock);
      return -1;
    }
  }

  // 获取队列头部的数据包
  struct udp_pkt *pkt = &ports[i].pkts[ports[i].head];
  
  // 复制源 IP 地址到用户空间
  if(copyout(p->pagetable, src_addr, (char *)&pkt->src, sizeof(pkt->src)) < 0) {
    release(&ports[i].lock);
    return -1;
  }
  
  // 复制源端口到用户空间
  if(copyout(p->pagetable, sport_addr, (char *)&pkt->sport, sizeof(pkt->sport)) < 0) {
    release(&ports[i].lock);
    return -1;
  }
  
  // 计算要复制的字节数
  int copy_len = pkt->len;
  if(copy_len > maxlen)
    copy_len = maxlen;
  
  // 复制数据包内容到用户空间
  if(copyout(p->pagetable, buf_addr, pkt->buf, copy_len) < 0) {
    release(&ports[i].lock);
    return -1;
  }
  
  // 记录实际复制的字节数
  int result = copy_len;
  
  // 释放数据包缓冲区
  kfree(pkt->buf);
  
  // 更新队列
  ports[i].head = (ports[i].head + 1) % NPKT;
  ports[i].count--;
  
  release(&ports[i].lock);
  return result;
}

// This code is lifted from FreeBSD's ping.c, and is copyright by the Regents
// of the University of California.
static unsigned short
in_cksum(const unsigned char *addr, int len)
{
  int nleft = len;
  const unsigned short *w = (const unsigned short *)addr;
  unsigned int sum = 0;
  unsigned short answer = 0;

  /*
   * Our algorithm is simple, using a 32 bit accumulator (sum), we add
   * sequential 16 bit words to it, and at the end, fold back all the
   * carry bits from the top 16 bits into the lower 16 bits.
   */
  while (nleft > 1)  {
    sum += *w++;
    nleft -= 2;
  }

  /* mop up an odd byte, if necessary */
  if (nleft == 1) {
    *(unsigned char *)(&answer) = *(const unsigned char *)w;
    sum += answer;
  }

  /* add back carry outs from top 16 bits to low 16 bits */
  sum = (sum & 0xffff) + (sum >> 16);
  sum += (sum >> 16);
  /* guaranteed now that the lower 16 bits of sum are correct */

  answer = ~sum; /* truncate to 16 bits */
  return answer;
}

//
// send(int sport, int dst, int dport, char *buf, int len)
//
uint64
sys_send(void)
{
  struct proc *p = myproc();
  int sport;
  int dst;
  int dport;
  uint64 bufaddr;
  int len;

  argint(0, &sport);
  argint(1, &dst);
  argint(2, &dport);
  argaddr(3, &bufaddr);
  argint(4, &len);

  int total = len + sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp);
  if(total > PGSIZE)
    return -1;

  char *buf = kalloc();
  if(buf == 0){
    printf("sys_send: kalloc failed\n");
    return -1;
  }
  memset(buf, 0, PGSIZE);

  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, host_mac, ETHADDR_LEN);
  memmove(eth->shost, local_mac, ETHADDR_LEN);
  eth->type = htons(ETHTYPE_IP);

  struct ip *ip = (struct ip *)(eth + 1);
  ip->ip_vhl = 0x45; // version 4, header length 4*5
  ip->ip_tos = 0;
  ip->ip_len = htons(sizeof(struct ip) + sizeof(struct udp) + len);
  ip->ip_id = 0;
  ip->ip_off = 0;
  ip->ip_ttl = 100;
  ip->ip_p = IPPROTO_UDP;
  ip->ip_src = htonl(local_ip);
  ip->ip_dst = htonl(dst);
  ip->ip_sum = in_cksum((unsigned char *)ip, sizeof(*ip));

  struct udp *udp = (struct udp *)(ip + 1);
  udp->sport = htons(sport);
  udp->dport = htons(dport);
  udp->ulen = htons(len + sizeof(struct udp));

  char *payload = (char *)(udp + 1);
  if(copyin(p->pagetable, payload, bufaddr, len) < 0){
    kfree(buf);
    printf("send: copyin failed\n");
    return -1;
  }

  e1000_transmit(buf, total);

  return 0;
}

void
ip_rx(char *buf, int len)
{
  // don't delete this printf; make grade depends on it.
  static int seen_ip = 0;
  if(seen_ip == 0)
    printf("ip_rx: received an IP packet\n");
  seen_ip = 1;

  //
  // Your code here.
  //
   // 检查数据包长度是否足够
  if(len < sizeof(struct eth) + sizeof(struct ip))
    goto drop;

  struct eth *eth = (struct eth *)buf;
  struct ip *ip = (struct ip *)(eth + 1);

  // 验证这是一个 UDP 数据包
  if(ip->ip_p != IPPROTO_UDP)
    goto drop;

  // 确保数据包长度足够包含 UDP 头
  if(len < sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp))
    goto drop;

  struct udp *udp = (struct udp *)(ip + 1);
  uint16 dport = ntohs(udp->dport);
  uint16 sport = ntohs(udp->sport);
  uint16 ulen = ntohs(udp->ulen);
  
  // 计算 UDP 负载长度
  int payload_len = ulen - sizeof(struct udp);
  if(payload_len < 0)
    goto drop;

  // 数据包负载指针
  char *payload = (char *)(udp + 1);

  // 查找绑定的端口
  int i;
  int found = 0;
  acquire(&netlock);
  for(i = 0; i < NPORT; i++) {
    if(ports[i].used && ports[i].port == dport) {
      found = 1;
      break;
    }
  }
  release(&netlock);

  if(!found)
    goto drop;  // 没有绑定的进程

  // 获取端口锁
  acquire(&ports[i].lock);

  // 检查队列是否已满
  if(ports[i].count >= NPKT) {
    release(&ports[i].lock);
    goto drop;  // 队列已满，丢弃数据包
  }

  // 分配一个新缓冲区来保存数据包
  char *pktbuf = kalloc();
  if(pktbuf == 0) {
    release(&ports[i].lock);
    goto drop;  // 内存不足
  }

  // 复制数据包内容
  memmove(pktbuf, payload, payload_len);

  // 添加到队列
  ports[i].pkts[ports[i].tail].buf = pktbuf;
  ports[i].pkts[ports[i].tail].len = payload_len;
  ports[i].pkts[ports[i].tail].src = ntohl(ip->ip_src);
  ports[i].pkts[ports[i].tail].sport = sport;
  
  ports[i].tail = (ports[i].tail + 1) % NPKT;
  ports[i].count++;

  // 唤醒等待此端口的进程
  wakeup(&ports[i]);
  
  release(&ports[i].lock);
  kfree(buf);  // 释放原始数据包
  return;

drop:
  // 丢弃数据包
  kfree(buf);

}

//
// send an ARP reply packet to tell qemu to map
// xv6's ip address to its ethernet address.
// this is the bare minimum needed to persuade
// qemu to send IP packets to xv6; the real ARP
// protocol is more complex.
//
void
arp_rx(char *inbuf)
{
  static int seen_arp = 0;

  if(seen_arp){
    kfree(inbuf);
    return;
  }
  printf("arp_rx: received an ARP packet\n");
  seen_arp = 1;

  struct eth *ineth = (struct eth *) inbuf;
  struct arp *inarp = (struct arp *) (ineth + 1);

  char *buf = kalloc();
  if(buf == 0)
    panic("send_arp_reply");
  
  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, ineth->shost, ETHADDR_LEN); // ethernet destination = query source
  memmove(eth->shost, local_mac, ETHADDR_LEN); // ethernet source = xv6's ethernet address
  eth->type = htons(ETHTYPE_ARP);

  struct arp *arp = (struct arp *)(eth + 1);
  arp->hrd = htons(ARP_HRD_ETHER);
  arp->pro = htons(ETHTYPE_IP);
  arp->hln = ETHADDR_LEN;
  arp->pln = sizeof(uint32);
  arp->op = htons(ARP_OP_REPLY);

  memmove(arp->sha, local_mac, ETHADDR_LEN);
  arp->sip = htonl(local_ip);
  memmove(arp->tha, ineth->shost, ETHADDR_LEN);
  arp->tip = inarp->sip;

  e1000_transmit(buf, sizeof(*eth) + sizeof(*arp));

  kfree(inbuf);
}

void
net_rx(char *buf, int len)
{
  struct eth *eth = (struct eth *) buf;

  if(len >= sizeof(struct eth) + sizeof(struct arp) &&
     ntohs(eth->type) == ETHTYPE_ARP){
    arp_rx(buf);
  } else if(len >= sizeof(struct eth) + sizeof(struct ip) &&
     ntohs(eth->type) == ETHTYPE_IP){
    ip_rx(buf, len);
  } else {
    kfree(buf);
  }
}
