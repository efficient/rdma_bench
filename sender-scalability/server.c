#include "hrd.h"
#include "main.h"

void* run_server(void* arg) {
  int i, ret;
  struct thread_params params = *(struct thread_params*)arg;
  int srv_gid = params.id; /* Global ID of this server thread */

  int ib_port_index = params.dual_port == 0 ? 0 : srv_gid % 2;
  int shm_key = BASE_SHM_KEY + srv_gid;

  /* All the buffers sent or received should fit in @conn_buf */
  assert(params.size * WINDOW_SIZE < BUF_SIZE);

  struct hrd_ctrl_blk* cb = hrd_ctrl_blk_init(
      srv_gid,                           /* local_hid */
      ib_port_index, 0,                  /* port_index, numa_node_id */
      NUM_CLIENT_THREADS, params.use_uc, /* conn qps, uc */
      NULL, BUF_SIZE, shm_key,           /* prealloc conn buf size, key */
      0, 0, -1);                         /* #dgram qps, buf size, shm key */

  /* Set the buffer to 0 so that we can detect READ completion by polling. */
  memset((void*)cb->conn_buf, 0, BUF_SIZE);

  for (i = 0; i < NUM_CLIENT_THREADS; i++) {
    /* In dual-port mode, connect to clients on the same port only */
    if (params.dual_port == 1 && (i % 2 != srv_gid % 2)) {
      continue;
    }

    char srv_qp_name[HRD_QP_NAME_SIZE];
    sprintf(srv_qp_name, "server-%d-%d", srv_gid, i);

    hrd_publish_conn_qp(cb, i, srv_qp_name);
    printf("main: Server %d published qp for client %d\n", srv_gid, i);
  }

  struct hrd_qp_attr* clt_qp[NUM_CLIENT_THREADS];

  for (i = 0; i < NUM_CLIENT_THREADS; i++) {
    if (params.dual_port == 1 && (i % 2 != srv_gid % 2)) {
      continue;
    }

    char clt_qp_name[HRD_QP_NAME_SIZE];
    sprintf(clt_qp_name, "client-%d-%d", i, srv_gid);

    clt_qp[i] = NULL;
    while (clt_qp[i] == NULL) {
      clt_qp[i] = hrd_get_published_qp(clt_qp_name);
      if (clt_qp[i] == NULL) {
        usleep(20000);
      }
    }

    printf("main: Server %d found client %d! Connecting..\n", srv_gid, i);
    hrd_connect_qp(cb, i, clt_qp[i]);
    hrd_wait_till_ready(clt_qp_name);
  }

  printf("main: Server %d ready\n", srv_gid);

  struct ibv_send_wr wr, *bad_send_wr;
  struct ibv_sge sgl;
  struct ibv_wc wc;
  long long rolling_iter = 0;                /* For performance measurement */
  long long nb_tx[NUM_CLIENT_THREADS] = {0}; /* Per-QP signaling */
  long long nb_tx_tot = 0; /* For windowing (for READs only) */

  struct timespec run_start, run_end;
  struct timespec msr_start, msr_end;
  clock_gettime(CLOCK_REALTIME, &run_start);
  clock_gettime(CLOCK_REALTIME, &msr_start);

  int opcode = params.do_read == 0 ? IBV_WR_RDMA_WRITE : IBV_WR_RDMA_READ;

  int cn = 0;
  uint64_t seed = 0xdeadbeef;

  while (1) {
    if (rolling_iter >= M_4) {
      clock_gettime(CLOCK_REALTIME, &msr_end);
      double msr_seconds =
          (msr_end.tv_sec - msr_start.tv_sec) +
          (double)(msr_end.tv_nsec - msr_start.tv_nsec) / 1000000000;
      double tput = rolling_iter / msr_seconds;

      clock_gettime(CLOCK_REALTIME, &run_end);
      double run_seconds =
          (run_end.tv_sec - run_start.tv_sec) +
          (double)(run_end.tv_nsec - run_start.tv_nsec) / 1000000000;
      if (run_seconds >= params.run_time) {
        printf("main: Server %d exiting.\n", srv_gid);
        hrd_ctrl_blk_destroy(cb);
        return NULL;
      }

      printf(
          "main: Server %d: %.2f ops. Total active QPs = %d. "
          "Outstanding ops per thread (for READs) = %d. "
          "Seconds = %.1f of %d.\n",
          srv_gid, tput, (NUM_SERVER_THREADS * NUM_CLIENT_THREADS) / 2,
          WINDOW_SIZE, run_seconds, params.run_time);

      params.tput[srv_gid] = tput;
      if (srv_gid == 0) {
        double total_tput = 0;
        for (i = 0; i < NUM_SERVER_THREADS; i++) {
          total_tput += params.tput[i];
        }
        hrd_red_printf("Total tput = %.2f ops\n", total_tput);
      }

      rolling_iter = 0;
      clock_gettime(CLOCK_REALTIME, &msr_start);
    }

    int window_i = nb_tx_tot % WINDOW_SIZE; /* Current window slot to use */

    /* For READs, restrict outstanding ops per-thread to WINDOW_SIZE */
    if (opcode == IBV_WR_RDMA_READ && nb_tx_tot >= WINDOW_SIZE) {
      while (cb->conn_buf[window_i * params.size] == 0) {
        /* Wait for a window slow to open up */
      }

      /* Zero-out the slot for this round */
      cb->conn_buf[window_i * params.size] = 0;
    }

    /* Choose the next client to send a packet to */
    cn = hrd_fastrand(&seed) % NUM_CLIENT_THREADS;
    if (params.dual_port == 1) {
      /* In dual-port mode, send requests to clients on the same port */
      while (cn % 2 != srv_gid % 2) {
        cn = hrd_fastrand(&seed) % NUM_CLIENT_THREADS;
      }
    }

    wr.opcode = opcode;
    wr.num_sge = 1;
    wr.next = NULL;
    wr.sg_list = &sgl;

    wr.send_flags = (nb_tx[cn] & UNSIG_BATCH_) == 0 ? IBV_SEND_SIGNALED : 0;
    if ((nb_tx[cn] & UNSIG_BATCH_) == 0 && nb_tx[cn] > 0) {
      /* This can happen if a client dies before the server */
      int ret = hrd_poll_cq_ret(cb->conn_cq[cn], 1, &wc);
      if (ret == -1) {
        hrd_ctrl_blk_destroy(cb);
        return NULL;
      }
    }

    wr.send_flags |= (params.do_read == 0) ? IBV_SEND_INLINE : 0;

    sgl.addr = (uint64_t)(uintptr_t)&cb->conn_buf[window_i * params.size];
    sgl.length = params.size;
    sgl.lkey = cb->conn_buf_mr->lkey;

    int remote_offset = hrd_fastrand(&seed) % (BUF_SIZE - params.size);
    wr.wr.rdma.remote_addr = clt_qp[cn]->buf_addr + remote_offset;
    wr.wr.rdma.rkey = clt_qp[cn]->rkey;

    nb_tx[cn]++;

    ret = ibv_post_send(cb->conn_qp[cn], &wr, &bad_send_wr);
    CPE(ret, "ibv_post_send error", ret);

    rolling_iter++;
  }

  return NULL;
}
