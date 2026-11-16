#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <infiniband/verbs.h>

#define TCP_PORT 12345
#define MSG_MAX 256

struct qp_info {
    uint32_t qpn;
    uint32_t psn;
    uint8_t  gid[16];
};

struct ctx {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    char buf[MSG_MAX];
    struct qp_info local;
    struct qp_info remote;
};

static int tcp_server(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0), c;
    int on = 1;
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(TCP_PORT), .sin_addr.s_addr = INADDR_ANY };
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    bind(s, (struct sockaddr *)&addr, sizeof(addr));
    listen(s, 1);
    c = accept(s, NULL, NULL);
    close(s);
    return c;
}

static int tcp_client(const char *ip) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(TCP_PORT) };
    inet_pton(AF_INET, ip, &addr.sin_addr);
    connect(s, (struct sockaddr *)&addr, sizeof(addr));
    return s;
}

static void wire_qp_info(struct qp_info *info, char *out) {
    *(uint32_t *)out = htonl(info->qpn);
    *(uint32_t *)(out + 4) = htonl(info->psn);
    memcpy(out + 8, info->gid, 16);
}

static void unwir_qp_info(char *in, struct qp_info *info) {
    info->qpn = ntohl(*(uint32_t *)in);
    info->psn = ntohl(*(uint32_t *)(in + 4));
    memcpy(info->gid, in + 8, 16);
}

static int exch(int sock, struct qp_info *local, struct qp_info *remote, int server) {
    char tx[24], rx[24];
    wire_qp_info(local, tx);
    if (server) {
        write(sock, tx, 24);
        read(sock, rx, 24);
    } else {
        read(sock, rx, 24);
        write(sock, tx, 24);
    }
    unwir_qp_info(rx, remote);
    return 0;
}

static struct ctx *setup(const char *dev, int port, int is_server) {
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
    c->mr = ibv_reg_mr(c->pd, c->buf, MSG_MAX, IBV_ACCESS_LOCAL_WRITE);

    struct ibv_qp_init_attr qpia = {
        .send_cq = c->cq, .recv_cq = c->cq,
        .cap = { .max_send_wr = 4, .max_recv_wr = 4, .max_send_sge = 1, .max_recv_sge = 1 },
        .qp_type = IBV_QPT_UC,
    };
    c->qp = ibv_create_qp(c->pd, &qpia);
    c->local.qpn = c->qp->qp_num;
    c->local.psn = rand() & 0xffffff;

    union ibv_gid gid;
    ibv_query_gid(c->ctx, port, 0, &gid);
    memcpy(c->local.gid, &gid, 16);

    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = port,
        .qp_access_flags = 0,
    };
    ibv_modify_qp(c->qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);

    if (is_server) {
        struct ibv_sge sge = { .addr = (uint64_t)c->buf, .length = MSG_MAX, .lkey = c->mr->lkey };
        struct ibv_recv_wr wr = { .wr_id = 1, .num_sge = 1, .sg_list = &sge };
        ibv_post_recv(c->qp, &wr, NULL);
    }
    return c;
}

static void connect_qp(struct ctx *c, int port) {
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = IBV_MTU_1024,
        .dest_qp_num = c->remote.qpn,
        .rq_psn = c->remote.psn,
        .ah_attr = {
            .is_global = 1, .port_num = port, .sl = 0,
            .grh = { .dgid = *(union ibv_gid *)c->remote.gid, .sgid_index = 0, .hop_limit = 255 },
        },
    };
    ibv_modify_qp(c->qp, &attr, IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_AV);

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn = c->local.psn;
    ibv_modify_qp(c->qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN);
}

static void poll_cq(struct ibv_cq *cq, int expect) {
    struct ibv_wc wc;
    while (ibv_poll_cq(cq, 1, &wc) < 1) {}
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "wc error: %s\n", ibv_wc_status_str(wc.status));
        exit(1);
    }
}

int main(int argc, char **argv) {
    const char *dev = "mlx5_0";
    int port = 1, server = 0;
    const char *peer_ip = NULL;
    const char *msg = "hello world!";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) dev = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s")) server = 1;
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) { peer_ip = argv[++i]; server = 0; }
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) msg = argv[++i];
        else { fprintf(stderr, "usage: %s -d <dev> -p <port> -s | -c <peer_ip> [-m <msg>]\n", argv[0]); return 1; }
    }

    struct ctx *c = setup(dev, port, server);
    int sock = server ? tcp_server() : tcp_client(peer_ip);
    exch(sock, &c->local, &c->remote, server);
    close(sock);
    connect_qp(c, port);

    if (server) {
        poll_cq(c->cq, 1);
        printf("received: %s\n", c->buf);
    } else {
        strncpy(c->buf, msg, MSG_MAX - 1);
        struct ibv_sge sge = { .addr = (uint64_t)c->buf, .length = strlen(c->buf) + 1, .lkey = c->mr->lkey };
        struct ibv_send_wr wr = { .wr_id = 2, .opcode = IBV_WR_SEND, .num_sge = 1, .sg_list = &sge, .send_flags = IBV_SEND_SIGNALED };
        ibv_post_send(c->qp, &wr, NULL);
        poll_cq(c->cq, 1);
        printf("sent: %s\n", c->buf);
    }

    ibv_destroy_qp(c->qp);
    ibv_dereg_mr(c->mr);
    ibv_destroy_cq(c->cq);
    ibv_dealloc_pd(c->pd);
    ibv_close_device(c->ctx);
    free(c);
    return 0;
}
