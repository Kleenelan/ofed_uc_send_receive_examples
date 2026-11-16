# UC Send/Receive App

最简的 RDMA UC（Unreliable Connected）send/recv 应用示例。

发送端与接收端分别独立实现，各为一个 C 文件，无公共文件。

## 环境

- Mellanox ConnectX-5 双口网卡
- 端口 1：`192.168.0.100`（`mlx5_0`）
- 端口 2：`192.168.0.101`（`mlx5_1`）
- 已安装 OFED 驱动
- 链路层：Ethernet（RoCE），使用 GID

## 文件

- `uc_send.c` — 发送端
- `uc_receive.c` — 接收端
- `Makefile`

## 编译

```bash
make
```

## 本机双端口测试

B 端（接收方，mlx5_1 / 192.168.0.101）：

```bash
./uc_receive -d mlx5_1 -p 1 -g 2
```

A 端（发送方，mlx5_0 / 192.168.0.100）：

```bash
./uc_send -d mlx5_0 -p 1 -g 2 -c 192.168.0.101 -m "hello world!"
```

## 两台主机测试

可以运行在两台主机上，只需满足：

1. 两端使用 **同一 GID index**，且该 GID 对应的地址在两台主机间可达。
2. 发送端 `-c` 指定接收端主机的 IP。
3. 防火墙放行 TCP 端口 `12345`。

推荐为 RoCE 使用与 IPv4 地址对应的 GID index（通常为 `2` 或 `3`），而不是默认的 link-local GID（`0`）。

例如：

主机 B（IP `192.168.0.101`，设备 `mlx5_0`）：

```bash
./uc_receive -d mlx5_0 -p 1 -g 2
```

主机 A（IP `192.168.0.100`，设备 `mlx5_0`）：

```bash
./uc_send -d mlx5_0 -p 1 -g 2 -c 192.168.0.101 -m "hello world!"
```

## 已验证输出

```text
sent: hello world!
received: hello world!
```

## 说明

- 使用 `ibverbs` 创建 UC QP。
- 通过 TCP 端口 `12345` 交换 QP 信息（QPN / PSN / GID）。
- 代码保持最小化，无多余依赖。
- 使用 `-g` 指定 GID index，`0` 为 link-local，`2`/`3` 通常为 IPv4-mapped 地址。
