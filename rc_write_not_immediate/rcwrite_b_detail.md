# rcwrite_b.c（普通 Write 版）逐句详细解释

本文档按照 `rc_write_not_immediate/rcwrite_b.c` 的执行顺序，逐句解释每个头文件、宏、变量、函数调用和语句的作用。建议与源码对照阅读。

---

## 一、头文件

与 A 版本相同：

- `stdio.h`：提供 `printf`。
- `stdlib.h`：提供 `malloc`。
- `string.h`：提供 `memcpy`、`strlen`、`memset`。
- `stdint.h`：固定宽度整数类型。
- `stddef.h`：提供 `offsetof`。
- `unistd.h`：提供 `read`、`write`。
- `endian.h`：字节序转换。
- `sys/socket.h`、`netinet/in.h`、`arpa/inet.h`：Socket API。
- `infiniband/verbs.h`：IB Verbs API。

---

## 二、宏定义与数据结构

```c
#define TCP_PORT   12345
#define IB_PORT    1
#define GID_INDEX  3
```

与 A 一致。本机 `show_gids` 中 IP `192.168.0.xxx` 对应的 RoCEv2 GID 位于 INDEX 3，因此源码默认使用 3。如果你的环境该索引不同，请修改此宏。

```c
struct info {
	uint32_t qp_num;
	uint32_t psn;
	uint32_t rkey;
	uint16_t lid;
	uint64_t addr;
	union ibv_gid gid;
};

struct msg_buf {
	volatile uint64_t flag;
	char data[256];
};
```

`struct info` 用于 TCP 交换 QP 元数据；`struct msg_buf` 是两端共享的内存布局，包含门铃 `flag` 和数据 `data`。

---

## 三、主函数与 TCP 连接

```c
int main(int argc, char *argv[])
```

B 需要 A 的 IP，并且带一个可选的 verbs 设备索引参数。运行方式示例：

```bash
./rcwrite_b 192.168.0.100
```

缺省打开 dev 0。在本机双口直连测试时，需要让 B 打开另一个设备：

```bash
./rcwrite_b 192.168.0.100 1
```

```c
int dev_idx = (argc > 2) ? atoi(argv[2]) : 0;
```

```c
int sock = socket(AF_INET, SOCK_STREAM, 0);
struct sockaddr_in ra = {
	.sin_family = AF_INET,
	.sin_port = htons(TCP_PORT),
	.sin_addr.s_addr = inet_addr(argv[1]),
};
connect(sock, (struct sockaddr *)&ra, sizeof(ra));
```

创建 TCP 套接字，使用命令行传入的 IP 和固定端口 12345 连接 A。

---

## 四、IB Verbs 资源初始化

```c
struct ibv_device **devs = ibv_get_device_list(NULL);
struct ibv_context *ctx = ibv_open_device(devs[dev_idx]);
struct ibv_pd *pd = ibv_alloc_pd(ctx);
struct ibv_cq *cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
```

打开由命令行参数指定的设备，分配 PD，创建 CQ。在本机双口直连测试中，B 通常要打开 `devs[1]`（`mlx5_1`），而 A 打开 `devs[0]`（`mlx5_0`）。

```c
struct msg_buf *buf = malloc(sizeof(*buf));
memset(buf, 0, sizeof(*buf));
struct ibv_mr *mr = ibv_reg_mr(pd, buf, sizeof(*buf),
		IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
```

用 `malloc` 申请并清零 `struct msg_buf`，注册 MR。

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

创建 RC QP。

```c
uint32_t psn = 0x87654321;
union ibv_gid my_gid;
ibv_query_gid(ctx, IB_PORT, GID_INDEX, &my_gid);
```

设置 PSN，查询本端 GID。

---

## 五、通过 TCP 交换 QP 信息

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

构造本地信息。`addr` 是 `struct msg_buf *` 基地址。

```c
int n = 0;
while (n < (int)sizeof(my))
	n += write(sock, ((char *)&my) + n, sizeof(my) - n);

n = 0;
while (n < (int)sizeof(peer))
	n += read(sock, ((char *)&peer) + n, sizeof(peer) - n);
peer.qp_num = ntohl(peer.qp_num);
peer.psn = ntohl(peer.psn);
peer.rkey = ntohl(peer.rkey);
peer.lid = ntohs(peer.lid);
peer.addr = be64toh(peer.addr);
```

B 先写自己的信息，再读取 A 的信息，然后做字节序转换。

---

## 六、修改 QP 状态机

三次 `ibv_modify_qp`：

1. `RESET` → `INIT`
2. `INIT` → `RTR`（填入 A 的 `qp_num`、`psn`、`gid`）
3. `RTR` → `RTS`

与 A 版本完全相同，只是本端 PSN 值不同。

---

## 七、通知 A 已就绪

```c
char ready = 1;
n = 0;
while (n < 1)
	n += write(sock, &ready, 1);
```

B 在 QP 已经 RTS、并且本地 `flag` 已初始化为 0 之后，通过 TCP 向 A 发送 1 字节 ready。A 收到后才会开始 RDMA Write。

---

## 八、轮询并打印 A 的消息

```c
while (!buf->flag) {}
printf("received: %s\n", buf->data);
```

B 不断读取本地 `buf->flag`。A 先把字符串写到 B 的 `buf->data`，再把 `buf->flag` 写成 1。由于同一个 RC QP 上的 Write 按顺序执行，当 B 看到 `flag == 1` 时，`data` 里的内容已经完整可用。于是打印。

`flag` 声明为 `volatile`，保证编译器每次循环都重新从内存读取，而不是使用寄存器里的旧值。

---

## 九、准备回复

```c
char *msg = "Hello!";
int len = strlen(msg) + 1;
memcpy(buf->data, msg, len);
```

把回复字符串（含 `\0`）写入 `buf->data`，覆盖之前 A 发来的消息。

---

## 十、普通 RDMA Write 发送回复数据

```c
uint64_t data_off = offsetof(struct msg_buf, data);
uint64_t flag_off = offsetof(struct msg_buf, flag);
```

计算结构体成员偏移。

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

构造并提交普通 RDMA Write WR，把 `"Hello!"` 写到 A 的 `data` 字段。`IBV_SEND_SIGNALED` 让 B 能 poll 到发送完成。

---

## 十一、普通 RDMA Write 发送回复门铃

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

构造并提交第二个普通 RDMA Write WR，把 `1` 写到 A 的 `flag` 字段。A 正在轮询自己的 `flag`，看到 1 后打印回复。

注意：本地 `buf->flag` 当前已经是 1（被 A 的门铃写成 1），这里显式再赋一次 1 作为本地源数据，确保写入 A 的 `flag` 值为 1。

---

## 十二、程序结束

```c
return 0;
```

B 的主函数返回。A 随后会打印回复。示例程序没有显式释放资源。

---

## 十三、整体流程回顾

1. B 根据命令行 IP 通过 TCP 连接 A。
2. 两端初始化 IB 资源，`malloc` 并注册 `struct msg_buf`。
3. B 先写自己的 QP 信息给 A，再读取 A 的 QP 信息。
4. 两端把 QP 改到 `INIT` → `RTR` → `RTS`。
5. B 通过 TCP 向 A 发送 1 字节 ready。
6. A 普通 Write 数据到 B 的 `data`，再普通 Write `flag = 1` 到 B 的 `flag`。
7. B 轮询到 `flag == 1`，打印 `"Hello RDMA RC write world!"`。
8. B 把 `"Hello!"` 写入 `buf->data`。
9. B 普通 Write 数据到 A 的 `data`，再普通 Write `flag = 1` 到 A 的 `flag`。
10. A 轮询到 `flag == 1`，打印 `"Hello!"`。

这就是 `rc_write_not_immediate/rcwrite_b.c` 的全部执行流程。
