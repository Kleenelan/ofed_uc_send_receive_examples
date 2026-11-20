# rcwrite_b.c 逐句详细解释

本文档按照 `rcwrite_b.c` 的实际执行顺序，逐句解释每个头文件、宏、变量、函数调用和语句的作用。建议与 `rcwrite_b.c` 源码对照阅读。

---

## 一、头文件

```c
#include <stdio.h>
```

标准输入输出头文件，提供 `printf`。

```c
#include <stdlib.h>
```

通用工具头文件，包含 `atoi` 等。虽然 B 没有显式调用 `atoi`，但解析命令行参数时依赖该头文件中的类型声明。

```c
#include <string.h>
```

提供 `memcpy`、`strlen`、`memset` 等内存/字符串操作。B 在收到 A 的消息后，用 `memcpy` 把回复字符串写入缓冲区。

```c
#include <stdint.h>
```

定义 `uint32_t`、`uint64_t` 等固定宽度整数类型。`struct info` 和 IB Verbs 结构体都依赖这些类型。

```c
#include <unistd.h>
```

提供 `read`、`write` 等 POSIX 系统调用。B 通过 TCP 与 A 交换 QP 元数据。

```c
#include <endian.h>
```

提供 `htonl`、`htons`、`htobe64`、`ntohl`、`ntohs`、`be64toh` 等字节序转换函数。通过 TCP 交换的整数必须先转成大端，接收后再转回主机字节序。

```c
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
```

Socket 编程三件套：

- `sys/socket.h`：`socket`、`connect`、`bind`、`listen`、`accept`、`sockaddr`。
- `netinet/in.h`：`sockaddr_in`、`INADDR_ANY`、`htons`。
- `arpa/inet.h`：`inet_addr`，用于把点分十进制 IP 字符串转成网络字节序整数。

```c
#include <infiniband/verbs.h>
```

IB Verbs 头文件，提供 RDMA 所需的所有数据结构和 API。

---

## 二、宏定义

```c
#define TCP_PORT   12345
```

与 A 约定好的 TCP 元数据交换端口。

```c
#define IB_PORT    1
```

使用 verbs 设备的第 1 个物理端口。

```c
#define GID_INDEX  3
```

RoCE 使用的 GID 索引。本机 `show_gids` 中 IP `192.168.0.xxx` 对应的 RoCEv2 GID 位于 INDEX 3，因此源码默认使用 3。如果你的环境该索引不同，请修改此宏。

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

与 A 完全一致的元数据结构体，用于在 TCP 上交换 QP 号、PSN、rkey、缓冲区地址和 GID。具体每个字段的含义已经在 `rcwrite_a_detail.md` 中解释过，这里不再重复。

---

## 四、主函数与 TCP 连接

```c
int main(int argc, char *argv[])
```

B 需要知道 A 的 IP 地址，并且带一个可选的 verbs 设备索引参数。运行方式示例：

```bash
./rcwrite_b 192.168.0.100
```

其中 `argv[1]` 就是 A 的 IP，缺省打开 dev 0。在本机双口直连测试时，需要让 B 打开另一个设备：

```bash
./rcwrite_b 192.168.0.100 1
```

```c
int dev_idx = (argc > 2) ? atoi(argv[2]) : 0;
```

如果命令行给了第三个参数，就用它作为设备索引；否则默认 `0`。

```c
int sock = socket(AF_INET, SOCK_STREAM, 0);
```

创建 IPv4 TCP 套接字，返回值 `sock` 用于后续读写。

```c
struct sockaddr_in ra = {
	.sin_family = AF_INET,
	.sin_port = htons(TCP_PORT),
	.sin_addr.s_addr = inet_addr(argv[1]),
};
```

构造对端地址结构。

- `sin_family = AF_INET`：IPv4。
- `sin_port = htons(TCP_PORT)`：A 监听的端口，转成大端。
- `sin_addr.s_addr = inet_addr(argv[1])`：把命令行传入的点分十进制字符串转成 32 位网络字节序整数。

```c
connect(sock, (struct sockaddr *)&ra, sizeof(ra));
```

主动连接 A。`connect` 成功返回后，TCP 连接建立，B 就可以和 A 交换 QP 元数据了。

---

## 五、IB Verbs 资源初始化

```c
struct ibv_device **devs = ibv_get_device_list(NULL);
```

获取本机 IB/RoCE 设备列表。

```c
struct ibv_context *ctx = ibv_open_device(devs[dev_idx]);
```

打开由命令行参数（或默认值 0）指定的设备，得到 verbs 上下文。在本机双口直连测试中，B 通常要打开 `devs[1]`（`mlx5_1`），而 A 打开 `devs[0]`（`mlx5_0`），这样 RDMA 报文才能经网线从一个端口进入另一个端口。

```c
struct ibv_pd *pd = ibv_alloc_pd(ctx);
```

分配 Protection Domain。

```c
struct ibv_cq *cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
```

创建大小为 16 的 Completion Queue，send 和 recv 完成都放到这里。

```c
char buf[256];
struct ibv_mr *mr = ibv_reg_mr(pd, buf, sizeof(buf),
		IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
```

- 申请 256 字节缓冲区。
- 注册 MR，允许本端写和远端 RDMA Write。

B 的 `buf` 先被 A 写入 Hello 消息，随后 B 把它覆盖为回复消息，再由 A 通过 RDMA Write 读回。

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
struct ibv_qp *qp = ibv_create_qp(pd, &qpia);
```

创建 RC QP。`cap` 表示队列容量，这里 send/recv 各 10 个 WR，每个 WR 1 个 SGE。

```c
uint32_t psn = 0x87654321;
union ibv_gid my_gid;
ibv_query_gid(ctx, IB_PORT, GID_INDEX, &my_gid);
```

设置本端起始 PSN，并查询端口 GID。PSN 值只要被对端知道即可，B 选了一个与 A 不同的数值。

---

## 六、通过 TCP 交换 QP 信息

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

构造本地信息并声明对端信息结构体。所有整数字段都转成大端，方便跨平台传输。

```c
int n = 0;
while (n < (int)sizeof(my))
	n += write(sock, ((char *)&my) + n, sizeof(my) - n);
```

B 先把本地信息写给 A。`write` 可能一次写不完，所以循环直到写完。

```c
n = 0;
while (n < (int)sizeof(peer))
	n += read(sock, ((char *)&peer) + n, sizeof(peer) - n);
peer.qp_num = ntohl(peer.qp_num);
peer.psn = ntohl(peer.psn);
peer.rkey = ntohl(peer.rkey);
peer.lid = ntohs(peer.lid);
peer.addr = be64toh(peer.addr);
```

B 再读取 A 的信息，并把字段转回主机字节序。顺序是“先写后读”，与 A 的“先读后写”配合，避免两端同时阻塞在 read 上。

---

## 七、修改 QP 状态机

### 7.1 改到 INIT

```c
struct ibv_qp_attr attr = {0};

attr.qp_state = IBV_QPS_INIT;
attr.pkey_index = 0;
attr.port_num = IB_PORT;
attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE;
ibv_modify_qp(qp, &attr,
	IBV_QP_STATE | IBV_QP_PKEY_INDEX |
	IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
```

与 A 完全相同：把 QP 改到 INIT 状态，指定端口和访问权限（允许对端 RDMA Write）。

### 7.2 改到 RTR

```c
memset(&attr, 0, sizeof(attr));
attr.qp_state = IBV_QPS_RTR;
attr.path_mtu = IBV_MTU_1024;
attr.dest_qp_num = peer.qp_num;
attr.rq_psn = peer.psn;
attr.max_dest_rd_atomic = 1;
attr.min_rnr_timer = 12;
attr.ah_attr.is_global = 1;
attr.ah_attr.grh.dgid = peer.gid;
attr.ah_attr.grh.sgid_index = GID_INDEX;
attr.ah_attr.grh.hop_limit = 1;
attr.ah_attr.dlid = 0;
attr.ah_attr.sl = 0;
attr.ah_attr.port_num = IB_PORT;
ibv_modify_qp(qp, &attr,
	IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
	IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
	IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
```

用 A 的 `qp_num`、`psn`、`gid` 配置 RTR。此时 B 的 QP 已能接收来自 A 的包。

### 7.3 改到 RTS

```c
memset(&attr, 0, sizeof(attr));
attr.qp_state = IBV_QPS_RTS;
attr.timeout = 14;
attr.retry_cnt = 7;
attr.rnr_retry = 7;
attr.sq_psn = psn;
attr.max_rd_atomic = 1;
ibv_modify_qp(qp, &attr,
	IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
	IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
```

把 QP 改到 RTS。B 随后可以主动发 RDMA Write。

---

## 八、Post Receive 并通知 A 就绪

```c
struct ibv_sge rsge = {
	.addr = (uint64_t)buf,
	.length = sizeof(buf),
	.lkey = mr->lkey,
};
struct ibv_recv_wr rwr = {
	.wr_id = 1,
	.sg_list = &rsge,
	.num_sge = 1,
};
struct ibv_recv_wr *bad_recv;
ibv_post_recv(qp, &rwr, &bad_recv);
```

B 先把接收 WR 挂到 QP。因为 A 的 RDMA Write With Immediate 会消耗一个 recv WR，所以必须提前准备好。

```c
char ready = 1;
n = 0;
while (n < 1)
	n += write(sock, &ready, 1);
```

B 向 A 的 TCP 连接写入 1 字节，告诉 A“我已经 post 好 recv，你可以发 RDMA Write 了”。

---

## 九、等待并打印 A 的消息

```c
struct ibv_wc wc;
while (ibv_poll_cq(cq, 1, &wc) == 0) {}
printf("received: %s\n", buf);
```

B 轮询 CQ。A 的 RDMA Write With Immediate 到达后，会在 B 的 recv CQ 上产生一个完成事件。`ibv_poll_cq` 返回后，`buf` 里已经是 `"Hello RDMA RC write world!"`，调用 `printf` 打印。

---

## 十、回复 A

```c
char *msg = "Hello!";
int len = strlen(msg) + 1;
memcpy(buf, msg, len);
```

把回复字符串（含结尾 `\0`）复制到 `buf`。

```c
struct ibv_sge ssge = {
	.addr = (uint64_t)buf,
	.length = len,
	.lkey = mr->lkey,
};
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
struct ibv_send_wr *bad_send;
ibv_post_send(qp, &swr, &bad_send);
```

构造并提交带立即数的 RDMA Write WR。目标地址是 A 的缓冲区 `peer.addr`，目标 key 是 A 的 `peer.rkey`。`IBV_SEND_SIGNALED` 保证发送完成后在 B 自己的 CQ 上产生完成事件。

```c
while (ibv_poll_cq(cq, 1, &wc) == 0) {}
```

B 轮询等待自己的发送完成。完成意味着 `"Hello!"` 已经到达 A 的内存，A 随后会打印它。

---

## 十一、程序结束

```c
return 0;
```

主函数返回。示例程序没有显式释放资源，进程退出时由操作系统回收。

---

## 十二、整体流程回顾

1. B 根据命令行 IP 通过 TCP 连接 A。
2. 两端分别初始化 IB 资源，创建 RC QP。
3. B 先写自己的 QP 信息给 A，再读取 A 的 QP 信息。
4. 两端把 QP 改到 `INIT` → `RTR` → `RTS`。
5. B post recv，然后通过 TCP 发 1 字节 ready 给 A。
6. A 收到 ready 后，用 RDMA Write With Immediate 把 Hello 消息写到 B 的缓冲区。
7. B poll 到 recv 完成，打印消息。
8. B 再用 RDMA Write With Immediate 把回复写到 A 的缓冲区。
9. A poll 到 send 完成和 recv 完成，打印回复。

这就是 `rcwrite_b.c` 的全部执行流程。
