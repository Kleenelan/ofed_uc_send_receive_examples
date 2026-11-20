# RC Write 双机示例

本目录包含两个极简的 RDMA RC Write 程序：

- `rcwrite_a.c`：运行在主机 A，监听 TCP 连接，先通过 RDMA RC Write 发送 `"Hello RDMA RC write world!"`，然后等待并打印 B 的回复。
- `rcwrite_b.c`：运行在主机 B，主动连接主机 A，先等待 A 的消息并打印，然后通过 RDMA RC Write 回复 `"Hello!"`。

两个源文件都只有 `main` 一个函数，没有错误检查，只保留完成一次 RC Write 往返所必需的调用。

---

## 1. 环境探索与 OFED 验证

### 1.1 工作目录检查

进入项目目录并查看内容：

```bash
ls -la
```

输出：

```
total 8
drwxrwxr-x 2 hongleili hongleili 4096 Nov 19 05:52 .
drwxrwxr-x 9 hongleili hongleili 4096 Nov 19 05:52 ..
```

目录初始为空，后续放入源码、说明文档和编译产物。

### 1.2 确认 OFED / libibverbs 头文件存在

```bash
ls /usr/include/infiniband/ | head -30
```

输出：

```
acm.h
acm_prov.h
arch.h
efadv.h
ib.h
ib_user_ioctl_verbs.h
mad.h
mad_osd.h
mlx4dv.h
mlx5_api.h
mlx5dv.h
mlx5_user_ioctl_verbs.h
opcode.h
sa.h
sa-kern-abi.h
umad_cm.h
umad.h
umad_sa.h
umad_sa_abi.h
umad_types.h
verbs_api.h
verbs.h
```

其中 `verbs.h` 是编写 RDMA 程序的核心头文件，证明 libibverbs 用户态开发包已经安装。

### 1.3 查看本机 IB 设备

```bash
ibv_devinfo
```

输出（节选）：

```
hca_id:	mlx5_0
	transport:			InfiniBand (0)
	fw_ver:				16.35.8002
	node_guid:			b8ce:f603:002a:07a4
	sys_image_guid:			b8ce:f603:002a:07a4
	vendor_id:			0x02c9
	vendor_part_id:		4119
	hw_ver:				0x0
	board_id:			MT_0000000008
	phys_port_cnt:			1
		port:	1
			state:			PORT_ACTIVE (4)
			max_mtu:		4096 (5)
			active_mtu:		1024 (3)
			sm_lid:			0
			port_lid:		0
			port_lmc:		0x00
			link_layer:		Ethernet

hca_id:	mlx5_1
	transport:			InfiniBand (0)
	fw_ver:				16.35.8002
	...
```

结论：本机有两张 mlx5 verbs 设备（`mlx5_0`、`mlx5_1`），对应一张双口 CX5 网卡的两个端口；每个设备只有一个端口；链路层为 Ethernet，因此使用 RoCE。

### 1.4 查看 GID 与 IP 配置

```bash
show_gids
```

输出：

```
DEV	PORT	INDEX	GID					IPv4		VER	DEV
---	----	-----	---					------------	---	---
mlx5_0	1	0	fe80:0000:0000:0000:bace:f6ff:fe2a:07a4			v1	enp3s0f0np0
mlx5_0	1	1	fe80:0000:0000:0000:bace:f6ff:fe2a:07a4			v2	enp3s0f0np0
mlx5_0	1	2	0000:0000:0000:0000:0000:ffff:c0a8:0064	192.168.0.100	v1	enp3s0f0np0
mlx5_0	1	3	0000:0000:0000:0000:0000:ffff:c0a8:0064	192.168.0.100	v2	enp3s0f0np0
mlx5_1	1	0	fe80:0000:0000:0000:bace:f6ff:fe2a:07a5			v1	enp3s0f1np1
mlx5_1	1	1	fe80:0000:0000:0000:bace:f6ff:fe2a:07a5			v2	enp3s0f1np1
mlx5_1	1	2	0000:0000:0000:0000:0000:ffff:c0a8:0065	192.168.0.101	v1	enp3s0f1np1
mlx5_1	1	3	0000:0000:0000:0000:0000:ffff:c0a8:0065	192.168.0.101	v2	enp3s0f1np1
n_gids_found=8
```

每个端口都有链路本地 GID（INDEX 0/1）和 IPv4 嵌入的 GID（INDEX 2/3）。本程序默认使用 `GID_INDEX = 3`（IPv4 对应的 RoCEv2 GID，与 `192.168.0.xxx` 匹配）。如果你的环境该索引不同，请修改源码里的 `GID_INDEX`。

### 1.5 编译与依赖验证

```bash
gcc -O2 -Wall -o rcwrite_a rcwrite_a.c -libverbs
gcc -O2 -Wall -o rcwrite_b rcwrite_b.c -libverbs
ldd rcwrite_a rcwrite_b
```

`ldd` 确认可执行文件链接到：

```
libibverbs.so.1 => /lib/x86_64-linux-gnu/libibverbs.so.1
```

编译过程无警告、无错误。

---

## 2. RC 连接建立所需信息、获取方法与交换方式

### 2.1 为什么需要交换信息

RC（Reliable Connected）QP 是面向连接的。两个 QP 在能够直接收发 RDMA 报文之前，必须互相知道对方的若干关键参数。这些参数不能从链路层自动协商得到（不像 TCP 三次握手），必须由应用程序显式传递。

### 2.2 需要交换的全部信息

本例中两端通过 TCP 交换一个 `struct info`：

| 字段 | 含义 | 获取方法 |
|------|------|----------|
| `qp_num` | 对端 RC QP 的编号 | `ibv_create_qp()` 返回的 `qp->qp_num` |
| `psn` | 对端发送队列的初始 Packet Sequence Number | 由应用程序自行选定，例如 `0x12345678` |
| `lid` | InfiniBand Local ID | RoCE 不使用 LID，固定填 `0` |
| `gid` | 对端端口的 GID（RoCE 必需） | `ibv_query_gid(ctx, port, gid_index, &gid)` |
| `rkey` | 对端 Memory Region 的 remote key | `ibv_reg_mr()` 返回的 `mr->rkey` |
| `addr` | 对端接收缓冲区的虚拟地址 | 用户缓冲区指针，例如 `char buf[256]` 的地址 |

### 2.3 各信息的具体用途

- **qp_num**：本端把 QP 改到 `RTR` 状态时，需要填入 `attr.dest_qp_num`，告诉硬件收到的包应该匹配哪个远端 QP。
- **psn**：本端 `RTR` 的 `attr.rq_psn` 必须等于对端发送队列的起始 PSN，这样接收端才能正确校验报文顺序。
- **lid**：传统 InfiniBand 用 LID 寻址；RoCE 使用 Ethernet + GID，`lid` 无意义，填 0。
- **gid**：RoCE 的 Address Handle 需要目的 GID（`grh.dgid`）和本端 GID 索引（`grh.sgid_index`），用于构造全局路由头。
- **rkey**：RDMA Write 到对端内存时，必须在 WR 中提供对端 MR 的 `rkey`，否则硬件会拒绝访问。
- **addr**：RDMA Write 的远端目标地址。对端把这个本地缓冲区地址注册成 MR 后发给本端。

### 2.4 信息交换方式

两端在 RDMA 之前先建立一条普通 TCP 连接：

1. A 监听 TCP 端口 `12345`。
2. B 用 A 的 IP 连接该端口。
3. 连接建立后，A 先读取 B 的 `struct info`，再把自己的 `struct info` 写给 B；B 则先写后读。
4. 所有整数字段在传输前转成大端（网络字节序），接收后转回主机字节序；GID 作为字节数组直接复制。

这种“一读一写”的顺序避免了两端同时阻塞在 `read` 上造成死锁。

### 2.5 QP 状态机与上述信息的关系

拿到对端信息后，两端分别调用 `ibv_modify_qp` 三次：

1. `RESET` → `INIT`：指定本端端口、PKEY、访问权限（`IBV_ACCESS_REMOTE_WRITE`）。
2. `INIT` → `RTR`：填入对端 `qp_num`、`psn`、`gid`，配置 Address Handle 和路径 MTU。
3. `RTR` → `RTS`：指定本端 `psn`、超时、重传等参数。

只有完成这三步，RC QP 才能正式收发 RDMA 报文。

---

## 3. 程序设计思路

1. A 监听 TCP 端口 `12345`。
2. B 用 A 的 IP 连接该端口。
3. 两端分别创建 IB 资源（context、pd、cq、mr、rc qp），并通过 TCP 互换 `struct info`。
4. 两端把 QP 从 `RESET` 改到 `INIT` → `RTR` → `RTS`。
5. B 先 post 一个 receive WR，然后向 A 发送 1 字节 TCP “ready”。
6. A 收到 ready 后，post RDMA Write With Immediate，把字符串写到 B 的缓冲区；该操作会在 B 的 recv CQ 上产生一个完成事件。
7. B poll 到 recv 完成，打印消息，再 post 另一个 RDMA Write With Immediate 把回复写到 A 的缓冲区。
8. A poll 到 send 完成和 recv 完成，打印回复。

之所以使用 `IBV_WR_RDMA_WRITE_WITH_IMM` 而不是普通 `IBV_WR_RDMA_WRITE`，是因为普通 RDMA Write 在接收端不会生成完成事件；而带 Immediate 的 Write 会消耗一个 recv WQE 并在接收端产生 CQE，这样接收端才能知道数据已经到达并打印。

---

## 4. 编译

可以直接用 Makefile：

```bash
make
```

输出：

```
gcc -O2 -Wall -o rcwrite_a rcwrite_a.c -libverbs
gcc -O2 -Wall -o rcwrite_b rcwrite_b.c -libverbs
```

也可以手动编译：

```bash
gcc -O2 -Wall -o rcwrite_a rcwrite_a.c -libverbs
gcc -O2 -Wall -o rcwrite_b rcwrite_b.c -libverbs
```

没有警告、没有错误。清理编译产物：

```bash
make clean
```

依赖检查：

```bash
ldd rcwrite_a rcwrite_b
```

确认二者都链接到了 `libibverbs.so.1`。

---

## 5. 运行与测试

### 5.1 两台物理主机（默认用法）

主机 A（只有一张 CX5，使用第一个 verbs 设备，默认 dev 0）：

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

如果一张双口 CX5 的两个端口用网线直连，并且两个进程都跑在同一台 Linux 上，必须让两端分别打开不同的 verbs 设备，否则 RDMA 报文无法从一条链路进入另一条链路。

本机 `ibv_devinfo` 显示两个设备：

- `mlx5_0` → IP `192.168.0.100`
- `mlx5_1` → IP `192.168.0.101`

因此在本机测试时：

终端 1（用 dev 0）：

```bash
./rcwrite_a 0
```

终端 2（用 dev 1，连接 A 的 IP）：

```bash
./rcwrite_b 192.168.0.100 1
```

程序接受一个可选的设备索引参数：

- `rcwrite_a [dev_idx]`：缺省为 `0`。
- `rcwrite_b <A_IP> [dev_idx]`：缺省为 `0`。

### 5.3 预期输出

主机 B 屏幕上会先出现：

```
received: Hello RDMA RC write world!
```

主机 A 屏幕上随后出现：

```
received: Hello!
```

### 5.4 本地双口说明

TCP 元数据交换仍走本机回环（因为目标 IP 是本机地址），但真正的 RDMA 流量会从一个端口发出、经网线进入另一个端口，因为两端 QP 分别建在不同的 verbs 设备上，且使用了 IPv4 对应的 RoCEv2 GID（`GID_INDEX = 3`）。该跑法已在本机双口直连环境实测通过。

---

## 6. 注意事项

- `TCP_PORT`、`IB_PORT`、`GID_INDEX` 都是源码中的宏，必要时请根据实际环境修改。
- 程序没有错误检查，仅用于演示最小流程；生产环境请检查每个 verbs 调用和 socket 调用的返回值。
- 如果运行时报 `Destination unreachable` 或 QP 进入错误状态，请检查：
  - 两端 `GID_INDEX` 是否一致且对应 RoCEv2；
  - 防火墙是否放行了 RoCE 流量（UDP 4791 等）以及 TCP 12345；
  - 两端 MTU、PKEY、port 是否一致。

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
