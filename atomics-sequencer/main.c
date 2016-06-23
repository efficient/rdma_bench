#include "hrd.h"
#include "main.h"
#include <getopt.h>
#include <byteswap.h>

/*
 * QP naming:
 * 1. server-i-j is the jth QP on port i of the server.
 * 2. client-i-j is the jth QP of client thread i in the system.
 */

/* A single server thread creates all QPs */
void *run_server(void *arg)
{
	int i, j;
	int total_srv_qps = NUM_CLIENTS * QPS_PER_CLIENT;

	struct thread_params params = *(struct thread_params *) arg;
	int srv_gid = params.id;	/* Global ID of this server thread */
	int num_server_ports = params.num_server_ports;

	/*
	 * Create one control block per port. Each control block has enough QPs
	 * for all client QPs. The ith client QP must connect to the ith QP in a
	 * a control block, but clients can choose the control block.
	 */
	struct hrd_ctrl_blk **cb = malloc(num_server_ports * sizeof(void *));
	assert(cb != NULL);

	/* Allocate a registered buffer for port #0 only */
	for(i = 0; i < num_server_ports; i++) {
		volatile void *prealloc_buf = (i == 0 ? NULL : cb[0]->conn_buf);
		int ib_port_index = params.base_port_index + i;
		int shm_key = (i == 0 ? SERVER_SHM_KEY : -1);

		cb[i] = hrd_ctrl_blk_init(srv_gid + i,	/* local hid */
			ib_port_index, 0, /* port index, numa node id */
			total_srv_qps, 0,	/* #conn qps, uc */
			prealloc_buf, BUF_SIZE, shm_key,
			0, 0, -1);	/* #dgram qps, buf size, shm key */

		/* Register all created QPs - only some will get used! */
		for(j = 0; j < total_srv_qps; j++) {
			char srv_name[HRD_QP_NAME_SIZE];
			sprintf(srv_name, "server-%d-%d", i, j);
			hrd_publish_conn_qp(cb[i], j, srv_name);
		}

		printf("main: Server published all QPs on port %d\n", ib_port_index);
	}

	hrd_red_printf("main: Server published all QPs. Waiting for clients..\n");

	for(i = 0; i < NUM_CLIENTS; i++) {
		for(j = 0; j < QPS_PER_CLIENT; j++) {
			/* Iterate over all client QPs */
			char clt_name[HRD_QP_NAME_SIZE];
			sprintf(clt_name, "client-%d-%d", i, j);

			struct hrd_qp_attr *clt_qp = NULL;
			while(clt_qp == NULL) {
				clt_qp = hrd_get_published_qp(clt_name);
				if(clt_qp == NULL) {
					usleep(200000);
				}
			}

			/* Calculate the control block and QP to use for this client */
			int cb_i = i % num_server_ports;
			int qp_i = (i * QPS_PER_CLIENT) + j;

			printf("main: Connecting client %d's QP %d --- port %d QP %d.\n",
				i, j, cb_i, qp_i);
			hrd_connect_qp(cb[cb_i], qp_i, clt_qp);

			char srv_name[HRD_QP_NAME_SIZE];
			sprintf(srv_name, "server-%d-%d", cb_i, qp_i);

			hrd_publish_ready(srv_name);
		}
	}

	volatile long long *counter = (volatile long long *) cb[0]->conn_buf;

	/* Repetedly print the counter */
	while(1) {
		printf("main: Counter = %lld\n", *counter);
		sleep(1);
	}

	return NULL;
}

void *run_client(void *arg)
{
	int i;

	struct thread_params params = *(struct thread_params *) arg;
	int clt_gid = params.id;	/* Global ID of this client thread */
	int num_client_ports = params.num_client_ports;
	int num_server_ports = params.num_server_ports;

	int ib_port_index = params.base_port_index + clt_gid % num_client_ports;

	/*
 	 * Don't use BUF_SIZE at client - it can be large and libhrd zeros it out,
	 * which can take time. We don't need a large buffer at clients.
	 */
	struct hrd_ctrl_blk *cb = hrd_ctrl_blk_init(clt_gid,	/* local_hid */
		ib_port_index, -1, /* port_index, numa_node_id */
		QPS_PER_CLIENT, 0,	/* conn qps, use_uc */
		NULL, 4096, -1, /* prealloc buf, size, key */
		0, 0, -1);	/* #dgram qps, buf size, shm key */

	memset((void *) cb->conn_buf, 0, 4096);

	struct hrd_qp_attr *srv_qp[QPS_PER_CLIENT];

	for(i = 0; i < QPS_PER_CLIENT; i++) {
		/* Compute the server port (or control block) and QP to use */
		int srv_cb_i = clt_gid % num_server_ports;
		int srv_qp_i = (clt_gid * QPS_PER_CLIENT) + i;
		char srv_name[HRD_QP_NAME_SIZE];
		sprintf(srv_name, "server-%d-%d", srv_cb_i, srv_qp_i);

		char clt_name[HRD_QP_NAME_SIZE];
		sprintf(clt_name, "client-%d-%d", clt_gid, i);

		/* Publish and connect the ith QP */
		hrd_publish_conn_qp(cb, i, clt_name);
		printf("main: Published client %d's QP %d. Waiting for server %s\n",
			clt_gid, i, srv_name);
	
		srv_qp[i] = NULL;
		while(srv_qp[i] == NULL) {
			srv_qp[i] = hrd_get_published_qp(srv_name);
			if(srv_qp[i] == NULL) {
				usleep(200000);
			}
		}

		printf("main: Client %d found server! Connecting..\n", clt_gid);
		hrd_connect_qp(cb, i, srv_qp[i]);
		hrd_wait_till_ready(srv_name);
	}

	/* Datapath */
	struct ibv_send_wr wr[MAX_POSTLIST], *bad_send_wr;
	struct ibv_sge sgl[MAX_POSTLIST];
	struct ibv_wc wc;
	int w_i = 0;	/* Window index */
	long long nb_tx[QPS_PER_CLIENT] = {0};	/* For selective signaling */
	int ret;

	long long rolling_iter = 0;	/* For performance measurement */
	long long num_reads = 0;	/* Number of RDMA reads issued */
	long long num_atomics = 0;	/* Number of atomics issued */

	int qp_i = 0;	/* Queue pair to use for this postlist */

	struct timespec start, end;
	clock_gettime(CLOCK_REALTIME, &start);

	/* Make this client's counter requests reasonably independent of others */
	uint64_t seed = 0xdeadbeef;
	for(i = 0; i < clt_gid * M_128; i++) {
		hrd_fastrand(&seed);
	}

	/* cb->conn_buf is 8-byte aligned even if hugepages are not used */
	long long *counter = (long long *) cb->conn_buf;

	while(1) {
		if(rolling_iter >= M_1) {
			clock_gettime(CLOCK_REALTIME, &end);
			double seconds = (end.tv_sec - start.tv_sec) + 
				(double) (end.tv_nsec - start.tv_nsec) / 1000000000;

			/* Model DrTM tput based on READ and ATOMIC tput */
			long long num_gets = num_reads;
			long long num_puts = num_atomics / 2;

#if HRD_CONNECT_IB_ATOMICS == 1
			printf("main: Client %d: %.2f GETs/s, %.2f PUTs/s, %.2f ops/s "
				"(%.2f atomics/s). Ctr = %lld\n",
				clt_gid, num_gets / seconds, num_puts / seconds,
				(num_gets + num_puts) / seconds,
				num_atomics / seconds,
				(long long) bswap_64(*counter));
#else
			printf("main: Client %d: %.2f GETs/s, %.2f PUTs/s, %.2f ops/s "
				"(%.2f atomics/s). Ctr = %lld\n",
				clt_gid, num_gets / seconds, num_puts / seconds,
				(num_gets + num_puts) / seconds,
				num_atomics / seconds,
				*counter);
#endif
			/* Reset counters */
			rolling_iter = 0;
			num_reads = 0;
			num_atomics = 0;

			clock_gettime(CLOCK_REALTIME, &start);
		}

		/* Post a postlist of work requests in a single ibv_post_send() */
		for(w_i = 0; w_i < params.postlist; w_i++) {
			int use_atomic = drtm_use_atomic(hrd_fastrand(&seed));
			if(use_atomic) {
				/*
				 * We should use compare-and-swap here for DrTM, but it makes no
				 * difference for performance (I checked). Using fetch-and-add
				 * allows us to use the same code for both DrTM and array of
				 * counters.
				 */
				wr[w_i].opcode = IBV_WR_ATOMIC_FETCH_AND_ADD;
				num_atomics++;
			} else {
				wr[w_i].opcode = IBV_WR_RDMA_READ;
				num_reads++;
			}

			wr[w_i].num_sge = 1;
			wr[w_i].next = (w_i == params.postlist - 1) ? NULL : &wr[w_i + 1];
			wr[w_i].sg_list = &sgl[w_i];

			wr[w_i].send_flags = (nb_tx[qp_i] & UNSIG_BATCH_) == 0 ?
				IBV_SEND_SIGNALED : 0;
			if((nb_tx[qp_i] & UNSIG_BATCH_) == 0 && nb_tx[qp_i] > 0) {
				hrd_poll_cq(cb->conn_cq[qp_i], 1, &wc);
			}

			sgl[w_i].addr = (uint64_t) (uintptr_t) cb->conn_buf;
			sgl[w_i].length = 8;	/* Only 8 bytes get written */
			sgl[w_i].lkey = cb->conn_buf_mr->lkey;

			int index = 0;
			if(USE_RANDOM == 1) {
				index = hrd_fastrand(&seed) % (BUF_SIZE / STRIDE_SIZE);
			}

			uint64_t remote_address = srv_qp[qp_i]->buf_addr +
				index * STRIDE_SIZE;
			assert(remote_address % STRIDE_SIZE == 0);

			if(use_atomic) {
				if(EMULATE_DRTM == 1) {
					/*
					 * With 16B keys, DrTM's atomic field is at offset 24B. This
					 * shouldn't really matter for performance...
					 */
					remote_address += 24;
				}
				wr[w_i].wr.atomic.remote_addr = remote_address;
				wr[w_i].wr.atomic.rkey = srv_qp[qp_i]->rkey;
				wr[w_i].wr.atomic.compare_add = 1ULL;
			} else {
				/* We shouldn't do READs for array of counters */
				assert(EMULATE_DRTM == 1);
				sgl[w_i].length = 64;	/* We're emulating 16B keys, 32B vals */
				wr[w_i].wr.rdma.remote_addr = remote_address;
				wr[w_i].wr.rdma.rkey = srv_qp[qp_i]->rkey;
			}

			nb_tx[qp_i]++;
		}
		
		ret = ibv_post_send(cb->conn_qp[qp_i], &wr[0], &bad_send_wr);
		CPE(ret, "ibv_post_send error", ret);

		HRD_MOD_ADD(qp_i, QPS_PER_CLIENT);
		rolling_iter += params.postlist;
	}

	return NULL;
}

int main(int argc, char *argv[])
{
	/* Check some macros  */
	if(EMULATE_DRTM == 1) {
		assert(USE_RANDOM == 1);
		assert(BUF_SIZE >= 128 * 1024 * 1024);	/* Spill to DRAM */
		assert(STRIDE_SIZE == 64);	/* Restrict to 16B keys and 32B values */
		assert(UPDATE_PERCENTAGE >= 0 && UPDATE_PERCENTAGE <= 100);
	} else {
		assert(BUF_SIZE <= 16 * 1024 * 1024);	/* Fit in L3 cache */
		assert(STRIDE_SIZE == 8);	/* If not DrTM, then array of counters */
		assert(UPDATE_PERCENTAGE == 100);	/* Counters are only updated */
	}

	int i, c;
	int num_threads = -1;
	int is_client = -1, machine_id = -1, postlist = -1;
	int base_port_index = -1, num_server_ports = -1, num_client_ports = -1;
	int appnet = -1;
	struct thread_params *param_arr;
	pthread_t *thread_arr;

	static struct option opts[] = {
		{ .name = "num-threads",    	.has_arg = 1, .val = 't' },
		{ .name = "base-port-index",	.has_arg = 1, .val = 'b' },
		{ .name = "num-server-ports",	.has_arg = 1, .val = 'N' },
		{ .name = "num-client-ports",	.has_arg = 1, .val = 'n' },
		{ .name = "num-ports",      	.has_arg = 1, .val = 'n' },
		{ .name = "appnet",				.has_arg = 1, .val = 'a' },
		{ .name = "is-client",      	.has_arg = 1, .val = 'c' },
		{ .name = "machine-id",     	.has_arg = 1, .val = 'm' },
		{ .name = "postlist",       	.has_arg = 1, .val = 'p' },
		{ 0 }
	};

	/* Parse and check arguments */
	while(1) {
		c = getopt_long(argc, argv, "t:b:N:n:a:c:m:p", opts, NULL);
		if(c == -1) {
			break;
		}
		switch (c) {
			case 't':
				num_threads = atoi(optarg);
				break;
			case 'b':
				base_port_index = atoi(optarg);
				break;
			case 'N':
				num_server_ports = atoi(optarg);
				break;
			case 'n':
				num_client_ports = atoi(optarg);
				break;
			case 'a':
				appnet = atoi(optarg);
				break;
			case 'c':
				is_client = atoi(optarg);
				break;
			case 'm':
				machine_id = atoi(optarg);
				break;
			case 'p':
				postlist = atoi(optarg);
				break;
			default:
				printf("Invalid argument %d\n", c);
				assert(false);
		}
	}

	assert(base_port_index >= 0 && base_port_index <= 8);
	assert(appnet == 0 || appnet == 1);
	assert(is_client == 0 || is_client == 1);

	if(is_client == 1) {
		assert(num_server_ports >= 1 && num_server_ports <= 8);
		assert(num_client_ports >= 1 && num_client_ports <= 8);
		assert(num_threads >= 1);
		assert(machine_id >= 0);
		assert(postlist >= 1 && postlist <= MAX_POSTLIST);

		assert(UNSIG_BATCH >= postlist); /* Postlist check */
		assert(HRD_Q_DEPTH >= 2 * UNSIG_BATCH);	/* Queue capacity check */
	} else {
		/* Server does not need to know number of client ports */
		assert(num_server_ports >= 1 && num_server_ports <= 8);
		assert(num_threads == -1);	/* Must use 1 server thread */
		num_threads = 1;	/* Needed to allocate thread structs later */

		assert(machine_id == -1);	/* We use only 1 server */
		assert(postlist == -1);	/* Server does not do post_send() */
	}

	/* Launch a single server thread or multiple client threads */
	printf("main: Using %d threads\n", num_threads);
	param_arr = malloc(num_threads * sizeof(struct thread_params));
	thread_arr = malloc(num_threads * sizeof(pthread_t));

	if(is_client == 1) {
		for(i = 0; i < num_threads; i++) {
			param_arr[i].id = (machine_id * num_threads) + i;
			param_arr[i].num_threads = num_threads;
			param_arr[i].base_port_index = base_port_index;
			param_arr[i].num_server_ports = num_server_ports;
			param_arr[i].num_client_ports = num_client_ports;
			param_arr[i].appnet = appnet;
			param_arr[i].postlist = postlist;

			pthread_create(&thread_arr[i], NULL, run_client, &param_arr[i]);
		}
	} else {
		/* Only a single server thread */
		param_arr[0].id = 0;
		param_arr[0].base_port_index = base_port_index;
		param_arr[0].num_server_ports = num_server_ports;
		param_arr[0].num_client_ports = num_client_ports;
		param_arr[0].appnet = appnet;
		pthread_create(&thread_arr[0], NULL, run_server, &param_arr[0]);
	}

	for(i = 0; i < num_threads; i++) {
		pthread_join(thread_arr[i], NULL);
	}

	return 0;
}
