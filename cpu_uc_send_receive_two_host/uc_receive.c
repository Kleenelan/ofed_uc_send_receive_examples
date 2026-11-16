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

static void read_all(int s, void *buf, size_t n) {
    char *p = buf;
    while (n) {
        ssize_t r = read(s, p, n);
        if (r <= 0) { perror("read"); exit(1); }
        p += r; n -= r;
    }
}

static void write_all(int s, const void *buf, size_t n) {
    const char *p = buf;
    while (n) {
        ssize_t w = write(s, p, n);
        if (w <= 0) { perror("write"); exit(1); }
        p += w; n -= w;
    }
}

static int tcp_server(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0), c;
    int on = 1;
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(TCP_PORT), .sin_addr.s_addr = INADDR_ANY };
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(s, 1) < 0) {
        perror("bind/listen");
        exit(1);
    }
    c = accept(s, NULL, NULL);
    close(s);
    return c;
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

static void exch(int sock, struct qp_info *local, struct qp_info *remote) {
    char tx[24], rx[24];
    wire_qp_info(local, tx);
    write_all(sock, tx, 24);
    read_all(sock, rx, 24);
    unwir_qp_info(rx, remote);
}

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
    ibv_query_gid(c->ctx, port, gid_index, &gid);
    memcpy(c->local.gid, &gid, 16);

    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = port,
        .qp_access_flags = 0,
    };
    ibv_modify_qp(c->qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);

    struct ibv_sge sge = { .addr = (uint64_t)c->buf, .length = MSG_MAX, .lkey = c->mr->lkey };
    struct ibv_recv_wr wr = { .wr_id = 1, .num_sge = 1, .sg_list = &sge };
    ibv_post_recv(c->qp, &wr, NULL);
    return c;
}

static void connect_qp(struct ctx *c, int port, int sgid_index) {
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = IBV_MTU_1024,
        .dest_qp_num = c->remote.qpn,
        .rq_psn = c->remote.psn,
        .ah_attr = {
            .is_global = 1, .port_num = port, .sl = 0,
            .grh = { .dgid = *(union ibv_gid *)c->remote.gid, .sgid_index = sgid_index, .hop_limit = 255 },
        },
    };
    ibv_modify_qp(c->qp, &attr, IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_AV);

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn = c->local.psn;
    ibv_modify_qp(c->qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN);
}

static void poll_cq(struct ibv_cq *cq) {
    struct ibv_wc wc;
    while (ibv_poll_cq(cq, 1, &wc) < 1) {}
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "wc error: %s\n", ibv_wc_status_str(wc.status));
        exit(1);
    }
}

static void cleanup(struct ctx *c) {
    ibv_destroy_qp(c->qp);
    ibv_dereg_mr(c->mr);
    ibv_destroy_cq(c->cq);
    ibv_dealloc_pd(c->pd);
    ibv_close_device(c->ctx);
    free(c);
}

int main(int argc, char **argv) {
    const char *dev = "mlx5_1";
    int port = 1;
    int gid_index = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) dev = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) gid_index = atoi(argv[++i]);
        else { fprintf(stderr, "usage: %s -d <dev> -p <port> -g <gid_index>\n", argv[0]); return 1; }
    }

    struct ctx *c = setup(dev, port, gid_index);
    int sock = tcp_server();
    exch(sock, &c->local, &c->remote);
    close(sock);
    connect_qp(c, port, gid_index);

    poll_cq(c->cq);
    printf("received: %s\n", c->buf);

    cleanup(c);
    return 0;
}
