#include "main.h"
#include <byteswap.h>
#include <getopt.h>
#include <gflags/gflags.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <vector>
#include "libhrd_cpp/hrd.h"

/*
 * QP naming:
 * 1. server-i-j is the jth QP on port i of the server.
 * 2. client-i-j is the jth QP of client thread i in the system.
 */
DEFINE_uint64(num_threads, 0, "Number of threads");
DEFINE_uint64(base_port_index, 0, "Base port index");
DEFINE_uint64(num_server_ports, 0, "Number of server ports");
DEFINE_uint64(num_client_ports, 0, "Number of client ports");
DEFINE_bool(is_client, false, "Is this process a client?");
DEFINE_uint64(machine_id, 0, "ID of this machine");
DEFINE_uint64(postlist, 0, "Postlist size");

/* A single server thread creates all QPs */
void* run_server(thread_params_t* params) {
  size_t total_srv_qps = NUM_CLIENTS * QPS_PER_CLIENT;
  size_t srv_gid = params->id; /* Global ID of this server thread */

  /*
   * Create one control block per port. Each control block has enough QPs
   * for all client QPs. The ith client QP must connect to the ith QP in a
   * a control block, but clients can choose the control block.
   */
  auto** cb = new hrd_ctrl_blk_t*[FLAGS_num_server_ports];

  /* Allocate a registered buffer for port #0 only */
  for (size_t i = 0; i < FLAGS_num_server_ports; i++) {
    volatile uint8_t* prealloc_buf = (i == 0 ? nullptr : cb[0]->conn_buf);
    size_t ib_port_index = FLAGS_base_port_index + i;
    int shm_key = (i == 0 ? SERVER_SHM_KEY : -1);

    hrd_conn_config_t conn_config;
    conn_config.num_qps = total_srv_qps;
    conn_config.use_uc = false;
    conn_config.prealloc_buf = prealloc_buf;
    conn_config.buf_size = BUF_SIZE;
    conn_config.buf_shm_key = shm_key;

    cb[i] = hrd_ctrl_blk_init(srv_gid + i,      /* local hid */
                              ib_port_index, 0, /* port index, numa node id */
                              &conn_config, nullptr);

    /* Register all created QPs - only some will get used! */
    for (size_t j = 0; j < total_srv_qps; j++) {
      char srv_name[HRD_QP_NAME_SIZE];
      sprintf(srv_name, "server-%zu-%zu", i, j);
      hrd_publish_conn_qp(cb[i], j, srv_name);
    }

    printf("main: Server published all QPs on port %zu\n", ib_port_index);
  }

  hrd_red_printf("main: Server published all QPs. Waiting for clients..\n");

  for (size_t i = 0; i < NUM_CLIENTS; i++) {
    for (size_t j = 0; j < QPS_PER_CLIENT; j++) {
      /* Iterate over all client QPs */
      char clt_name[HRD_QP_NAME_SIZE];
      sprintf(clt_name, "client-%zu-%zu", i, j);

      hrd_qp_attr_t* clt_qp = nullptr;
      while (clt_qp == nullptr) {
        clt_qp = hrd_get_published_qp(clt_name);
        if (clt_qp == nullptr) usleep(200000);
      }

      /* Calculate the control block and QP to use for this client */
      size_t cb_i = i % FLAGS_num_server_ports;
      size_t qp_i = (i * QPS_PER_CLIENT) + j;

      printf("main: Connecting client %zu's QP %zu --- port %zu QP %zu.\n", i,
             j, cb_i, qp_i);
      hrd_connect_qp(cb[cb_i], qp_i, clt_qp);

      char srv_name[HRD_QP_NAME_SIZE];
      sprintf(srv_name, "server-%zu-%zu", cb_i, qp_i);

      hrd_publish_ready(srv_name);
    }
  }

  auto* counter = reinterpret_cast<volatile size_t*>(cb[0]->conn_buf);

  /* Repetedly print the counter */
  while (1) {
    printf("main: Counter = %zu\n", *counter);
    sleep(1);
  }

  return nullptr;
}

void* run_client(thread_params_t* params) {
  size_t clt_gid = params->id; /* Global ID of this client thread */

  size_t ib_port_index =
      FLAGS_base_port_index + clt_gid % FLAGS_num_client_ports;

  /*
   * Don't use BUF_SIZE at client - it can be large and libhrd zeros it out,
   * which can take time. We don't need a large buffer at clients.
   */
  hrd_conn_config_t conn_config;
  conn_config.num_qps = QPS_PER_CLIENT;
  conn_config.use_uc = false;
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = 4096;
  conn_config.buf_shm_key = -1;

  hrd_ctrl_blk_t* cb =
      hrd_ctrl_blk_init(clt_gid, ib_port_index, 9, &conn_config, nullptr);

  memset(const_cast<uint8_t*>(cb->conn_buf), 0, 4096);

  hrd_qp_attr_t* srv_qp[QPS_PER_CLIENT];

  for (size_t i = 0; i < QPS_PER_CLIENT; i++) {
    /* Compute the server port (or control block) and QP to use */
    size_t srv_cb_i = clt_gid % FLAGS_num_server_ports;
    size_t srv_qp_i = (clt_gid * QPS_PER_CLIENT) + i;
    char srv_name[HRD_QP_NAME_SIZE];
    sprintf(srv_name, "server-%zu-%zu", srv_cb_i, srv_qp_i);

    char clt_name[HRD_QP_NAME_SIZE];
    sprintf(clt_name, "client-%zu-%zu", clt_gid, i);

    /* Publish and connect the ith QP */
    hrd_publish_conn_qp(cb, i, clt_name);
    printf("main: Published client %zu's QP %zu. Waiting for server %s\n",
           clt_gid, i, srv_name);

    srv_qp[i] = nullptr;
    while (srv_qp[i] == nullptr) {
      srv_qp[i] = hrd_get_published_qp(srv_name);
      if (srv_qp[i] == nullptr) {
        usleep(200000);
      }
    }

    printf("main: Client %zu found server! Connecting..\n", clt_gid);
    hrd_connect_qp(cb, i, srv_qp[i]);
    hrd_wait_till_ready(srv_name);
  }

  /* Datapath */
  struct ibv_send_wr wr[MAX_POSTLIST], *bad_send_wr;
  struct ibv_sge sgl[MAX_POSTLIST];
  struct ibv_wc wc;
  size_t w_i = 0;                     /* Window index */
  size_t nb_tx[QPS_PER_CLIENT] = {0}; /* For selective signaling */
  int ret;

  size_t rolling_iter = 0; /* For performance measurement */
  size_t num_reads = 0;    /* Number of RDMA reads issued */
  size_t num_atomics = 0;  /* Number of atomics issued */

  size_t qp_i = 0; /* Queue pair to use for this postlist */

  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &start);

  /* Make this client's counter requests reasonably independent of others */
  uint64_t seed = 0xdeadbeef;
  for (size_t i = 0; i < clt_gid * M_128; i++) {
    hrd_fastrand(&seed);
  }

  /* cb->conn_buf is 8-byte aligned even if hugepages are not used */
  auto* counter = reinterpret_cast<volatile size_t*>(cb->conn_buf);

  while (1) {
    if (rolling_iter >= M_1) {
      clock_gettime(CLOCK_REALTIME, &end);
      double seconds = (end.tv_sec - start.tv_sec) +
                       (end.tv_nsec - start.tv_nsec) / 100000000.0;

      /* Model DrTM tput based on READ and ATOMIC tput */
      size_t num_gets = num_reads;
      size_t num_puts = num_atomics / 2;

#if HRD_CONNECT_IB_ATOMICS == 1
      printf(
          "main: Client %d: %.2f GETs/s, %.2f PUTs/s, %.2f ops/s "
          "(%.2f atomics/s). Ctr = %lld\n",
          clt_gid, num_gets / seconds, num_puts / seconds,
          (num_gets + num_puts) / seconds, num_atomics / seconds,
          (size_t)bswap_64(*counter));
#else
      printf(
          "main: Client %zu: %.2f GETs/s, %.2f PUTs/s, %.2f ops/s "
          "(%.2f atomics/s). Ctr = %zu\n",
          clt_gid, num_gets / seconds, num_puts / seconds,
          (num_gets + num_puts) / seconds, num_atomics / seconds, *counter);
#endif
      /* Reset counters */
      rolling_iter = 0;
      num_reads = 0;
      num_atomics = 0;

      clock_gettime(CLOCK_REALTIME, &start);
    }

    /* Post a postlist of work requests in a single ibv_post_send() */
    for (w_i = 0; w_i < FLAGS_postlist; w_i++) {
      int use_atomic = drtm_use_atomic(hrd_fastrand(&seed));
      if (use_atomic) {
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
      wr[w_i].next = (w_i == FLAGS_postlist - 1) ? nullptr : &wr[w_i + 1];
      wr[w_i].sg_list = &sgl[w_i];

      wr[w_i].send_flags =
          (nb_tx[qp_i] & UNSIG_BATCH_) == 0 ? IBV_SEND_SIGNALED : 0;
      if ((nb_tx[qp_i] & UNSIG_BATCH_) == 0 && nb_tx[qp_i] > 0) {
        hrd_poll_cq(cb->conn_cq[qp_i], 1, &wc);
      }

      sgl[w_i].addr = reinterpret_cast<uint64_t>(cb->conn_buf);
      sgl[w_i].length = 8; /* Only 8 bytes get written */
      sgl[w_i].lkey = cb->conn_buf_mr->lkey;

      size_t index = 0;
      if (USE_RANDOM == 1) {
        index = hrd_fastrand(&seed) % (BUF_SIZE / STRIDE_SIZE);
      }

      uint64_t remote_address = srv_qp[qp_i]->buf_addr + index * STRIDE_SIZE;
      assert(remote_address % STRIDE_SIZE == 0);

      if (use_atomic) {
        if (EMULATE_DRTM == 1) {
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
        sgl[w_i].length = 64; /* We're emulating 16B keys, 32B vals */
        wr[w_i].wr.rdma.remote_addr = remote_address;
        wr[w_i].wr.rdma.rkey = srv_qp[qp_i]->rkey;
      }

      nb_tx[qp_i]++;
    }

    ret = ibv_post_send(cb->conn_qp[qp_i], &wr[0], &bad_send_wr);
    CPE(ret, "ibv_post_send error", ret);

    HRD_MOD_ADD(qp_i, QPS_PER_CLIENT);
    rolling_iter += FLAGS_postlist;
  }

  return nullptr;
}

int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  /* Check some macros  */
  if (EMULATE_DRTM == 1) {
    assert(USE_RANDOM == 1);
    assert(BUF_SIZE >= 128 * 1024 * 1024); /* Spill to DRAM */
    assert(STRIDE_SIZE == 64); /* Restrict to 16B keys and 32B values */
    assert(UPDATE_PERCENTAGE >= 0 && UPDATE_PERCENTAGE <= 100);
  } else {
    assert(BUF_SIZE <= 16 * 1024 * 1024); /* Fit in L3 cache */
    assert(STRIDE_SIZE == 8);         /* If not DrTM, then array of counters */
    assert(UPDATE_PERCENTAGE == 100); /* Counters are only updated */
  }

  // Check the flags
  assert(FLAGS_base_port_index >= 0 && FLAGS_base_port_index <= 8);

  if (FLAGS_is_client) {
    assert(FLAGS_num_server_ports >= 1 && FLAGS_num_server_ports <= 8);
    assert(FLAGS_num_client_ports >= 1 && FLAGS_num_client_ports <= 8);
    assert(FLAGS_num_threads >= 1);
    assert(FLAGS_machine_id >= 0);
    assert(FLAGS_postlist >= 1 && FLAGS_postlist <= MAX_POSTLIST);

    assert(UNSIG_BATCH >= FLAGS_postlist);   /* Postlist check */
    assert(HRD_SQ_DEPTH >= 2 * UNSIG_BATCH); /* Queue capacity check */
  } else {
    assert(FLAGS_num_server_ports >= 1 && FLAGS_num_server_ports <= 8);
    FLAGS_num_threads = 1; /* Needed to allocate thread structs later */
  }

  /* Launch a single server thread or multiple client threads */
  printf("main: Using %zu threads\n", FLAGS_num_threads);
  auto* param_arr = new thread_params_t[FLAGS_num_threads];
  std::vector<std::thread> thread_arr(FLAGS_num_threads);

  if (FLAGS_is_client) {
    for (size_t i = 0; i < FLAGS_num_threads; i++) {
      param_arr[i].id = (FLAGS_machine_id * FLAGS_num_threads) + i;
      thread_arr[i] = std::thread(run_client, &param_arr[i]);
    }
  } else {
    /* Only a single server thread */
    param_arr[0].id = 0;
    thread_arr[0] = std::thread(run_server, &param_arr[0]);
  }

  for (std::thread& thread : thread_arr) thread.join();
  return 0;
}
