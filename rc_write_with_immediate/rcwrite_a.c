#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <endian.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>

#define TCP_PORT   12345
#define IB_PORT    1
#define GID_INDEX  3

struct info {
	uint32_t qp_num;
	uint32_t psn;
	uint32_t rkey;
	uint16_t lid;
	uint64_t addr;
	union ibv_gid gid;
};

int main(int argc, char *argv[])
{
	int dev_idx = (argc > 1) ? atoi(argv[1]) : 0;

	int ls = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in la = {
		.sin_family = AF_INET,
		.sin_port = htons(TCP_PORT),
		.sin_addr.s_addr = INADDR_ANY,
	};
	bind(ls, (struct sockaddr *)&la, sizeof(la));
	listen(ls, 1);
	int sock = accept(ls, NULL, NULL);

	struct ibv_device **devs = ibv_get_device_list(NULL);
	struct ibv_context *ctx = ibv_open_device(devs[dev_idx]);
	struct ibv_pd *pd = ibv_alloc_pd(ctx);
	struct ibv_cq *cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);

	char buf[256];
	struct ibv_mr *mr = ibv_reg_mr(pd, buf, sizeof(buf),
			IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);

	struct ibv_qp_init_attr qpia = {
		.send_cq = cq,
		.recv_cq = cq,
		.cap = {
			.max_send_wr = 10,
			.max_recv_wr = 10,
			.max_send_sge = 1,
			.max_recv_sge = 1,
		},
		.qp_type = IBV_QPT_RC,
	};
	struct ibv_qp *qp = ibv_create_qp(pd, &qpia);

	uint32_t psn = 0x12345678;
	union ibv_gid my_gid;
	ibv_query_gid(ctx, IB_PORT, GID_INDEX, &my_gid);

	struct info my = {
		.qp_num = htonl(qp->qp_num),
		.psn = htonl(psn),
		.rkey = htonl(mr->rkey),
		.lid = htons(0),
		.addr = htobe64((uint64_t)buf),
		.gid = my_gid,
	};
	struct info peer;

	int n = 0;
	while (n < (int)sizeof(peer))
		n += read(sock, ((char *)&peer) + n, sizeof(peer) - n);
	peer.qp_num = ntohl(peer.qp_num);
	peer.psn = ntohl(peer.psn);
	peer.rkey = ntohl(peer.rkey);
	peer.lid = ntohs(peer.lid);
	peer.addr = be64toh(peer.addr);

	n = 0;
	while (n < (int)sizeof(my))
		n += write(sock, ((char *)&my) + n, sizeof(my) - n);

	struct ibv_qp_attr attr = {0};

	attr.qp_state = IBV_QPS_INIT;
	attr.pkey_index = 0;
	attr.port_num = IB_PORT;
	attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE;
	ibv_modify_qp(qp, &attr,
		IBV_QP_STATE | IBV_QP_PKEY_INDEX |
		IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_RTR;
	attr.path_mtu = IBV_MTU_1024;
	attr.dest_qp_num = peer.qp_num;
	attr.rq_psn = peer.psn;
	attr.max_dest_rd_atomic = 1;
	attr.min_rnr_timer = 12;
	attr.ah_attr.is_global = 1;
	attr.ah_attr.grh.dgid = peer.gid;
	attr.ah_attr.grh.sgid_index = GID_INDEX;
	attr.ah_attr.grh.hop_limit = 1;
	attr.ah_attr.dlid = 0;
	attr.ah_attr.sl = 0;
	attr.ah_attr.port_num = IB_PORT;
	ibv_modify_qp(qp, &attr,
		IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
		IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
		IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_RTS;
	attr.timeout = 14;
	attr.retry_cnt = 7;
	attr.rnr_retry = 7;
	attr.sq_psn = psn;
	attr.max_rd_atomic = 1;
	ibv_modify_qp(qp, &attr,
		IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
		IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);

	struct ibv_sge rsge = {
		.addr = (uint64_t)buf,
		.length = sizeof(buf),
		.lkey = mr->lkey,
	};
	struct ibv_recv_wr rwr = {
		.wr_id = 1,
		.sg_list = &rsge,
		.num_sge = 1,
	};
	struct ibv_recv_wr *bad_recv;
	ibv_post_recv(qp, &rwr, &bad_recv);

	char ready;
	n = 0;
	while (n < 1)
		n += read(sock, &ready, 1);

	char *msg = "Hello RDMA RC write world!";
	int len = strlen(msg) + 1;
	memcpy(buf, msg, len);

	struct ibv_sge ssge = {
		.addr = (uint64_t)buf,
		.length = len,
		.lkey = mr->lkey,
	};
	struct ibv_send_wr swr = {
		.wr_id = 2,
		.opcode = IBV_WR_RDMA_WRITE_WITH_IMM,
		.send_flags = IBV_SEND_SIGNALED,
		.imm_data = htonl(len),
		.wr.rdma.remote_addr = peer.addr,
		.wr.rdma.rkey = peer.rkey,
		.sg_list = &ssge,
		.num_sge = 1,
	};
	struct ibv_send_wr *bad_send;
	ibv_post_send(qp, &swr, &bad_send);

	struct ibv_wc wc;
	while (ibv_poll_cq(cq, 1, &wc) == 0) {}

	while (ibv_poll_cq(cq, 1, &wc) == 0) {}
	printf("received: %s\n", buf);

	return 0;
}
