#include <gflags/gflags.h>
#include <limits>
#include <thread>
#include "libhrd_cpp/hrd.h"

static constexpr size_t kAppNumQPs = 1;
static constexpr size_t kAppBufSize = 8192;
static constexpr bool kAppRoundOffset = true;
static constexpr size_t kAppMaxPostlist = 64;
static constexpr size_t kAppMaxServers = 64;
static constexpr size_t kAppUnsigBatch = 64;
static_assert(is_power_of_two(kAppUnsigBatch), "");

DEFINE_uint64(machine_id, std::numeric_limits<size_t>::max(), "Machine ID");
DEFINE_uint64(num_threads, 0, "Number of threads");
DEFINE_uint64(is_client, 0, "Is this process a client?");
DEFINE_uint64(dual_port, 0, "Use two ports?");
DEFINE_uint64(use_uc, 0, "Use unreliable connected transport?");
DEFINE_uint64(do_read, 0, "Do RDMA reads?");
DEFINE_uint64(size, 0, "RDMA size");
DEFINE_uint64(postlist, 0, "Postlist size");

std::array<double, kAppMaxServers> tput;

struct thread_params_t {
  size_t id;
};

int stick_this_thread_to_core(int) {
  // TODO: Copy from eRPC
  return 0;
}

void run_server(thread_params_t* params) {
  size_t srv_gid = params->id;  // Global ID of this server thread
  size_t clt_gid = srv_gid;     // One-to-one connections
  size_t ib_port_index = FLAGS_dual_port == 0 ? 0 : (srv_gid % 2) * 2;

  // Pinning is useful when running servers on 2 sockets - we want to check
  // if socket 0's cores are getting higher tput than socket 1, which is hard
  // if threads move between cores.
  if (FLAGS_num_threads > 16) {
    hrd_red_printf(
        "main: Server is running on two sockets. Pinning threads"
        " using CPU_SET\n");
    sleep(2);
    stick_this_thread_to_core(srv_gid);
  }

  hrd_conn_config_t conn_config;
  conn_config.num_qps = kAppNumQPs;
  conn_config.use_uc = (FLAGS_use_uc == 1);
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = kAppBufSize;
  conn_config.buf_shm_key = -1;

  auto* cb = hrd_ctrl_blk_init(srv_gid, ib_port_index, kHrdInvalidNUMANode,
                               &conn_config, nullptr);

  memset(const_cast<uint8_t*>(cb->conn_buf), static_cast<uint8_t>(srv_gid) + 1,
         kAppBufSize);

  char srv_name[kAppNumQPs][kHrdQPNameSize];
  for (size_t i = 0; i < kAppNumQPs; i++) {
    sprintf(srv_name[i], "server-%zu-%zu", srv_gid, i);
    hrd_publish_conn_qp(cb, i, srv_name[i]);
  }
  printf("main: Server %zu published. Waiting for client %zu\n", srv_gid,
         clt_gid);

  char clt_name[kAppNumQPs][kHrdQPNameSize];
  hrd_qp_attr_t* clt_qp[kAppNumQPs];
  for (size_t i = 0; i < kAppNumQPs; i++) {
    sprintf(clt_name[i], "client-%zu-%zu", clt_gid, i);

    clt_qp[i] = nullptr;
    while (clt_qp[i] == nullptr) {
      clt_qp[i] = hrd_get_published_qp(clt_name[i]);
      if (clt_qp[i] == nullptr) usleep(200000);
    }

    hrd_connect_qp(cb, i, clt_qp[i]);
    hrd_wait_till_ready(clt_name[i]);
  }

  printf("main: Server %zu connected!\n", srv_gid);

  struct ibv_send_wr wr[kAppMaxPostlist], *bad_send_wr;
  struct ibv_sge sgl[kAppMaxPostlist];
  struct ibv_wc wc;
  size_t rolling_iter = 0;  // For performance measurement
  size_t nb_tx[kAppNumQPs] = {0};
  size_t qp_i = 0;  // Round robin between QPs across postlists
  uint64_t seed = 0xdeadbeef;

  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &start);

  auto opcode = FLAGS_do_read == 0 ? IBV_WR_RDMA_WRITE : IBV_WR_RDMA_READ;

  while (true) {
    if (rolling_iter >= MB(8)) {
      clock_gettime(CLOCK_REALTIME, &end);
      double seconds = (end.tv_sec - start.tv_sec) +
                       (end.tv_nsec - start.tv_nsec) / 1000000000.0;

      tput[srv_gid] = rolling_iter / seconds;
      printf("main: Server %zu: %.2f IOPS\n", srv_gid, tput[srv_gid]);

      // Collecting stats at every server can reduce tput by ~ 10%
      if (srv_gid == 0 && rand() % 5 == 0) {
        double total_tput = 0;
        for (size_t i = 0; i < kAppMaxServers; i++) total_tput += tput[i];
        hrd_red_printf("main: Total throughput = %.2f\n", total_tput);
      }

      rolling_iter = 0;

      clock_gettime(CLOCK_REALTIME, &start);
    }

    // Post a list of work requests in a single ibv_post_send()
    for (size_t w_i = 0; w_i < FLAGS_postlist; w_i++) {
      wr[w_i].opcode = opcode;
      wr[w_i].num_sge = 1;
      wr[w_i].next = (w_i == FLAGS_postlist - 1) ? nullptr : &wr[w_i + 1];
      wr[w_i].sg_list = &sgl[w_i];

      wr[w_i].send_flags =
          nb_tx[qp_i] % kAppUnsigBatch == 0 ? IBV_SEND_SIGNALED : 0;
      if (nb_tx[qp_i] % kAppUnsigBatch == 0 && nb_tx[qp_i] != 0) {
        hrd_poll_cq(cb->conn_cq[qp_i], 1, &wc);
      }

      wr[w_i].send_flags |= (FLAGS_do_read == 0) ? IBV_SEND_INLINE : 0;

      size_t offset = hrd_fastrand(&seed) % kAppBufSize;
      if (kAppRoundOffset) offset = round_up<64>(offset);
      while (unlikely(offset >= kAppBufSize - FLAGS_size)) {
        offset = hrd_fastrand(&seed) % kAppBufSize;
        if (kAppRoundOffset) offset = round_up<64>(offset);
      }

      sgl[w_i].addr = reinterpret_cast<uint64_t>(&cb->conn_buf[offset]);
      sgl[w_i].length = FLAGS_size;
      sgl[w_i].lkey = cb->conn_buf_mr->lkey;

      wr[w_i].wr.rdma.remote_addr = clt_qp[qp_i]->buf_addr + offset;
      wr[w_i].wr.rdma.rkey = clt_qp[qp_i]->rkey;

      nb_tx[qp_i]++;
    }

    int ret = ibv_post_send(cb->conn_qp[qp_i], &wr[0], &bad_send_wr);
    rt_assert(ret == 0);

    rolling_iter += FLAGS_postlist;
    qp_i++;
    if (qp_i == kAppNumQPs) qp_i = 0;
  }
}

void run_client(thread_params_t* params) {
  size_t clt_gid = params->id;  // Global ID of this server thread
  size_t srv_gid = clt_gid;     // One-to-one connections
  size_t ib_port_index = FLAGS_dual_port == 0 ? 0 : (srv_gid % 2) * 2;

  hrd_conn_config_t conn_config;
  conn_config.num_qps = kAppNumQPs;
  conn_config.use_uc = (FLAGS_use_uc == 1);
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = kAppBufSize;
  conn_config.buf_shm_key = -1;

  auto* cb = hrd_ctrl_blk_init(clt_gid, ib_port_index, kHrdInvalidNUMANode,
                               &conn_config, nullptr);

  char srv_name[kAppNumQPs][kHrdQPNameSize];
  for (size_t i = 0; i < kAppNumQPs; i++) {
    sprintf(srv_name[i], "server-%zu-%zu", srv_gid, i);
  }

  char clt_name[kAppNumQPs][kHrdQPNameSize];
  for (size_t i = 0; i < kAppNumQPs; i++) {
    sprintf(clt_name[i], "client-%zu-%zu", clt_gid, i);
    hrd_publish_conn_qp(cb, i, clt_name[i]);
  }

  printf("main: Client %zu published. Waiting for server %zu.\n", clt_gid,
         srv_gid);

  hrd_qp_attr_t* srv_qp[kAppNumQPs] = {nullptr};
  for (size_t i = 0; i < kAppNumQPs; i++) {
    while (srv_qp[i] == nullptr) {
      srv_qp[i] = hrd_get_published_qp(srv_name[i]);
      if (srv_qp[i] == nullptr) usleep(200000);
    }

    hrd_connect_qp(cb, i, srv_qp[i]);
    hrd_publish_ready(clt_name[i]);
  }

  printf("main: Client %zu connected!\n", clt_gid);

  while (1) {
    printf("main: Client %zu: %d\n", clt_gid, cb->conn_buf[0]);
    sleep(1);
  }
}

int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  rt_assert(FLAGS_num_threads >= 1, "");
  rt_assert(kAppUnsigBatch >= FLAGS_postlist, "");   // Postlist check
  rt_assert(kHrdSQDepth >= 2 * FLAGS_postlist, "");  // Queue capacity check

  if (FLAGS_is_client == 0) {
    // Server
    rt_assert(FLAGS_machine_id == std::numeric_limits<size_t>::max(), "");
    rt_assert(FLAGS_size > 0, "");
    if (FLAGS_do_read == 0) rt_assert(FLAGS_size <= kHrdMaxInline, "Inl error");

    rt_assert(FLAGS_postlist >= 1 && FLAGS_postlist <= kAppMaxPostlist, "");
  }

  for (size_t i = 0; i < kAppMaxServers; i++) tput[i] = 0;

  std::vector<thread_params_t> param_arr(FLAGS_num_threads);
  std::vector<std::thread> thread_arr(FLAGS_num_threads);

  printf("main: Using %zu threads\n", FLAGS_num_threads);

  for (size_t i = 0; i < FLAGS_num_threads; i++) {
    if (FLAGS_is_client == 1) {
      param_arr[i].id = (FLAGS_machine_id * FLAGS_num_threads) + i;
      thread_arr[i] = std::thread(run_client, &param_arr[i]);
    } else {
      param_arr[i].id = i;
      thread_arr[i] = std::thread(run_server, &param_arr[i]);
    }
  }

  for (auto& t : thread_arr) t.join();
  return 0;
}
