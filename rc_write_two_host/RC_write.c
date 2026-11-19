/*
 * RC_write.c - InfiniBand/RDMA Reliable Connected (RC) 写入端示例
 *
 * 本程序作为 RDMA Write 的“发起端”：
 *   1. 初始化 IB 设备、保护域、完成队列、内存区域和 RC QP。
 *   2. 通过 TCP 连接被写入端，交换 QP 信息（QPN、PSN、GID）以及对端暴露的
 *      缓冲区地址（remote_addr）和远程访问密钥（rkey）。
 *   3. 将 QP 状态迁移到 RTS（Ready To Send）。
 *   4. 准备数据并投递 RDMA Write WR，将本地缓冲区数据直接写入对端内存。
 *   5. 轮询 CQ 等待写入完成，printf 输出已发送的信息。
 *
 * 与 UC 不同，RC（Reliable Connected）QP 提供可靠传输、按序到达，并支持更丰富的
 * 远端操作。本示例仅使用 RDMA Write。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <endian.h>
#include <sys/socket.h>
#include <infiniband/verbs.h>

#define TCP_PORT 12345   /* 默认用于交换 QP/内存元数据的 TCP 端口 */
#define MSG_MAX  4096    /* 发送缓冲区最大字节数 */

/*
 * struct rdma_info - 需要在两端之间交换的连接与内存信息
 */
struct rdma_info {
    uint32_t qpn;
    uint32_t psn;
    uint8_t  gid[16];
    uint64_t addr;
    uint32_t rkey;
    uint32_t len;
};

/*
 * struct ctx - 聚合整个 RDMA 运行时需要的对象
 */
struct ctx {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    char buf[MSG_MAX];
    struct rdma_info local;
    struct rdma_info remote;
};

/* 确保从 socket 完整读取 n 字节 */
static void read_all(int s, void *buf, size_t n) {
    char *p = buf;
    while (n) {
        ssize_t r = read(s, p, n);
        if (r <= 0) { perror("read"); exit(1); }
        p += r; n -= r;
    }
}

/* 确保向 socket 完整写入 n 字节 */
static void write_all(int s, const void *buf, size_t n) {
    const char *p = buf;
    while (n) {
        ssize_t w = write(s, p, n);
        if (w <= 0) { perror("write"); exit(1); }
        p += w; n -= w;
    }
}

/* 主动连接被写入端，返回已连接的 socket fd */
static int tcp_client(const char *ip, int tcp_port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(tcp_port),
    };
    inet_pton(AF_INET, ip, &addr.sin_addr);
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        exit(1);
    }
    return s;
}

/* 将 struct rdma_info 序列化为 40 字节网络序数据 */
static void wire_rdma_info(struct rdma_info *info, char *out) {
    *(uint32_t *)(out + 0)  = htonl(info->qpn);
    *(uint32_t *)(out + 4)  = htonl(info->psn);
    memcpy(out + 8, info->gid, 16);
    *(uint64_t *)(out + 24) = htobe64(info->addr);
    *(uint32_t *)(out + 32) = htonl(info->rkey);
    *(uint32_t *)(out + 36) = htonl(info->len);
}

/* 从 40 字节网络序数据中反序列化出 struct rdma_info */
static void unwir_rdma_info(char *in, struct rdma_info *info) {
    info->qpn = ntohl(*(uint32_t *)(in + 0));
    info->psn = ntohl(*(uint32_t *)(in + 4));
    memcpy(info->gid, in + 8, 16);
    info->addr = be64toh(*(uint64_t *)(in + 24));
    info->rkey = ntohl(*(uint32_t *)(in + 32));
    info->len  = ntohl(*(uint32_t *)(in + 36));
}

/*
 * 通过 TCP 交换两端信息：写入端先读对端（含 addr/rkey），再发本端
 */
static void exch(int sock, struct rdma_info *local, struct rdma_info *remote) {
    char tx[40], rx[40];
    wire_rdma_info(local, tx);
    read_all(sock, rx, 40);
    write_all(sock, tx, 40);
    unwir_rdma_info(rx, remote);
}

/*
 * setup - 初始化全部 IB 资源
 */
static struct ctx *setup(const char *dev, int port, int gid_index) {
    struct ctx *c = calloc(1, sizeof(*c));

    struct ibv_device **devs = ibv_get_device_list(NULL);
    struct ibv_device *ibd = NULL;
    for (int i = 0; devs[i]; i++)
        if (!strcmp(ibv_get_device_name(devs[i]), dev)) ibd = devs[i];
    if (!ibd) { fprintf(stderr, "device %s not found\n", dev); exit(1); }

    c->ctx = ibv_open_device(ibd);
    ibv_free_device_list(devs);

    c->pd = ibv_alloc_pd(c->ctx);
    c->cq = ibv_create_cq(c->ctx, 16, NULL, NULL, 0);

    /*
     * 写入端注册本地 MR：网卡需要读取本地缓冲区
     */
    c->mr = ibv_reg_mr(c->pd, c->buf, MSG_MAX, IBV_ACCESS_LOCAL_WRITE);
    if (!c->mr) { perror("ibv_reg_mr"); exit(1); }

    struct ibv_qp_init_attr qpia = {
        .send_cq = c->cq, .recv_cq = c->cq,
        .cap = {
            .max_send_wr = 4, .max_recv_wr = 4,
            .max_send_sge = 1, .max_recv_sge = 1,
        },
        .qp_type = IBV_QPT_RC,   /* Reliable Connected */
    };
    c->qp = ibv_create_qp(c->pd, &qpia);

    c->local.qpn = c->qp->qp_num;
    c->local.psn = rand() & 0xffffff;

    union ibv_gid gid;
    ibv_query_gid(c->ctx, port, gid_index, &gid);
    memcpy(c->local.gid, &gid, 16);

    /* RESET -> INIT */
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = port,
        .qp_access_flags = 0,
    };
    if (ibv_modify_qp(c->qp, &attr,
                      IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                      IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
        perror("ibv_modify_qp INIT");
        exit(1);
    }

    return c;
}

/*
 * connect_qp - 根据对端 QP 信息完成 QP 状态迁移到 RTS
 */
static void connect_qp(struct ctx *c, int port, int sgid_index) {
    /* INIT -> RTR */
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = IBV_MTU_1024,
        .dest_qp_num = c->remote.qpn,
        .rq_psn = c->remote.psn,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer = 12,
        .ah_attr = {
            .is_global = 1, .port_num = port, .sl = 0,
            .grh = {
                .dgid = *(union ibv_gid *)c->remote.gid,
                .sgid_index = sgid_index,
                .hop_limit = 255,
            },
        },
    };
    if (ibv_modify_qp(c->qp, &attr,
                      IBV_QP_STATE | IBV_QP_PATH_MTU |
                      IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                      IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER |
                      IBV_QP_AV)) {
        perror("ibv_modify_qp RTR");
        exit(1);
    }

    /* RTR -> RTS */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn = c->local.psn;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.max_rd_atomic = 1;
    if (ibv_modify_qp(c->qp, &attr,
                      IBV_QP_STATE | IBV_QP_SQ_PSN |
                      IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                      IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC)) {
        perror("ibv_modify_qp RTS");
        exit(1);
    }
}

/*
 * poll_cq - 轮询完成队列，直到拿到一个成功 CQE
 */
static void poll_cq(struct ibv_cq *cq) {
    struct ibv_wc wc;
    while (ibv_poll_cq(cq, 1, &wc) < 1) {}
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "wc error: %s\n", ibv_wc_status_str(wc.status));
        exit(1);
    }
}

/*
 * cleanup - 释放所有 IB 资源
 */
static void cleanup(struct ctx *c) {
    ibv_destroy_qp(c->qp);
    ibv_dereg_mr(c->mr);
    ibv_destroy_cq(c->cq);
    ibv_dealloc_pd(c->pd);
    ibv_close_device(c->ctx);
    free(c);
}

int main(int argc, char **argv) {
    const char *dev = "mlx5_0";
    int port = 1;
    int gid_index = 0;
    const char *peer_ip = NULL;
    int tcp_port = TCP_PORT;
    const char *msg = "hello world!";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) dev = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) gid_index = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) peer_ip = argv[++i];
        else if (!strcmp(argv[i], "-P") && i + 1 < argc) tcp_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) msg = argv[++i];
        else {
            fprintf(stderr,
                    "usage: %s -d <dev> -p <port> -g <gid_index> "
                    "-c <peer_ip> -P <tcp_port> [-m <msg>]\n",
                    argv[0]);
            return 1;
        }
    }
    if (!peer_ip) { fprintf(stderr, "missing peer ip\n"); return 1; }

    struct ctx *c = setup(dev, port, gid_index);

    int sock = tcp_client(peer_ip, tcp_port);
    exch(sock, &c->local, &c->remote);
    close(sock);

    printf("[RC_write] remote buf addr=0x%lx rkey=0x%x\n",
           (unsigned long)c->remote.addr, c->remote.rkey);

    connect_qp(c, port, gid_index);

    size_t msg_len = strlen(msg);
    if (msg_len >= MSG_MAX) msg_len = MSG_MAX - 1;
    memcpy(c->buf, msg, msg_len);
    c->buf[msg_len] = '\0';

    struct ibv_sge sge = {
        .addr = (uint64_t)c->buf,
        .length = msg_len + 1,
        .lkey = c->mr->lkey,
    };

    struct ibv_send_wr wr = {
        .wr_id = 2,
        .opcode = IBV_WR_RDMA_WRITE,
        .num_sge = 1,
        .sg_list = &sge,
        .send_flags = IBV_SEND_SIGNALED,
        .wr.rdma = {
            .remote_addr = c->remote.addr,
            .rkey = c->remote.rkey,
        },
    };

    if (ibv_post_send(c->qp, &wr, NULL)) {
        perror("ibv_post_send");
        exit(1);
    }

    poll_cq(c->cq);
    printf("[RC_write] wrote to remote: %s\n", c->buf);

    cleanup(c);
    return 0;
}
