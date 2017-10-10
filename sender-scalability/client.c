#include "hrd.h"
#include "main.h"

void* run_client(void* arg) {
  int i;
  int num_threads = NUM_CLIENT_THREADS / NUM_CLIENT_MACHINES;

  struct thread_params params = *(struct thread_params*)arg;
  int clt_gid = params.id; /* Global ID of this server thread */
  int ib_port_index = params.dual_port == 0 ? 0 : clt_gid % 2;
  int shm_key = BASE_SHM_KEY + clt_gid % num_threads;

  struct hrd_ctrl_blk* cb = hrd_ctrl_blk_init(
      clt_gid,                           /* local_hid */
      ib_port_index, 0,                  /* port_index, numa_node_id */
      NUM_SERVER_THREADS, params.use_uc, /* conn qps, uc */
      NULL, BUF_SIZE, shm_key,           /* prealloc conn buf, buf size, key */
      0, 0, -1);                         /* #dgram qps, buf size, shm key */

  /* Set to some non-zero value so the server can detect READ completion */
  memset((void*)cb->conn_buf, 1, BUF_SIZE);

  for (i = 0; i < NUM_SERVER_THREADS; i++) {
    /* In dual-port mode, connect to servers on the same port only */
    if (params.dual_port == 1 && (i % 2 != clt_gid % 2)) {
      continue;
    }

    char clt_qp_name[HRD_QP_NAME_SIZE];
    sprintf(clt_qp_name, "client-%d-%d", clt_gid, i);

    hrd_publish_conn_qp(cb, i, clt_qp_name);
    printf("main: Client %d published qp for server %d\n", clt_gid, i);
  }

  for (i = 0; i < NUM_SERVER_THREADS; i++) {
    if (params.dual_port == 1 && (i % 2 != clt_gid % 2)) {
      continue;
    }

    char srv_qp_name[HRD_QP_NAME_SIZE];
    sprintf(srv_qp_name, "server-%d-%d", i, clt_gid);

    struct hrd_qp_attr* srv_qp = NULL;
    while (srv_qp == NULL) {
      srv_qp = hrd_get_published_qp(srv_qp_name);
      if (srv_qp == NULL) {
        usleep(20000);
      }
    }

    printf("main: Client %d found server %d! Connecting..\n", clt_gid, i);
    hrd_connect_qp(cb, i, srv_qp);

    char clt_qp_name[HRD_QP_NAME_SIZE];
    sprintf(clt_qp_name, "client-%d-%d", clt_gid, i);
    hrd_publish_ready(clt_qp_name);
  }

  printf("main: Client %d READY\n", clt_gid);

  struct timespec run_start, run_end;
  clock_gettime(CLOCK_REALTIME, &run_start);

  while (1) {
    printf("main: Client %d: %d\n", clt_gid, cb->conn_buf[0]);

    clock_gettime(CLOCK_REALTIME, &run_end);
    double run_seconds =
        (run_end.tv_sec - run_start.tv_sec) +
        (double)(run_end.tv_nsec - run_start.tv_nsec) / 1000000000;

    if (run_seconds >= params.run_time + RUN_TIME_SLACK) {
      printf("main: Client %d: exiting\n", clt_gid);
      hrd_ctrl_blk_destroy(cb);
      return NULL;
    } else {
      printf("main: Client %d: active for %.2f seconds (of %d + %d)\n", clt_gid,
             run_seconds, params.run_time, RUN_TIME_SLACK);
    }

    sleep(1);
  }
}
