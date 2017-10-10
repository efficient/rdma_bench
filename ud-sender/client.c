#include "hrd.h"
#include "main.h"

void* run_client(void* arg) {
  int i;
  struct thread_params params = *(struct thread_params*)arg;

  /*
   * The local HID of a control block should be <= 64 to keep the SHM key low.
   * But the number of clients over all machines can be larger.
   */
  int clt_gid = params.id; /* Global ID of this client thread */
  int clt_local_hid = clt_gid % params.num_threads;

  int ib_port_index = params.dual_port == 0 ? 0 : clt_gid % 2;

  struct hrd_ctrl_blk* cb = hrd_ctrl_blk_init(
      clt_local_hid, ib_port_index, -1, /* port_index, numa_node_id */
      0, 0,                             /* conn qps, use uc */
      NULL, 0, -1,                      /* prealloc conn buf, buf size, key */
      1, BUF_SIZE, -1);                 /* num_dgram_qps, dgram_buf_size, key */

  /* Fill the RECV queue */
  for (i = 0; i < HRD_Q_DEPTH; i++) {
    hrd_post_dgram_recv(cb->dgram_qp[0], (void*)cb->dgram_buf, BUF_SIZE,
                        cb->dgram_buf_mr->lkey);
  }

  char clt_name[HRD_QP_NAME_SIZE];
  sprintf(clt_name, "client-%d", clt_gid);

  hrd_publish_dgram_qp(cb, 0, clt_name);
  printf("main: Client %s published.\n", clt_name);

  struct ibv_wc wc;

  long long rolling_iter = 0; /* For throughput measurement */
  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &start);

  while (1) {
    if (rolling_iter >= K_512) {
      clock_gettime(CLOCK_REALTIME, &end);
      double seconds = (end.tv_sec - start.tv_sec) +
                       (double)(end.tv_nsec - start.tv_nsec) / 1000000000;
      printf("main: Client %d: %.2f IOPS\n", clt_gid, K_512 / seconds);
      rolling_iter = 0;

      clock_gettime(CLOCK_REALTIME, &start);
    }

    hrd_poll_cq(cb->dgram_recv_cq[0], 1, &wc);
    hrd_post_dgram_recv(cb->dgram_qp[0], (void*)cb->dgram_buf, BUF_SIZE,
                        cb->dgram_buf_mr->lkey);

    rolling_iter++;
  }

  return NULL;
}
