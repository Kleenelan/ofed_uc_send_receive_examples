#include "uc_common.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    const char *dev = "mlx5_0";
    int port = 1;
    const char *peer_ip = NULL;
    const char *msg = "hello world!";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) dev = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) peer_ip = argv[++i];
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) msg = argv[++i];
        else { fprintf(stderr, "usage: %s -d <dev> -p <port> -c <peer_ip> [-m <msg>]\n", argv[0]); return 1; }
    }
    if (!peer_ip) { fprintf(stderr, "missing peer ip\n"); return 1; }

    struct ctx *c = setup(dev, port, 0);
    int sock = tcp_client(peer_ip);
    exch(sock, &c->local, &c->remote, 0);
    close(sock);
    connect_qp(c, port);

    strncpy(c->buf, msg, MSG_MAX - 1);
    struct ibv_sge sge = { .addr = (uint64_t)c->buf, .length = strlen(c->buf) + 1, .lkey = c->mr->lkey };
    struct ibv_send_wr wr = { .wr_id = 2, .opcode = IBV_WR_SEND, .num_sge = 1, .sg_list = &sge, .send_flags = IBV_SEND_SIGNALED };
    ibv_post_send(c->qp, &wr, NULL);
    poll_cq(c->cq, 1);
    printf("sent: %s\n", c->buf);

    cleanup(c);
    return 0;
}
