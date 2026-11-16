#ifndef UC_COMMON_H
#define UC_COMMON_H

#include <infiniband/verbs.h>
#include <stdint.h>

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

int tcp_server(void);
int tcp_client(const char *ip);
void wire_qp_info(struct qp_info *info, char *out);
void unwir_qp_info(char *in, struct qp_info *info);
int exch(int sock, struct qp_info *local, struct qp_info *remote, int server);
struct ctx *setup(const char *dev, int port, int is_server);
void connect_qp(struct ctx *c, int port);
void poll_cq(struct ibv_cq *cq, int expect);
void cleanup(struct ctx *c);

#endif
