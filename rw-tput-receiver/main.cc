#include <gflags/gflags.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <vector>
#include "libhrd_cpp/hrd.h"

static constexpr size_t kAppBufSize = (8 * 1024 * 1024);
static constexpr size_t kAppMaxPostlist = 64;
static constexpr size_t kAppUnsigBatch = 64;

DEFINE_uint64(num_threads, 0, "Number of threads");
DEFINE_uint64(is_client, 0, "Is this process a client?");
DEFINE_uint64(dual_port, 0, "Use two ports?");
DEFINE_uint64(use_uc, 0, "Use unreliable connected transport?");
DEFINE_uint64(do_read, 0, "Do RDMA reads?");
DEFINE_uint64(machine_id, 0, "ID of this machine");
DEFINE_uint64(size, 0, "RDMA size");
DEFINE_uint64(postlist, 0, "Postlist size");

struct thread_params_t {
  size_t id;
  double* tput;
};

void run_server(thread_params_t* params) {
  size_t srv_gid = params->id;  // Global ID of this server thread
  size_t clt_gid = srv_gid;     // One-to-one connections
  size_t ib_port_index = FLAGS_dual_port == 0 ? 0 : (srv_gid % 2) * 2;

  struct hrd_conn_config_t conn_config;
  conn_config.num_qps = 1;
  conn_config.use_uc = (FLAGS_use_uc == 1);
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = kAppBufSize;
  conn_config.buf_shm_key = -1;

  auto* cb = hrd_ctrl_blk_init(srv_gid, ib_port_index, kHrdInvalidNUMANode,
                               &conn_config, nullptr);

  char srv_name[kHrdQPNameSize];
  sprintf(srv_name, "server-%zu", srv_gid);
  char clt_name[kHrdQPNameSize];
  sprintf(clt_name, "client-%zu", clt_gid);

  hrd_publish_conn_qp(cb, 0, srv_name);
  printf("main: Server %s published. Waiting for client %s\n", srv_name,
         clt_name);

  hrd_qp_attr_t* clt_qp = nullptr;
  while (clt_qp == nullptr) {
    clt_qp = hrd_get_published_qp(clt_name);
    if (clt_qp == nullptr) usleep(200000);
  }

  printf("main: Server %s found client! Connecting..\n", srv_name);
  hrd_connect_qp(cb, 0, clt_qp);

  // This garbles the server's qp_attr - which is safe
  hrd_publish_ready(srv_name);
  printf("main: Server %s READY\n", srv_name);

  while (true) {
    printf("main: Server %s: %d\n", srv_name, cb->conn_buf[0]);
    sleep(1);
  }
}

void run_client(thread_params_t* params) {
  size_t clt_gid = params->id;  // Global ID of this client thread
  size_t srv_gid = clt_gid;     // One-to-one connections
  size_t clt_lid = params->id % FLAGS_num_threads;  // Local ID of this client
  size_t ib_port_index = FLAGS_dual_port == 0 ? 0 : (srv_gid % 2) * 2;

  hrd_conn_config_t conn_config;
  conn_config.num_qps = 1;
  conn_config.use_uc = (FLAGS_use_uc == 1);
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = kAppBufSize;
  conn_config.buf_shm_key = -1;

  auto* cb = hrd_ctrl_blk_init(clt_gid, ib_port_index, kHrdInvalidNUMANode,
                               &conn_config, nullptr);

  memset(const_cast<uint8_t*>(cb->conn_buf), static_cast<uint8_t>(clt_gid) + 1,
         kAppBufSize);

  char srv_name[kHrdQPNameSize];
  sprintf(srv_name, "server-%zu", srv_gid);
  char clt_name[kHrdQPNameSize];
  sprintf(clt_name, "client-%zu", clt_gid);

  hrd_publish_conn_qp(cb, 0, clt_name);
  printf("main: Client %s published. Waiting for server %s\n", clt_name,
         srv_name);

  hrd_qp_attr_t* srv_qp = nullptr;
  while (srv_qp == nullptr) {
    srv_qp = hrd_get_published_qp(srv_name);
    if (srv_qp == nullptr) usleep(200000);
  }

  printf("main: Client %s found server. Connecting..\n", clt_name);
  hrd_connect_qp(cb, 0, srv_qp);
  printf("main: Client %s connected!\n", clt_name);

  hrd_wait_till_ready(srv_name);

  struct ibv_send_wr wr[kAppMaxPostlist], *bad_send_wr;
  struct ibv_sge sgl[kAppMaxPostlist];
  struct ibv_wc wc;
  size_t rolling_iter = 0;  // For performance measurement
  size_t nb_tx = 0;         // For selective signaling
  int ret;

  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &start);

  // The reads/writes at different postlist positions should be done to/from
  // different cache lines.
  size_t stride = 64;
  while (stride < FLAGS_size) stride += 64;
  rt_assert(stride * FLAGS_postlist <= kAppBufSize);

  auto opcode = FLAGS_do_read == 0 ? IBV_WR_RDMA_WRITE : IBV_WR_RDMA_READ;

  while (true) {
    if (rolling_iter >= KB(512)) {
      clock_gettime(CLOCK_REALTIME, &end);
      double seconds = (end.tv_sec - start.tv_sec) +
                       (end.tv_nsec - start.tv_nsec) / 1000000000.0;
      double tput_mrps = rolling_iter / (seconds * 1000000);
      printf("main: Client %zu: %.2f M/s\n", clt_gid, tput_mrps);
      rolling_iter = 0;

      // Per-machine stats
      params->tput[clt_lid] = tput_mrps;
      if (clt_lid == 0) {
        double tot = 0;
        for (size_t i = 0; i < FLAGS_num_threads; i++) tot += params->tput[i];
        hrd_red_printf("main: Machine: %.2f M/s\n", tot);
      }

      clock_gettime(CLOCK_REALTIME, &start);
    }

    // Post a batch
    for (size_t w_i = 0; w_i < FLAGS_postlist; w_i++) {
      wr[w_i].opcode = opcode;
      wr[w_i].num_sge = 1;
      wr[w_i].next = (w_i == FLAGS_postlist - 1) ? nullptr : &wr[w_i + 1];
      wr[w_i].sg_list = &sgl[w_i];

      wr[w_i].send_flags = nb_tx % kAppUnsigBatch == 0 ? IBV_SEND_SIGNALED : 0;
      if (nb_tx % kAppUnsigBatch == 0 && nb_tx > 0) {
        hrd_poll_cq(cb->conn_cq[0], 1, &wc);
      }

      wr[w_i].send_flags |= FLAGS_do_read == 0 ? IBV_SEND_INLINE : 0;

      sgl[w_i].addr = reinterpret_cast<uint64_t>(&cb->conn_buf[stride * w_i]);
      sgl[w_i].length = FLAGS_size;
      sgl[w_i].lkey = cb->conn_buf_mr->lkey;

      wr[w_i].wr.rdma.remote_addr = srv_qp->buf_addr + (stride * w_i);
      wr[w_i].wr.rdma.rkey = srv_qp->rkey;

      nb_tx++;
    }

    ret = ibv_post_send(cb->conn_qp[0], &wr[0], &bad_send_wr);
    rt_assert(ret == 0);

    rolling_iter += FLAGS_postlist;
  }
}

int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  rt_assert(FLAGS_num_threads >= 1, "At least on thread needed");

  if (FLAGS_is_client == 1) {
    if (FLAGS_do_read == 0) {
      rt_assert(FLAGS_size <= kHrdMaxInline, "Inline size too small");
    }
    rt_assert(FLAGS_postlist <= kAppMaxPostlist, "Postlist too large");
    rt_assert(kAppUnsigBatch >= FLAGS_postlist, "Postlist check failed");
    rt_assert(kHrdSQDepth >= 2 * kAppUnsigBatch, "Queue capacity check failed");
  }

  // Launch a single server thread or multiple client threads
  printf("main: Using %zu threads\n", FLAGS_num_threads);
  auto* param_arr = new thread_params_t[FLAGS_num_threads];
  std::vector<std::thread> thread_arr(FLAGS_num_threads);
  auto* tput = new double[FLAGS_num_threads];

  for (size_t i = 0; i < FLAGS_num_threads; i++) {
    if (FLAGS_is_client == 1) {
      param_arr[i].id = (FLAGS_machine_id * FLAGS_num_threads) + i;
      param_arr[i].tput = tput;

      thread_arr[i] = std::thread(run_client, &param_arr[i]);
    } else {
      param_arr[i].id = i;
      thread_arr[i] = std::thread(run_server, &param_arr[i]);
    }
  }

  for (auto& thread : thread_arr) thread.join();
  return 0;
}
