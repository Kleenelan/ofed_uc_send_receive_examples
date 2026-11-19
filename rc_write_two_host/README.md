# RC RDMA Write 双机/双端口示例

本目录包含两个独立的 C 源文件，演示如何在 Mellanox CX5（mlx5）网卡上使用
**Reliable Connected (RC)** 类型的 Queue Pair 进行 **RDMA Write** 操作：

- `RC_write.c`：写入端（发起 RDMA Write）。
- `RC_be_writed.c`：被写入端（暴露本地内存，轮询检测数据到达）。

程序可直接在两台主机上运行，也可在同一台主机的两个 RoCE/IB 端口之间运行。

与上级目录 `uc_write_two_host` 中的 UC 示例相比，本示例仅将 QP 类型从
`IBV_QPT_UC` 改为 `IBV_QPT_RC`，并在 QP 状态机迁移时补齐 RC 特有的可靠性参数
（`timeout`、`retry_cnt`、`rnr_retry`、`max_dest_rd_atomic` 等）。

---

## 1. 文件结构

```
.
├── RC_write.c      # RDMA Write 发起端
├── RC_be_writed.c  # RDMA Write 目标端（内存轮询）
├── RC_write        # 编译产物
├── RC_be_writed    # 编译产物
└── README.md       # 本文件
```

两个源文件完全独立，各自包含所有必要代码（IB 资源初始化、TCP 元数据交换、
RC QP 状态机迁移、内存注册、完成轮询/内存轮询、资源释放）。

---

## 2. 编译

需要已安装 OFED / rdma-core 开发包（提供 `libibverbs`）。

### 2.1 使用 Makefile（推荐）

```bash
make
```

编译成功后生成两个可执行文件 `RC_write` 和 `RC_be_writed`。

清理编译产物：

```bash
make clean
```

### 2.2 手动编译

```bash
gcc -o RC_write RC_write.c -libverbs -Wall
gcc -o RC_be_writed RC_be_writed.c -libverbs -Wall
```

---

## 3. 运行前环境检查

在运行本示例之前，请先确认 IB 设备、端口状态、GID 索引与 IP 地址的对应关系。

### 3.1 查看 IB 设备与端口状态

```bash
ibv_devinfo
```

示例输出关键信息：

```
hca_id: mlx5_0
    phys_port_cnt: 1
        port: 1
            state: PORT_ACTIVE (4)
            max_mtu: 4096 (5)
            active_mtu: 1024 (3)
            link_layer: Ethernet

hca_id: mlx5_1
    phys_port_cnt: 1
        port: 1
            state: PORT_ACTIVE (4)
            link_layer: Ethernet
```

- `state` 应为 `PORT_ACTIVE`，否则 RDMA 流量无法收发。
- `link_layer` 为 `Ethernet` 时表示 RoCE 模式；为 `InfiniBand` 时表示 IB 模式。

### 3.2 查看 IP 与网卡对应关系

```bash
ip addr show
```

找到目标网卡（如 `enp3s0f0np0`、`enp3s0f1np1`）对应的 IP 地址。

### 3.3 查看 GID 索引

RoCE 使用 GID（Global Identifier）进行路由。需要找到与目标 IPv4 地址对应的 GID 索引：

```bash
show_gids
```

示例输出：

```
DEV     PORT  INDEX  GID                                      IPv4            VER
---     ----  -----  ---                                      ------------    ---
mlx5_0  1     0      fe80:0000:0000:0000:bace:f6ff:fe2a:07a4                  v1
mlx5_0  1     1      fe80:0000:0000:0000:bace:f6ff:fe2a:07a4                  v2
mlx5_0  1     2      0000:0000:0000:0000:0000:ffff:c0a8:0064  192.168.0.100   v1
mlx5_0  1     3      0000:0000:0000:0000:0000:ffff:c0a8:0064  192.168.0.100   v2
mlx5_1  1     0      fe80:0000:0000:0000:bace:f6ff:fe2a:07a5                  v1
mlx5_1  1     1      fe80:0000:0000:0000:bace:f6ff:fe2a:07a5                  v2
mlx5_1  1     2      0000:0000:0000:0000:0000:ffff:c0a8:0065  192.168.0.101   v1
mlx5_1  1     3      0000:0000:0000:0000:0000:ffff:c0a8:0065  192.168.0.101   v2
```

- 通常选择 IPv4 映射 GID 的 **v1 版本**，本例中 `192.168.0.100` 对应 `mlx5_0` 的 index `2`，
  `192.168.0.101` 对应 `mlx5_1` 的 index `2`。
- 程序中 `-g` 参数即为此 INDEX。

### 3.4 基本连通性检查

在两台主机或本机两个端口之间，先确认 IP 层能互通：

```bash
ping 192.168.0.101
```

> 注意：IP 能 ping 通不代表 RoCE 一定可达，但不通则 RDMA 一定不通。

---

## 4. 运行测试流程

### 4.1 标准本机双端口测试流程

**环境假设：**

- `mlx5_0` ↔ `192.168.0.100`，GID index `2`
- `mlx5_1` ↔ `192.168.0.101`，GID index `2`
- 两个端口通过光纤直连或交换机互通

**步骤：**

1. **打开终端 1，启动被写入端：**

   ```bash
   ./RC_be_writed -d mlx5_1 -p 1 -g 2 -P 12346
   ```

2. **打开终端 2，启动写入端：**

   ```bash
   ./RC_write -d mlx5_0 -p 1 -g 2 -c 192.168.0.101 -P 12346 -m "hello RC RDMA write"
   ```

3. **观察输出：**

   终端 1 应输出：

   ```
   [RC_be_writed] waiting for writer on TCP port 12346 ...
   [RC_be_writed] QP ready, local buf addr=0x... rkey=0x...
   [RC_be_writed] polling local buffer ...
   [RC_be_writed] received: hello RC RDMA write
   ```

   终端 2 应输出：

   ```
   [RC_write] remote buf addr=0x... rkey=0x...
   [RC_write] wrote to remote: hello RC RDMA write
   ```

### 4.2 标准两台主机测试流程

**主机 B（被写入端，IP：192.168.0.Y）：**

```bash
./RC_be_writed -d mlx5_0 -p 1 -g 2 -P 12346
```

**主机 A（写入端，IP：192.168.0.X）：**

```bash
./RC_write -d mlx5_0 -p 1 -g 2 -c 192.168.0.Y -P 12346 -m "hello from host A"
```

> 确保两台主机之间 TCP 端口（默认 12346）和 RoCE 流量均可达；必要时关闭防火墙或放行端口。

---

## 5. RDMA Write 建立连接前需交换的信息

RDMA Write 是“单边操作”，写入端需要知道对端内存的精确位置和访问权限。
因此两机在真正发起 Write 之前，必须通过带外通道（本例为 TCP）交换两类信息：
QP 连接信息和内存访问信息。

### 5.1 QP 连接信息（双向交换）

以下三项在两端之间**互相**传递，用于把各自的 QP 迁移到 `RTR/RTS`：

| 字段 | 字节数 | 含义 | 在本代码中的获取方法 |
|------|--------|------|---------------------|
| **QPN** | 4 | Queue Pair Number，QP 在本地设备上的唯一编号 | 创建 QP 后读取 `c->qp->qp_num` |
| **PSN** | 4 | Packet Sequence Number，初始序列号（24 位有效） | 程序生成：`rand() & 0xffffff` |
| **GID** | 16 | Global Identifier，RoCE/IB 全局路由地址 | 调用 `ibv_query_gid(c->ctx, port, gid_index, &gid)` 后拷贝 16 字节 |

### 5.2 内存访问信息（被写入端 → 写入端）

以下三项只从**被写入端**流向**写入端**，是 RDMA Write 能够命中目标内存的关键：

| 字段 | 字节数 | 含义 | 在本代码中的获取方法 |
|------|--------|------|---------------------|
| **addr** | 8 | 被写入端缓冲区的虚拟地址 | 直接取 `(uint64_t)c->buf` |
| **rkey** | 4 | 被写入端 MR 的 remote key，远端 RDMA Write 的访问凭证 | 注册 MR 后读取 `c->mr->rkey` |
| **len** | 4 | 缓冲区长度 | 程序中固定为 `MSG_MAX`（本例 4096） |

### 5.3 获取这些信息的代码位置

**被写入端（`RC_be_writed.c`）在 `setup()` 中准备本端信息：**

```c
c->local.qpn = c->qp->qp_num;
c->local.psn = rand() & 0xffffff;

union ibv_gid gid;
ibv_query_gid(c->ctx, port, gid_index, &gid);
memcpy(c->local.gid, &gid, 16);

c->local.addr = (uint64_t)c->buf;
c->local.rkey = c->mr->rkey;
c->local.len  = MSG_MAX;
```

**写入端（`RC_write.c`）在 `setup()` 中准备本端信息：**

```c
c->local.qpn = c->qp->qp_num;
c->local.psn = rand() & 0xffffff;

union ibv_gid gid;
ibv_query_gid(c->ctx, port, gid_index, &gid);
memcpy(c->local.gid, &gid, 16);
```

写入端所需的 `remote.addr` / `remote.rkey` / `remote.len` 在 TCP 交换后从对端信息中解析得到。

### 5.4 交换顺序与死锁避免

两端如果同时阻塞在 `read` 上等待对方先发数据，会造成死锁。因此交换顺序相反：

| 端 | 动作 |
|----|------|
| `RC_be_writed` | 先 `write` 本端信息，再 `read` 对端信息 |
| `RC_write` | 先 `read` 对端信息，再 `write` 本端信息 |

---

## 6. 命令行参数速查

#### RC_be_writed

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `-d` | `mlx5_1` | IB 设备名 |
| `-p` | `1` | 物理端口号 |
| `-g` | `0` | GID 表索引 |
| `-P` | `12345` | TCP 协商端口 |

#### RC_write

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `-d` | `mlx5_0` | IB 设备名 |
| `-p` | `1` | 物理端口号 |
| `-g` | `0` | GID 表索引 |
| `-c` | 必填 | 被写入端 IP 地址 |
| `-P` | `12345` | TCP 协商端口 |
| `-m` | `hello world!` | 要写入的字符串 |

---

## 7. 工作原理

### 7.1 RC 与 UC 的主要区别

- **QP 类型**：RC 使用 `IBV_QPT_RC`，UC 使用 `IBV_QPT_UC`。
- **可靠性**：RC 提供硬件级可靠传输、按序到达、自动重传；UC 不保证可靠交付。
- **操作能力**：RC 支持 RDMA Read/Write/Atomic；UC 仅支持 Send/Recv 和 RDMA Write。
- **状态机参数**：RC 在 RTR/RTS 阶段需要额外设置可靠性相关参数：
  - `max_dest_rd_atomic`、`min_rnr_timer`（RTR 阶段）
  - `timeout`、`retry_cnt`、`rnr_retry`、`max_rd_atomic`（RTS 阶段）

### 7.2 RDMA Write 单边操作

与 `SEND/RECV` 不同，**RDMA Write** 不需要接收端预先投递 Receive WR，
也不需要接收端 CPU 参与：

1. 被写入端注册一段本地内存，并声明 `IBV_ACCESS_REMOTE_WRITE` 权限。
2. 被写入端将这段内存的 **虚拟地址** 和 **rkey** 通过 TCP 告知写入端。
3. 写入端构造 `IBV_WR_RDMA_WRITE` 工作请求，设置 `wr.rdma.remote_addr` 和
   `wr.rdma.rkey`。
4. 写入端网卡直接将数据包发送到被写入端网卡，被写入端网卡根据 rkey 校验权限后，
   将 payload DMA 到指定内存地址。
5. 写入端通过轮询自身 CQ 确认 Write 已在本地网卡侧完成；被写入端则通过
   **轮询内存** 判断数据是否已写入。

### 7.3 为什么被写入端要轮询内存

RDMA Write 在目标端不产生 CQE（Completion Queue Entry），因为目标端没有投递
任何 WR。因此 `RC_be_writed` 不能通过 `ibv_poll_cq` 检测数据到达，而是不断检查
本地缓冲区第一个字节是否从 `\0` 变为非零（即写入端已写入字符串）。

### 7.4 QP 状态机

RC 两端都需要完成以下状态迁移：

```
RESET -> INIT -> RTR -> RTS
```

**RTR 阶段关键属性：**

```c
struct ibv_qp_attr attr = {
    .qp_state = IBV_QPS_RTR,
    .path_mtu = IBV_MTU_1024,
    .dest_qp_num = c->remote.qpn,
    .rq_psn = c->remote.psn,
    .max_dest_rd_atomic = 1,
    .min_rnr_timer = 12,
    .ah_attr = { /* GID、port、hop_limit 等 */ },
};
```

**RTS 阶段关键属性：**

```c
struct ibv_qp_attr attr = {
    .qp_state = IBV_QPS_RTS,
    .sq_psn = c->local.psn,
    .timeout = 14,
    .retry_cnt = 7,
    .rnr_retry = 7,
    .max_rd_atomic = 1,
};
```

在 `INIT` 阶段，被写入端必须将 `qp_access_flags` 设置为
`IBV_ACCESS_REMOTE_WRITE`，否则对端 Write 请求会被拒绝。

### 7.5 内存注册

- **写入端**：`ibv_reg_mr(..., IBV_ACCESS_LOCAL_WRITE)`，网卡读取本地缓冲区。
- **被写入端**：`ibv_reg_mr(..., IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE)`，
  网卡既能 DMA 写入本地内存，也允许远端 RDMA Write。

---

## 8. 本机测试记录

**测试环境：**

- 主机：Mellanox CX5 双端口网卡
- 端口 1（`mlx5_0`）：`192.168.0.100`，GID index 2
- 端口 2（`mlx5_1`）：`192.168.0.101`，GID index 2
- 两个端口通过光纤直连，状态 `PORT_ACTIVE`

**执行命令：**

```bash
# 终端 1
./RC_be_writed -d mlx5_1 -p 1 -g 2 -P 12346

# 终端 2
./RC_write -d mlx5_0 -p 1 -g 2 -c 192.168.0.101 -P 12346 -m "hello RC RDMA write"
```

**实际输出：**

```
# 终端 1
[RC_be_writed] waiting for writer on TCP port 12346 ...
[RC_be_writed] QP ready, local buf addr=0x5721fb11f858 rkey=0x1bcdcc
[RC_be_writed] polling local buffer ...
[RC_be_writed] received: hello RC RDMA write

# 终端 2
[RC_write] remote buf addr=0x5721fb11f858 rkey=0x1bcdcc
[RC_write] wrote to remote: hello RC RDMA write
```

测试结果：写入端成功通过 RC RDMA Write 将字符串写入对端内存，被写入端通过内存轮询
检测到数据并正确打印。

---

## 9. 常见问题

### 9.1 `ibv_modify_qp RTR: No data available`

通常是因为使用了错误的 GID index。请使用 `show_gids` 查看与目标 IP 对应的 GID：

```bash
show_gids
```

选择包含目标 IPv4 地址的 GID 索引（例如 IPv4 映射 GID 的 v1 版本，索引 2）。

### 9.2 连接建立成功但 Write 失败

检查被写入端 MR 是否注册了 `IBV_ACCESS_REMOTE_WRITE`，以及 QP 在 INIT 状态时
`qp_access_flags` 是否包含 `IBV_ACCESS_REMOTE_WRITE`。

### 9.3 被写入端一直轮询不到数据

- 检查两端 QP 状态是否都已到达 `RTR/RTS`。
- 检查写入端 `ibv_poll_cq` 是否返回成功，确认 Write 已发出。
- 检查被写入端内存是否被正确注册以及 `rkey`/`addr` 是否正确传递。
- 检查 RoCE 链路层是否互通（IP 能 ping 通不代表 RoCE 路径一定正常）。

---

## 10. 扩展建议

- 当前轮询策略为“检查 `buf[0] != '\0'`”，生产环境建议使用显式序列号或完成标志位，
  避免全零 payload 的误判。
- RC 支持 `IBV_WR_RDMA_WRITE_WITH_IMMEDIATE`，可在 Write 完成时向接收端 CQ 产生
  一个带 immediate 数据的 CQE，从而替代内存轮询。
- 可根据需要增大 `MSG_MAX` 或改为命令行参数配置。

---

## 11. 参考

- OFED / rdma-core：`libibverbs`
- 相关示例：同级目录中的 `uc_write_two_host`
- `show_gids`、`ibv_devinfo` 等工具用于确认 GID 与端口状态
