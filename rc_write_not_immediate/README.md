# RC Write（不带 Immediate）双机示例

本目录是 `rcwrite_a.c` / `rcwrite_b.c` 的普通 RDMA Write 版本。与带 Immediate 的版本相比，唯一区别是：

- 使用 `IBV_WR_RDMA_WRITE`（普通 Write）传输字符串。
- 普通 Write 不会在对端产生完成事件，因此两端额外通过一个 `flag` 字段做“门铃”通知：发送方先写数据，再写标志位；接收方轮询标志位，标志位变为 1 时即可读取并打印数据。
- 字符串内容预先存放在 `malloc` 申请的 `struct msg_buf` 中。

两个源文件仍然只有 `main` 一个函数，没有错误检查。

---

## 1. 环境探索与 OFED 验证

### 1.1 工作目录

```bash
mkdir -p rc_write_not_immediate
```

项目根目录下创建该子目录，用于存放本版本的源码、Makefile 与文档。

### 1.2 确认 libibverbs 可用

```bash
ls /usr/include/infiniband/verbs.h
ls /lib/x86_64-linux-gnu/libibverbs.so.1
```

两个文件均存在，说明 OFED/libibverbs 已安装。

### 1.3 查看本机 IB 设备

```bash
ibv_devinfo
```

输出（节选）：

```
hca_id:	mlx5_0
	transport:			InfiniBand (0)
	fw_ver:				16.35.8002
	vendor_part_id:		4119
	phys_port_cnt:			1
		port:	1
			state:			PORT_ACTIVE (4)
			active_mtu:		1024 (3)
			link_layer:		Ethernet

hca_id:	mlx5_1
...
```

### 1.4 查看 GID 与 IP 配置

```bash
show_gids
```

输出（节选）：

```
DEV	PORT	INDEX	GID					IPv4		VER	DEV
mlx5_0	1	1	fe80:...:bace:f6ff:fe2a:07a4				v2	enp3s0f0np0
mlx5_0	1	3	0000:...:ffff:c0a8:0064		192.168.0.100	v2	enp3s0f0np0
mlx5_1	1	1	fe80:...:bace:f6ff:fe2a:07a5				v2	enp3s0f1np1
mlx5_1	1	3	0000:...:ffff:c0a8:0065		192.168.0.101	v2	enp3s0f1np1
```

程序默认 `GID_INDEX = 3`（IPv4 对应的 RoCEv2 GID，与 `192.168.0.xxx` 匹配）。如果你的环境该索引不同，请修改源码里的 `GID_INDEX`。

### 1.5 编译验证

```bash
make
```

输出：

```
gcc -O2 -Wall -o rcwrite_a rcwrite_a.c -libverbs
gcc -O2 -Wall -o rcwrite_b rcwrite_b.c -libverbs
```

无警告、无错误。`ldd rcwrite_a rcwrite_b` 确认二者链接到 `libibverbs.so.1`。

---

## 2. RC 连接建立所需信息、获取方法与交换方式

### 2.1 需要交换的全部信息

| 字段 | 含义 | 获取方法 |
|------|------|----------|
| `qp_num` | 对端 RC QP 编号 | `ibv_create_qp()` 返回的 `qp->qp_num` |
| `psn` | 对端发送队列初始序号 | 应用程序自行选定 |
| `lid` | InfiniBand Local ID | RoCE 不使用，填 `0` |
| `gid` | 对端端口 GID | `ibv_query_gid(ctx, port, gid_index, &gid)` |
| `rkey` | 对端 MR 的 remote key | `ibv_reg_mr()` 返回的 `mr->rkey` |
| `addr` | 对端 `struct msg_buf *` 的虚拟地址 | `malloc()` 返回的指针 |

### 2.2 与普通 Write 的区别

带 Immediate 的 Write 会消耗对端一个 recv WQE，因此接收端可以通过 poll CQ 知道数据到达。普通 Write 不会在对端产生任何通知，所以接收端需要自己看内存里的标志位。

本程序把缓冲区设计成一个结构体：

```c
struct msg_buf {
    volatile uint64_t flag;
    char data[256];
};
```

- `data` 存放字符串。
- `flag` 作为“门铃”。

发送方：

1. 用普通 RDMA Write 把 `data` 写到对端 `data` 字段。
2. 再用一次普通 RDMA Write 把 `flag` 写成 `1`。

由于同一个 RC QP 上的 Write 按提交顺序执行，对端一定先看到完整数据，再看到 `flag` 变 1。

接收方：

1. 初始 `flag = 0`。
2. 轮询 `while (!buf->flag) {}`。
3. 当 `flag` 变为 1 时，读取 `data` 并打印。

### 2.3 信息交换方式

两端先建立 TCP 连接，然后互相发送一个 `struct info`（含上述 6 个字段）。A 先读后写，B 先写后读，避免死锁。整数字段使用网络字节序传输。

---

## 3. 程序设计思路

1. A 监听 TCP 端口 `12345`。
2. B 用 A 的 IP 连接该端口。
3. 两端分别 `malloc` 一个 `struct msg_buf`，注册 MR，创建 RC QP。
4. 通过 TCP 互换 `struct info`。
5. 两端把 QP 改到 `INIT` → `RTR` → `RTS`。
6. A 预先把 `"Hello RDMA RC write world!"` 写入本地 `buf->data`。
7. B post 完成后，通过 TCP 向 A 发送 1 字节 ready。
8. A 收到 ready 后，先普通 Write 数据到 B 的 `buf->data`，再普通 Write `flag = 1` 到 B 的 `buf->flag`。
9. B 轮询到 `flag == 1`，打印消息；然后把 `"Hello!"` 写入本地 `buf->data`。
10. B 先普通 Write 数据到 A 的 `buf->data`，再普通 Write `flag = 1` 到 A 的 `buf->flag`。
11. A 轮询到 `flag == 1`，打印回复。

---

## 4. 编译

```bash
make
```

或手动：

```bash
gcc -O2 -Wall -o rcwrite_a rcwrite_a.c -libverbs
gcc -O2 -Wall -o rcwrite_b rcwrite_b.c -libverbs
```

清理：

```bash
make clean
```

---

## 5. 运行与测试

### 5.1 两台物理主机（默认用法）

主机 A：

```bash
./rcwrite_a
```

主机 B：

```bash
./rcwrite_b <A_IP>
```

例如：

```bash
./rcwrite_b 192.168.0.100
```

### 5.2 本机双口直连测试

如果一张双口 CX5 的两个端口用网线直连，并且两个进程都跑在同一台 Linux 上，必须让两端分别打开不同的 verbs 设备。本机设备对应关系：

- `mlx5_0` → IP `192.168.0.100`
- `mlx5_1` → IP `192.168.0.101`

终端 1（用 dev 0）：

```bash
./rcwrite_a 0
```

终端 2（用 dev 1）：

```bash
./rcwrite_b 192.168.0.100 1
```

程序接受可选设备索引参数：

- `rcwrite_a [dev_idx]`：缺省 `0`。
- `rcwrite_b <A_IP> [dev_idx]`：缺省 `0`。

### 5.3 预期输出

主机 B：

```
received: Hello RDMA RC write world!
```

主机 A：

```
received: Hello!
```

### 5.4 本地双口说明

TCP 元数据交换仍走本机回环，但真正的 RDMA 流量会从一个端口发出、经网线进入另一个端口，因为两端 QP 分别建在不同的 verbs 设备上，且使用了 IPv4 对应的 RoCEv2 GID（`GID_INDEX = 3`）。该跑法已在本机双口直连环境实测通过。

---

## 6. 注意事项

- `TCP_PORT`、`IB_PORT`、`GID_INDEX` 为源码宏，可按需修改。
- 程序无错误检查，仅用于演示最小流程。
- 如果 `flag` 轮询一直不返回，请检查 GID、MTU、防火墙、QP 状态等。

---

## 7. 文件清单

```
Makefile             编译规则
rcwrite_a.c          主机 A 程序源码
rcwrite_b.c          主机 B 程序源码
rcwrite_a            主机 A 可执行文件
rcwrite_b            主机 B 可执行文件
README.md            本说明
rcwrite_a_detail.md  rcwrite_a.c 逐句解释
rcwrite_b_detail.md  rcwrite_b.c 逐句解释
```
