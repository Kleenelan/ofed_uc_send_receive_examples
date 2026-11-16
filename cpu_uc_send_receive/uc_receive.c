#include "uc_common.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    const char *dev = "mlx5_1";
    int port = 1;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) dev = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) port = atoi(argv[++i]);
        else { fprintf(stderr, "usage: %s -d <dev> -p <port>\n", argv[0]); return 1; }
    }

    struct ctx *c = setup(dev, port, 1);
    int sock = tcp_server();
    exch(sock, &c->local, &c->remote, 1);
    close(sock);
    connect_qp(c, port);

    poll_cq(c->cq, 1);
    printf("received: %s\n", c->buf);

    cleanup(c);
    return 0;
}
