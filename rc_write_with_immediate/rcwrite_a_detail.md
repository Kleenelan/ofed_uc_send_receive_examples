# rcwrite_a.c 逐句详细解释

本文档按照 `rcwrite_a.c` 的实际执行顺序，不厌其烦地解释每个头文件、宏、变量、函数调用和语句的作用。阅读前建议先打开 `rcwrite_a.c` 源码对照。

---

## 一、头文件

```c
#include <stdio.h>
```

`stdio.h` 提供标准输入输出，例如最后的 `printf`。

```c
#include <stdlib.h>
```

`stdlib.h` 提供 `atoi`、`malloc`、`free` 等通用工具函数。本程序虽然没有显式调用 `malloc`，但部分编译环境在解析命令行或转换数据时会依赖它。

```c
#include <string.h>
```

`string.h` 提供 `memcpy`、`strlen`、`memset` 等字符串和内存操作函数。后面把待发送字符串复制到 RDMA 缓冲区时使用了 `memcpy` 和 `strlen`。

```c
#include <stdint.h>
```

`stdint.h` 定义固定宽度的整数类型，例如 `uint32_t`、`uint64_t`。这些类型在 `struct info` 和 IB Verbs 结构体中大量使用，保证不同平台上字段大小一致。

```c
#include <unistd.h>
```

`unistd.h` 提供 `read`、`write`、`close` 等 POSIX 系统调用。TCP 连接建立后，两端用 `read`/`write` 交换元数据。

```c
#include <endian.h>
```

`endian.h` 提供 `htonl`、`htons`、`htobe64`、`ntohl`、`ntohs`、`be64toh` 等字节序转换函数。因为两台主机可能架构不同，TCP 上传输的整数需要先转成大端（网络字节序），接收后再转回主机字节序。

```c
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
```

这三个头文件一起提供 Berkeley Socket API。

- `sys/socket.h`：定义 `socket`、`bind`、`listen`、`accept`、`connect`、`sockaddr` 等。
- `netinet/in.h`：定义 `sockaddr_in`、`INADDR_ANY`、`htons`、`htonl` 等。
- `arpa/inet.h`：提供 `inet_addr` 等地址转换函数。B 程序会用到，A 程序没有直接调用，但通常一起包含。

```c
#include <infiniband/verbs.h>
```

这是整个 RDMA 程序的核心头文件，来自 OFED/libibverbs。它声明了所有 IB Verbs 数据结构（`ibv_context`、`ibv_pd`、`ibv_cq`、`ibv_qp`、`ibv_mr` 等）以及 API 函数（`ibv_get_device_list`、`ibv_open_device`、`ibv_create_qp`、`ibv_modify_qp`、`ibv_post_send`、`ibv_post_recv`、`ibv_poll_cq` 等）。

---

## 二、宏定义

```c
#define TCP_PORT   12345
```

A 和 B 用来交换 QP 元数据的 TCP 监听端口。两端必须一致。

```c
#define IB_PORT    1
```

使用 verbs 设备的第 1 个物理端口。对于 CX5 单口设备，通常就是 1；如果是双口卡被识别成两个 verbs 设备，每个设备也只有一个端口 1。

```c
#define GID_INDEX  3
```

RoCE 不使用 LID，而是使用 GID。`GID_INDEX` 表示从端口 GID 表中取第几个 GID。本机 `show_gids` 中 IP `192.168.0.xxx` 对应的 RoCEv2 GID 位于 INDEX 3，因此源码默认使用 3。如果你的环境该索引不同，请修改此宏。

---

## 三、QP 元数据结构体

```c
struct info {
	uint32_t qp_num;
	uint32_t psn;
	uint32_t rkey;
	uint16_t lid;
	uint64_t addr;
	union ibv_gid gid;
};
```

这是 A 和 B 在 TCP 上交换的数据包。

- `qp_num`：本端 RC QP 的编号。对端在把 QP 改到 `RTR` 状态时需要它。
- `psn`：本端发送队列初始 Packet Sequence Number。对端在配置 `RTR` 的 `rq_psn` 时需要它。
- `rkey`：本地 Memory Region 的 remote key。对端执行 RDMA Write 到本端内存时必须提供这个 key。
- `lid`：InfiniBand 的 Local ID。RoCE 不使用 LID，所以填 0。
- `addr`：本地接收缓冲区的虚拟地址。对端 Write 数据时把数据放到这个地址。
- `gid`：本地端口的 GID。RoCE 建立 Address Handle 时需要对端 GID。

整个结构体在 TCP 上一次按字节发送/接收。

---

## 四、主函数与 TCP 监听

```c
int main(int argc, char *argv[])
```

A 程序带一个可选的命令行参数：verbs 设备索引。缺省为 `0`（即 `mlx5_0`）。在本机双口直连测试时，可以让 A 用 dev 0，B 用 dev 1。

```c
int dev_idx = (argc > 1) ? atoi(argv[1]) : 0;
```

如果命令行给了参数，例如 `./rcwrite_a 0`，就打开 `devs[0]`；否则默认打开第一个设备。

```c
int ls = socket(AF_INET, SOCK_STREAM, 0);
int one = 1;
setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
```

创建一个 IPv4 流式（TCP）套接字。`AF_INET` 表示 IPv4，`SOCK_STREAM` 表示面向连接的字节流，`0` 让系统选择默认协议（TCP）。返回值 `ls` 是监听套接字描述符。

紧接着设置 `SO_REUSEADDR`，允许进程快速重新绑定仍处于 `TIME_WAIT` 状态的端口。示例程序不做错误检查，但在反复启动测试时这个选项能避免端口被占用导致无法监听。

```c
struct sockaddr_in la = {
	.sin_family = AF_INET,
	.sin_port = htons(TCP_PORT),
	.sin_addr.s_addr = INADDR_ANY,
};
```

构造本端地址结构。

- `sin_family = AF_INET`：地址族为 IPv4。
- `sin_port = htons(TCP_PORT)`：端口号转成大端字节序。宏 `TCP_PORT` 是 12345。
- `sin_addr.s_addr = INADDR_ANY`：绑定到所有本地 IPv4 地址，这样无论从哪个网卡连接都能接受。

```c
bind(ls, (struct sockaddr *)&la, sizeof(la));
```

把监听套接字 `ls` 和本地地址 `la` 绑定。之后该套接字就在 12345 端口上接收连接请求。

```c
listen(ls, 1);
```

把套接字设为监听状态，允许最多 1 个未处理连接排队。

```c
int sock = accept(ls, NULL, NULL);
```

阻塞等待 B 的连接。连接建立后返回新的已连接套接字 `sock`，后续的 QP 信息交换都通过 `sock` 进行。

---

## 五、IB Verbs 资源初始化

```c
struct ibv_device **devs = ibv_get_device_list(NULL);
```

获取本机所有 IB/RoCE 设备的列表。`devs` 是一个以 `NULL` 结尾的指针数组。`NULL` 参数表示不需要返回设备数量。因为程序最小化，没有调用 `ibv_free_device_list`；进程退出时由操作系统回收。

```c
struct ibv_context *ctx = ibv_open_device(devs[dev_idx]);
```

打开由命令行参数（或默认值 0）指定的设备，返回一个 `ibv_context`。后续所有 verbs 操作都基于这个上下文。在本机双口直连测试中，A 和 B 必须分别打开不同的设备（例如 `mlx5_0` 和 `mlx5_1`），RDMA 报文才能经网线从一个端口进入另一个端口。

```c
struct ibv_pd *pd = ibv_alloc_pd(ctx);
```

分配 Protection Domain。PD 是 IB 资源的容器，MR、QP 等都必须属于某个 PD。可以把 PD 理解为“权限域”：只有同一 PD 内的资源才能互相引用。

```c
struct ibv_cq *cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
```

创建 Completion Queue。

- `ctx`：所属上下文。
- `16`：CQ 能容纳的完成条目数。一次往返只需要几个条目，16 足够。
- 接下来的两个 `NULL` 表示不使用完成事件通道和完成事件上下文。
- 最后的 `0` 表示没有特殊标志。

A 和 B 都复用同一个 CQ 处理 send 完成和 recv 完成。

```c
char buf[256];
```

申请 256 字节的本地缓冲区。它有两个用途：

1. A 先把要发送的字符串放进 `buf`，再由 RDMA Write 传到 B。
2. A 也把自己的 `buf` 注册成 MR，供 B 回复时写入。

```c
struct ibv_mr *mr = ibv_reg_mr(pd, buf, sizeof(buf),
		IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
```

把 `buf` 注册成 Memory Region。

- `pd`：所属的 Protection Domain。
- `buf`/`sizeof(buf)`：起始地址和长度。
- 访问权限：
  - `IBV_ACCESS_LOCAL_WRITE`：本端发送/接收时可以写这个缓冲区。
  - `IBV_ACCESS_REMOTE_WRITE`：允许对端通过 RDMA Write 写入这个缓冲区。

注册成功后，`mr->lkey` 给本端 post send/recv 使用，`mr->rkey` 给对端做 RDMA Write 使用。

---

## 六、创建 RC QP

```c
struct ibv_qp_init_attr qpia = {
	.send_cq = cq,
	.recv_cq = cq,
	.cap = {
		.max_send_wr = 10,
		.max_recv_wr = 10,
		.max_send_sge = 1,
		.max_recv_sge = 1,
	},
	.qp_type = IBV_QPT_RC,
};
```

初始化 QP 创建属性。

- `send_cq` / `recv_cq`：发送和接收完成都放到同一个 CQ。
- `cap.max_send_wr`：发送队列最多挂 10 个 WR。
- `cap.max_recv_wr`：接收队列最多挂 10 个 WR。
- `cap.max_send_sge` / `max_recv_sge`：每个 WR 最多 1 个 SGE。
- `qp_type = IBV_QPT_RC`：Reliable Connected，可靠连接类型。

```c
struct ibv_qp *qp = ibv_create_qp(pd, &qpia);
```

在 PD 中创建一个 RC QP。创建后 QP 处于 `RESET` 状态，还不能收发数据；后面要通过 `ibv_modify_qp` 逐步改到 `INIT`、`RTR`、`RTS`。

---

## 七、准备本地 QP 信息

```c
uint32_t psn = 0x12345678;
```

指定本端发送队列的起始 PSN。只要两端互相知道对方的 PSN，具体数值并不重要，A 和 B 用了不同值以示区分。

```c
union ibv_gid my_gid;
ibv_query_gid(ctx, IB_PORT, GID_INDEX, &my_gid);
```

查询本端端口在 `GID_INDEX` 位置的 GID。RoCE 建链需要这个值来填充对端 Address Handle 的 `dgid`。

```c
struct info my = {
	.qp_num = htonl(qp->qp_num),
	.psn = htonl(psn),
	.rkey = htonl(mr->rkey),
	.lid = htons(0),
	.addr = htobe64((uint64_t)buf),
	.gid = my_gid,
};
```

构造要发给对端的信息。

- `qp->qp_num`、`psn`、`mr->rkey` 都是 32 位，先用 `htonl` 转成大端。
- `lid` 是 16 位，RoCE 不用，填 0 并 `htons`。
- `addr` 是 64 位缓冲区地址，用 `htobe64` 转成大端。
- `gid` 本身已经是网络字节序的字节数组，直接复制。

```c
struct info peer;
```

声明一个结构体，用来存放对端发来的信息。

---

## 八、通过 TCP 交换 QP 信息

```c
int n = 0;
while (n < (int)sizeof(peer))
	n += read(sock, ((char *)&peer) + n, sizeof(peer) - n);
```

A 先读取 B 发过来的 `struct info`。`read` 可能一次读不完，所以用循环把 `sizeof(peer)` 字节全部读满。每读一次，指针向后移动 `n` 字节，剩余长度减少。

```c
peer.qp_num = ntohl(peer.qp_num);
peer.psn = ntohl(peer.psn);
peer.rkey = ntohl(peer.rkey);
peer.lid = ntohs(peer.lid);
peer.addr = be64toh(peer.addr);
```

把对端字段从网络字节序转回主机字节序，这样后面的代码才能正确使用。

```c
n = 0;
while (n < (int)sizeof(my))
	n += write(sock, ((char *)&my) + n, sizeof(my) - n);
```

A 再把本地的 `struct info` 完整地写给 B。顺序是“先读后写”，B 程序则是“先写后读”，避免死锁。

---

## 九、修改 QP 状态机

### 9.1 改到 INIT

```c
struct ibv_qp_attr attr = {0};
```

声明 QP 属性结构体并清零。

```c
attr.qp_state = IBV_QPS_INIT;
attr.pkey_index = 0;
attr.port_num = IB_PORT;
attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE;
```

设置 INIT 状态参数：

- `qp_state`：目标状态 `INIT`。
- `pkey_index`：Partition Key 索引，默认 0。
- `port_num`：使用的物理端口。
- `qp_access_flags`：允许对端对本端做 RDMA Write。

```c
ibv_modify_qp(qp, &attr,
	IBV_QP_STATE | IBV_QP_PKEY_INDEX |
	IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
```

调用 `ibv_modify_qp` 把 QP 从 `RESET` 改到 `INIT`。后面的位掩码告诉驱动哪些字段有效。

### 9.2 改到 RTR（Ready To Receive）

```c
memset(&attr, 0, sizeof(attr));
attr.qp_state = IBV_QPS_RTR;
attr.path_mtu = IBV_MTU_1024;
attr.dest_qp_num = peer.qp_num;
attr.rq_psn = peer.psn;
attr.max_dest_rd_atomic = 1;
attr.min_rnr_timer = 12;
```

- `qp_state = RTR`：准备接收。
- `path_mtu = IBV_MTU_1024`：路径 MTU，和端口实际 MTU 匹配即可。
- `dest_qp_num`：对端 QP 号。
- `rq_psn`：对端发送队列的初始 PSN，本端接收队列从该序号开始期望包。
- `max_dest_rd_atomic`：允许对端同时发起的原子操作数，这里写 1。
- `min_rnr_timer`：RNR NAK 定时器参数。

```c
attr.ah_attr.is_global = 1;
attr.ah_attr.grh.dgid = peer.gid;
attr.ah_attr.grh.sgid_index = GID_INDEX;
attr.ah_attr.grh.hop_limit = 1;
attr.ah_attr.dlid = 0;
attr.ah_attr.sl = 0;
attr.ah_attr.port_num = IB_PORT;
```

配置 Address Handle。对于 RoCE：

- `is_global = 1`：使用全局路由头 GRH。
- `grh.dgid`：目的 GID，填对端 GID。
- `grh.sgid_index`：本端使用的 GID 索引。
- `grh.hop_limit`：跳数限制，直连写 1 即可。
- `dlid = 0`：RoCE 不使用 LID。
- `sl = 0`：服务等级。
- `port_num`：本端出口物理端口。

```c
ibv_modify_qp(qp, &attr,
	IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
	IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
	IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
```

把 QP 改到 `RTR`。完成后，本端 QP 已经能够接收对端发来的包。

### 9.3 改到 RTS（Ready To Send）

```c
memset(&attr, 0, sizeof(attr));
attr.qp_state = IBV_QPS_RTS;
attr.timeout = 14;
attr.retry_cnt = 7;
attr.rnr_retry = 7;
attr.sq_psn = psn;
attr.max_rd_atomic = 1;
```

- `qp_state = RTS`：准备发送。
- `timeout`：ACK 超时参数。
- `retry_cnt`：重传次数。
- `rnr_retry`：RNR 重试次数。
- `sq_psn`：本端发送队列起始 PSN。
- `max_rd_atomic`：本端同时发起的 RDMA Read/Atomic 数。

```c
ibv_modify_qp(qp, &attr,
	IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
	IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
```

把 QP 改到 `RTS`。现在两端 QP 都已经可以收发。

---

## 十、Post Receive 等待 B 的回复

```c
struct ibv_sge rsge = {
	.addr = (uint64_t)buf,
	.length = sizeof(buf),
	.lkey = mr->lkey,
};
```

构造接收 Scatter/Gather Element。

- `addr`：接收缓冲区地址。
- `length`：缓冲区长度 256。
- `lkey`：本端 MR 的 local key。

```c
struct ibv_recv_wr rwr = {
	.wr_id = 1,
	.sg_list = &rsge,
	.num_sge = 1,
};
```

构造接收 WR。

- `wr_id`：用户自定义标识，完成时可以通过它区分是哪一笔 WR。
- `sg_list`：指向 SGE 数组。
- `num_sge`：SGE 数量。

```c
struct ibv_recv_wr *bad_recv;
ibv_post_recv(qp, &rwr, &bad_recv);
```

把接收 WR 挂到 QP 的接收队列。因为后面 B 会发一个带 Immediate 的 RDMA Write，这个 Write 会消耗一个 recv WR 并产生 recv CQE。如果没有预先 post recv，带 Immediate 的 Write 会因为没有可用接收 WQE 而触发异步错误。

---

## 十一、等待 B 就绪

```c
char ready;
n = 0;
while (n < 1)
	n += read(sock, &ready, 1);
```

A 从 TCP 连接读取 1 字节。B 在 post 完 recv 后会发送这 1 字节，表示“我已经准备好接收你的 RDMA Write”。A 收到后才发数据，避免 B 的 recv WR 还没挂好就收到 Write。

---

## 十二、准备并发送 RDMA Write With Immediate

```c
char *msg = "Hello RDMA RC write world!";
int len = strlen(msg) + 1;
memcpy(buf, msg, len);
```

把要发送的字符串（包括结尾的 `\0`）复制到 `buf`。RDMA Write 的源数据就来自这个本地缓冲区。

```c
struct ibv_sge ssge = {
	.addr = (uint64_t)buf,
	.length = len,
	.lkey = mr->lkey,
};
```

构造发送 SGE，描述本地源数据。

```c
struct ibv_send_wr swr = {
	.wr_id = 2,
	.opcode = IBV_WR_RDMA_WRITE_WITH_IMM,
	.send_flags = IBV_SEND_SIGNALED,
	.imm_data = htonl(len),
	.wr.rdma.remote_addr = peer.addr,
	.wr.rdma.rkey = peer.rkey,
	.sg_list = &ssge,
	.num_sge = 1,
};
```

构造发送 WR。

- `wr_id = 2`：发送完成时可通过它识别。
- `opcode = IBV_WR_RDMA_WRITE_WITH_IMM`：带立即数的 RDMA Write。
- `send_flags = IBV_SEND_SIGNALED`：要求硬件在发送完成后产生 CQE。如果不置位，A 就无法知道 Write 是否完成。
- `imm_data = htonl(len)`：立即数，这里放字符串长度。对端收到后可以在 CQE 中读到它。
- `wr.rdma.remote_addr`：对端缓冲区的虚拟地址。
- `wr.rdma.rkey`：对端 MR 的 remote key。
- `sg_list` / `num_sge`：本地源数据 SGE。

```c
struct ibv_send_wr *bad_send;
ibv_post_send(qp, &swr, &bad_send);
```

把发送 WR 提交到 QP 的发送队列。硬件会负责把 `buf` 中的数据写到 B 的内存，并在 B 的 recv CQ 上产生一个完成事件。

---

## 十三、轮询完成队列

```c
struct ibv_wc wc;
while (ibv_poll_cq(cq, 1, &wc) == 0) {}
```

轮询 CQ，等待 A 自己发送的 Write 完成。`ibv_poll_cq` 返回本次取到的完成条目数；返回 0 表示还没有完成，所以循环空转。第一笔完成是发送 WR 的完成。

```c
while (ibv_poll_cq(cq, 1, &wc) == 0) {}
printf("received: %s\n", buf);
```

继续轮询，等待第二笔完成。第二笔完成来自 B 的 RDMA Write With Immediate：B 把 `"Hello!"` 写到了 A 的 `buf`，同时产生 recv CQE。收到后，`buf` 里已经是回复内容，调用 `printf` 打印。

---

## 十四、程序结束

```c
return 0;
```

主函数返回。程序退出时，套接字、IB 资源由操作系统回收。由于这是示例程序，没有显式调用 `ibv_destroy_qp`、`ibv_dereg_mr`、`ibv_destroy_cq` 等清理函数。

---

## 十五、整体流程回顾

1. A 在 TCP 12345 端口监听。
2. B 连接上来。
3. 两端分别初始化 IB 资源，创建 RC QP。
4. 通过 TCP 互换 QP 号、PSN、GID、rkey、缓冲区地址。
5. 两端把 QP 改到 `INIT` → `RTR` → `RTS`。
6. A 先 post recv，等待 B 的 ready 信号。
7. B post recv，向 A 发送 ready 信号。
8. A 收到 ready，把字符串写入 B 的缓冲区（RDMA Write With Immediate）。
9. B poll 到 recv 完成，打印消息，再把回复写入 A 的缓冲区。
10. A poll 到 send 完成和 recv 完成，打印回复。

这就是 `rcwrite_a.c` 的全部执行流程。
