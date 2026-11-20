# rcwrite_a.c（普通 Write 版）逐句详细解释

本文档按照 `rc_write_not_immediate/rcwrite_a.c` 的执行顺序，逐句解释每个头文件、宏、变量、函数调用和语句的作用。建议与源码对照阅读。

---

## 一、头文件

```c
#include <stdio.h>
```

标准输入输出，提供 `printf`。

```c
#include <stdlib.h>
```

通用工具头文件，提供 `malloc`。

```c
#include <string.h>
```

提供 `memcpy`、`strlen`、`memset`。

```c
#include <stdint.h>
```

定义 `uint32_t`、`uint64_t` 等固定宽度整数。

```c
#include <stddef.h>
```

提供 `offsetof` 宏，用于计算结构体成员偏移。

```c
#include <unistd.h>
```

提供 `read`、`write`。

```c
#include <endian.h>
```

提供字节序转换函数。

```c
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
```

Socket 编程头文件。

```c
#include <infiniband/verbs.h>
```

IB Verbs 核心头文件。

---

## 二、宏定义

```c
#define TCP_PORT   12345
#define IB_PORT    1
#define GID_INDEX  3
```

与带 Immediate 版本相同：TCP 元数据端口、IB 物理端口、RoCE GID 索引。本机 `show_gids` 中 IP `192.168.0.xxx` 对应的 RoCEv2 GID 位于 INDEX 3，因此源码默认使用 3。如果你的环境该索引不同，请修改此宏。

---

## 三、数据结构

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

两端通过 TCP 交换的 QP 元数据。含义与带 Immediate 版本一致。

```c
struct msg_buf {
	volatile uint64_t flag;
	char data[256];
};
```

本次新增的数据缓冲区结构体。

- `flag`：门铃标志位。`volatile` 防止编译器优化掉轮询循环中的内存读取。发送方把对端该字段写成 `1`，接收方轮询到 `1` 就知道数据已到达。
- `data`：实际存放字符串的数组，大小 256 字节。

整个结构体通过 `malloc` 分配，注册成一个 MR。

---

## 四、主函数与 TCP 监听

```c
int main(int argc, char *argv[])
```

A 程序带一个可选的 verbs 设备索引参数，缺省为 `0`。在本机双口直连测试时，A 用 dev 0，B 用 dev 1。

```c
int dev_idx = (argc > 1) ? atoi(argv[1]) : 0;
```

```c
int ls = socket(AF_INET, SOCK_STREAM, 0);
int one = 1;
setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
struct sockaddr_in la = {
	.sin_family = AF_INET,
	.sin_port = htons(TCP_PORT),
	.sin_addr.s_addr = INADDR_ANY,
};
bind(ls, (struct sockaddr *)&la, sizeof(la));
listen(ls, 1);
int sock = accept(ls, NULL, NULL);
```

创建 TCP 监听套接字，设置 `SO_REUSEADDR` 以便重复测试时能快速重新绑定端口，然后绑定任意地址的 12345 端口，等待 B 连接。

---

## 五、IB Verbs 资源初始化

```c
struct ibv_device **devs = ibv_get_device_list(NULL);
struct ibv_context *ctx = ibv_open_device(devs[dev_idx]);
struct ibv_pd *pd = ibv_alloc_pd(ctx);
struct ibv_cq *cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
```

获取设备列表、打开由命令行参数指定的设备、分配 PD、创建 CQ。在本机双口直连测试中，A 和 B 必须分别打开不同的设备，RDMA 报文才能经网线从一个端口进入另一个端口。

```c
struct msg_buf *buf = malloc(sizeof(*buf));
memset(buf, 0, sizeof(*buf));
```

用 `malloc` 申请一个 `struct msg_buf`，并清零。清零很重要：初始 `flag` 必须是 0，接收方才能通过轮询 `flag != 0` 判断数据到达。

```c
struct ibv_mr *mr = ibv_reg_mr(pd, buf, sizeof(*buf),
		IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
```

把整个 `struct msg_buf` 注册为 MR，允许本端写和远端 RDMA Write。远端可以通过同一个 `rkey` 访问 `flag` 和 `data` 两个字段。

```c
struct ibv_qp_init_attr qpia = {
	.send_cq = cq,
	.recv_cq = cq,
	.cap = {
		.max_send_wr = 10,
		.max_recv_wr = 1,
		.max_send_sge = 1,
		.max_recv_sge = 1,
	},
	.qp_type = IBV_QPT_RC,
};
struct ibv_qp *qp = ibv_create_qp(pd, &qpia);
```

创建 RC QP。因为本版本只用普通 Write，不需要 post recv WR，但 `max_recv_wr` 仍保留 1。

---

## 六、准备并交换 QP 信息

```c
uint32_t psn = 0x12345678;
union ibv_gid my_gid;
ibv_query_gid(ctx, IB_PORT, GID_INDEX, &my_gid);
```

选定 PSN，查询本端 GID。

```c
struct info my = {
	.qp_num = htonl(qp->qp_num),
	.psn = htonl(psn),
	.rkey = htonl(mr->rkey),
	.lid = htons(0),
	.addr = htobe64((uint64_t)buf),
	.gid = my_gid,
};
struct info peer;
```

构造本地信息。注意 `addr` 是 `struct msg_buf *` 的地址，也就是整个缓冲区的基地址。对端会在这个基地址上加上 `offsetof(struct msg_buf, data)` 和 `offsetof(struct msg_buf, flag)` 来分别访问数据和门铃。

```c
int n = 0;
while (n < (int)sizeof(peer))
	n += read(sock, ((char *)&peer) + n, sizeof(peer) - n);
peer.qp_num = ntohl(peer.qp_num);
peer.psn = ntohl(peer.psn);
peer.rkey = ntohl(peer.rkey);
peer.lid = ntohs(peer.lid);
peer.addr = be64toh(peer.addr);

n = 0;
while (n < (int)sizeof(my))
	n += write(sock, ((char *)&my) + n, sizeof(my) - n);
```

A 先读取 B 的 `struct info`，再写回自己的。所有整数字段做字节序转换。

---

## 七、修改 QP 状态机

与带 Immediate 版本完全一致，三次 `ibv_modify_qp` 把 QP 从 `RESET` 改到 `INIT` → `RTR` → `RTS`。

关键点：

- `INIT` 时允许 `IBV_ACCESS_REMOTE_WRITE`。
- `RTR` 时填入对端 `qp_num`、`psn`、`gid`，配置 Address Handle。
- `RTS` 时启用本端发送。

---

## 八、预存待发送字符串

```c
char *msg = "Hello RDMA RC write world!";
int len = strlen(msg) + 1;
memcpy(buf->data, msg, len);
```

把要发送的字符串（含结尾 `\0`）复制到 `buf->data`。这个缓冲区已经注册成 MR，可以直接作为 RDMA Write 的源地址。

---

## 九、等待 B 就绪

```c
char ready;
n = 0;
while (n < 1)
	n += read(sock, &ready, 1);
```

A 从 TCP 读取 B 发来的 1 字节 ready 信号。B 在把 QP 改到 RTS 后、并且已经准备好轮询 `flag` 之后，才会发送这 1 字节。这样可以避免 A 过早写数据导致 B 还没开始轮询。

---

## 十、普通 RDMA Write 发送数据

```c
uint64_t data_off = offsetof(struct msg_buf, data);
uint64_t flag_off = offsetof(struct msg_buf, flag);
```

计算两个字段相对于 `struct msg_buf` 基地址的偏移。`data_off` 通常是 8，`flag_off` 是 0。使用 `offsetof` 让代码不依赖具体布局。

```c
struct ibv_sge dsge = {
	.addr = (uint64_t)buf->data,
	.length = len,
	.lkey = mr->lkey,
};
struct ibv_send_wr dwr = {
	.wr_id = 2,
	.opcode = IBV_WR_RDMA_WRITE,
	.send_flags = IBV_SEND_SIGNALED,
	.wr.rdma.remote_addr = peer.addr + data_off,
	.wr.rdma.rkey = peer.rkey,
	.sg_list = &dsge,
	.num_sge = 1,
};
ibv_post_send(qp, &dwr, &bad_send);
while (ibv_poll_cq(cq, 1, &wc) == 0) {}
```

构造并提交第一个普通 RDMA Write WR。

- `opcode = IBV_WR_RDMA_WRITE`：不带 Immediate。
- `wr.rdma.remote_addr = peer.addr + data_off`：目标地址是 B 缓冲区的 `data` 字段。
- `wr.rdma.rkey = peer.rkey`：B 缓冲区的 remote key。
- `send_flags = IBV_SEND_SIGNALED`：发送完成后在本端 CQ 产生完成事件。

提交后，A 轮询 CQ 等待该 Write 完成。

---

## 十一、普通 RDMA Write 发送门铃

```c
buf->flag = 1;
struct ibv_sge fsge = {
	.addr = (uint64_t)&buf->flag,
	.length = sizeof(uint64_t),
	.lkey = mr->lkey,
};
struct ibv_send_wr fwr = {
	.wr_id = 3,
	.opcode = IBV_WR_RDMA_WRITE,
	.send_flags = IBV_SEND_SIGNALED,
	.wr.rdma.remote_addr = peer.addr + flag_off,
	.wr.rdma.rkey = peer.rkey,
	.sg_list = &fsge,
	.num_sge = 1,
};
ibv_post_send(qp, &fwr, &bad_send);
while (ibv_poll_cq(cq, 1, &wc) == 0) {}
```

构造并提交第二个普通 RDMA Write WR，把 `1` 写到 B 缓冲区的 `flag` 字段。

注意这里用 `&buf->flag` 作为本地源地址。先把 `buf->flag` 设为 1，硬件从这个本地字段读取值，写到对端 `flag`。等发送完成后，再把本地 `buf->flag` 清 0，用于接下来等待 B 的回复。

由于两个 Write 按提交顺序在同一条 RC QP 上执行，B 一定先看到完整数据，再看到 `flag == 1`。

---

## 十二、等待并打印 B 的回复

```c
buf->flag = 0;
while (!buf->flag) {}
printf("received: %s\n", buf->data);
```

A 把本地 `flag` 清 0，然后轮询等待。B 收到 A 的消息后，会把 `"Hello!"` 写入 A 的 `data`，再把 A 的 `flag` 写成 1。A 检测到 `flag` 变为 1 后，读取并打印 `buf->data`。

这里的 `while (!buf->flag) {}` 就是“内存轮询门铃”。因为 `flag` 是 `volatile`，编译器每次都会重新从内存读取；在 x86 平台上，PCIe 写入与 CPU cache 是一致的，所以能看到对端 RDMA Write 的最新值。

---

## 十三、程序结束

```c
return 0;
```

主函数返回，资源由操作系统回收。

---

## 十四、整体流程回顾

1. A 监听 TCP 12345，接受 B 连接。
2. 两端初始化 IB 资源，`malloc` 并注册 `struct msg_buf`。
3. 通过 TCP 互换 QP 信息。
4. QP 改到 `INIT` → `RTR` → `RTS`。
5. A 把 `"Hello RDMA RC write world!"` 写入 `buf->data`。
6. B 通过 TCP 发送 ready 信号。
7. A 普通 Write 数据到 B 的 `data`。
8. A 普通 Write `flag = 1` 到 B 的 `flag`。
9. B 轮询到 `flag == 1`，打印消息。
10. B 把 `"Hello!"` 写入 `buf->data`。
11. B 普通 Write 数据到 A 的 `data`。
12. B 普通 Write `flag = 1` 到 A 的 `flag`。
13. A 轮询到 `flag == 1`，打印回复。

这就是 `rc_write_not_immediate/rcwrite_a.c` 的全部执行流程。
