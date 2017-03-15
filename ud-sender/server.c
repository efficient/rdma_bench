#include "main.h"
#include "hrd.h"

void *run_server(void *arg)
{
	int i;
	struct thread_params params = *(struct thread_params *) arg;
	int srv_gid = params.id;	/* Global ID of this server thread */
	int ib_port_index = params.dual_port == 0 ? 0 : srv_gid % 2;

	struct hrd_ctrl_blk *cb = hrd_ctrl_blk_init(srv_gid,	/* local_hid */
		ib_port_index, -1, /* port_index, numa_node_id */
		0, 0,	/* conn qps, use uc */
		NULL, 0, -1,	/* prealloc conn buf, conn buf size, key */
		NUM_UD_QPS, BUF_SIZE, -1);	/* num_dgram_qps, dgram_buf_size, key */

	/* Buffer to receive requests into */
	memset((void *) cb->dgram_buf, 0, BUF_SIZE);

	/* Buffer to send responses from */
	uint8_t *resp_buf = malloc(params.size);
	assert(resp_buf != 0);
	memset(resp_buf, 1, params.size);

	/* Create an address handle for each client */
	struct ibv_ah *ah[NUM_CLIENTS];
	memset(ah, 0, NUM_CLIENTS * sizeof(uintptr_t));

	struct hrd_qp_attr *clt_qp[NUM_CLIENTS];

	/*
	 * Connect this server to NUM_CLIENTS clients whose global IDs are the
	 * same as this server's modulo 2. This ensures that the connected
	 * clients are on the same port as the server.
	 */
	for(i = 0; i < NUM_CLIENTS; i++) {
		char clt_name[HRD_QP_NAME_SIZE];
	
		/* ah[i] maps to client clt_id */
		int clt_id = params.dual_port == 0 ? i : 2 * i + (srv_gid % 2);
		sprintf(clt_name, "client-%d", clt_id);

		/* Get the UD queue pair for the ith client */
		clt_qp[i] = NULL;
		while(clt_qp[i] == NULL) {
			clt_qp[i] = hrd_get_published_qp(clt_name);
			if(clt_qp[i] == NULL) {
				usleep(200000);
			}
		}

		printf("main: Server %d got client %d (clt_id = %d) of %d clients.\n",
			srv_gid, i, clt_id, NUM_CLIENTS);
		
		struct ibv_ah_attr ah_attr = {
			.is_global = 0,
			.dlid = clt_qp[i]->lid,
			.sl = 0,
			.src_path_bits = 0,
			.port_num = cb->dev_port_id,
		};

		ah[i]= ibv_create_ah(cb->pd, &ah_attr);
		assert(ah[i] != NULL);
	}
	
	struct ibv_send_wr wr[MAX_POSTLIST], *bad_send_wr;
	struct ibv_wc wc[MAX_POSTLIST];
	struct ibv_sge sgl[MAX_POSTLIST];
	long long rolling_iter = 0;	/* For throughput measurement */
	long long nb_tx[NUM_UD_QPS] = {0};	/* For selective signaling */
	int ud_qp_i = 0;	/* Round-robin between QPs across postlists */
	int w_i = 0;	/* Window index */
	int ret;

	struct timespec start, end;
	clock_gettime(CLOCK_REALTIME, &start);

	while(1) {
		if(rolling_iter >= M_4) {
			clock_gettime(CLOCK_REALTIME, &end);
			double seconds = (end.tv_sec - start.tv_sec) + 
				(double) (end.tv_nsec - start.tv_nsec) / 1000000000;
			double my_tput = M_4 / seconds;
			printf("main: Server %d: %.2f IOPS. \n", srv_gid, my_tput);
			params.tput[srv_gid] = my_tput;
			if(srv_gid == 0) {
				double total_tput = 0;
				for(i = 0; i < params.num_threads; i++) {
					total_tput += params.tput[i];
				}
				hrd_red_printf("main: Total tput = %.2f IOPS.\n", total_tput);
			}

			rolling_iter = 0;
		
			clock_gettime(CLOCK_REALTIME, &start);
		}

		for(w_i = 0; w_i < params.postlist; w_i++) {
			int cn = nb_tx[ud_qp_i] & NUM_CLIENTS_;

			wr[w_i].wr.ud.ah = ah[cn];
			wr[w_i].wr.ud.remote_qpn = clt_qp[cn]->qpn;
			wr[w_i].wr.ud.remote_qkey = HRD_DEFAULT_QKEY;

			wr[w_i].opcode = IBV_WR_SEND;
			wr[w_i].num_sge = 1;
			wr[w_i].next = (w_i == params.postlist - 1) ? NULL : &wr[w_i + 1];
			wr[w_i].sg_list = &sgl[w_i];

			wr[w_i].send_flags = ((nb_tx[ud_qp_i] & UNSIG_BATCH_) == 0) ?
				IBV_SEND_SIGNALED : 0;
			if((nb_tx[ud_qp_i] & UNSIG_BATCH_) == 0 && nb_tx[ud_qp_i] > 0) {
				hrd_poll_cq(cb->dgram_send_cq[ud_qp_i], 1, wc);
			}

			wr[w_i].send_flags |= IBV_SEND_INLINE;

			sgl[w_i].addr = (uint64_t) (uintptr_t) resp_buf;
			sgl[w_i].length = params.size;

			nb_tx[ud_qp_i]++;
			rolling_iter++;
		}

		ret = ibv_post_send(cb->dgram_qp[ud_qp_i], &wr[0], &bad_send_wr);
		CPE(ret, "ibv_post_send error", ret);

		/* Use a different QP for the next postlist */
		ud_qp_i++;
		if(ud_qp_i == NUM_UD_QPS) {
			ud_qp_i = 0;
		}
	}

	return NULL;
}
