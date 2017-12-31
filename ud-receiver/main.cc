#include <gflags/gflags.h>
#include <stdlib.h>
#include <string.h>
#include <limits>
#include <thread>
#include "libhrd_cpp/hrd.h"

static constexpr size_t kAppNumQPs = 1;  // UD QPs used by server for RECVs
static constexpr size_t kAppBufSize = 4096;
static constexpr size_t kAppMaxPostlist = 64;
static constexpr size_t kAppUnsigBatch = 64;
static_assert(is_power_of_two(kAppUnsigBatch), "");

struct thread_params_t {
  size_t id;
  double* tput;
};

DEFINE_uint64(machine_id, std::numeric_limits<size_t>::max(), "Machine ID");
DEFINE_uint64(num_threads, 0, "Number of threads");
DEFINE_uint64(is_client, 0, "Is this process a client?");
DEFINE_uint64(dual_port, 0, "Use two ports?");
DEFINE_uint64(size, 0, "RDMA size");
DEFINE_uint64(postlist, std::numeric_limits<size_t>::max(), "Postlist size");

void run_server(thread_params_t* params) {
  size_t srv_gid = params->id;  // Global ID of this server thread
  size_t ib_port_index = FLAGS_dual_port == 0 ? 0 : srv_gid % 2;

  hrd_dgram_config_t dgram_config;
  dgram_config.num_qps = kAppNumQPs;
  dgram_config.prealloc_buf = nullptr;
  dgram_config.buf_size = kAppBufSize;
  dgram_config.buf_shm_key = -1;

  auto* cb = hrd_ctrl_blk_init(srv_gid, ib_port_index, kHrdInvalidNUMANode,
                               nullptr, &dgram_config);

  // Buffer to receive packets into. Set to zero.
  memset(const_cast<uint8_t*>(cb->dgram_buf), 0, kAppBufSize);

  for (size_t qp_i = 0; qp_i < kAppNumQPs; qp_i++) {
    // Fill this QP with recvs before publishing it to clients
    for (size_t i = 0; i < kHrdRQDepth; i++) {
      hrd_post_dgram_recv(cb->dgram_qp[qp_i],
                          const_cast<uint8_t*>(&cb->dgram_buf[0]),
                          FLAGS_size + 40, cb->dgram_buf_mr->lkey);
    }

    char srv_name[kHrdQPNameSize];
    sprintf(srv_name, "server-%zu-%zu", srv_gid, qp_i);
    hrd_publish_dgram_qp(cb, qp_i, srv_name);
  }

  printf("main: Server %zu published QPs. Now polling.\n", srv_gid);

  size_t rolling_iter = 0, qp_i = 0;
  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &start);

  while (true) {
    if (rolling_iter >= MB(8)) {
      clock_gettime(CLOCK_REALTIME, &end);
      double seconds = (end.tv_sec - start.tv_sec) +
                       (end.tv_nsec - start.tv_nsec) / 1000000000.0;
      printf("main: Server %zu: %.2f IOPS. \n", srv_gid,
             rolling_iter / seconds);

      params->tput[srv_gid] = rolling_iter / seconds;
      if (srv_gid == 0) {
        double tot = 0;
        for (size_t i = 0; i < FLAGS_num_threads; i++) tot += params->tput[i];
        hrd_red_printf("main: Total tput %.2f IOPS.\n", tot);
      }

      rolling_iter = 0;

      clock_gettime(CLOCK_REALTIME, &start);
    }

    struct ibv_wc wc[kAppMaxPostlist];
    int num_comps = ibv_poll_cq(cb->dgram_recv_cq[qp_i], FLAGS_postlist, wc);
    assert(num_comps >= 0);
    if (num_comps == 0) continue;

    // Post a batch of RECVs
    struct ibv_recv_wr recv_wr[kAppMaxPostlist], *bad_recv_wr;
    struct ibv_sge sgl[kAppMaxPostlist];
    for (size_t w_i = 0; w_i < static_cast<size_t>(num_comps); w_i++) {
      assert(wc[w_i].imm_data == 3185);
      sgl[w_i].length = FLAGS_size + 40;
      sgl[w_i].lkey = cb->dgram_buf_mr->lkey;
      sgl[w_i].addr = reinterpret_cast<uint64_t>(&cb->dgram_buf[0]);

      recv_wr[w_i].sg_list = &sgl[w_i];
      recv_wr[w_i].num_sge = 1;
      recv_wr[w_i].next = w_i == static_cast<size_t>(num_comps) - 1
                              ? nullptr
                              : &recv_wr[w_i + 1];
    }

    int ret = ibv_post_recv(cb->dgram_qp[qp_i], &recv_wr[0], &bad_recv_wr);
    rt_assert(ret == 0);

    rolling_iter += static_cast<size_t>(num_comps);
    qp_i++;
    if (qp_i == kAppNumQPs) qp_i = 0;
  }
}

void run_client(thread_params_t* params) {
  // The local HID of a control block should be <= 64 to keep the SHM key low.
  // But the number of clients over all machines can be larger.
  size_t clt_gid = params->id;  // Global ID of this client thread
  size_t clt_local_hid = clt_gid % FLAGS_num_threads;
  size_t srv_gid = clt_gid % FLAGS_num_threads;
  size_t ib_port_index = FLAGS_dual_port == 0 ? 0 : srv_gid % 2;

  hrd_dgram_config_t dgram_config;
  dgram_config.num_qps = 1;
  dgram_config.prealloc_buf = nullptr;
  dgram_config.buf_size = kAppBufSize;
  dgram_config.buf_shm_key = -1;

  auto* cb = hrd_ctrl_blk_init(clt_local_hid, ib_port_index,
                               kHrdInvalidNUMANode, nullptr, &dgram_config);

  // Buffer to send packets from. Set to a non-zero value.
  memset(const_cast<uint8_t*>(cb->dgram_buf), 1, kAppBufSize);

  printf("main: Client %zu waiting for server %zu\n", clt_gid, srv_gid);

  hrd_qp_attr_t* srv_qp[kAppNumQPs] = {nullptr};
  for (size_t qp_i = 0; qp_i < kAppNumQPs; qp_i++) {
    char srv_name[kHrdQPNameSize];
    sprintf(srv_name, "server-%zu-%zu", srv_gid, qp_i);
    while (srv_qp[qp_i] == nullptr) {
      srv_qp[qp_i] = hrd_get_published_qp(srv_name);
      if (srv_qp[qp_i] == nullptr) usleep(200000);
    }
  }

  printf("main: Client %zu found server! Now posting SENDs.\n", clt_gid);

  // We need only one address handle because we contacts only one server
  struct ibv_ah_attr ah_attr;
  memset(&ah_attr, 0, sizeof(ah_attr));
  ah_attr.is_global = 0;
  ah_attr.dlid = srv_qp[0]->lid;
  ah_attr.sl = 0;
  ah_attr.src_path_bits = 0;
  ah_attr.port_num = cb->resolve.dev_port_id;

  struct ibv_ah* ah = ibv_create_ah(cb->pd, &ah_attr);
  assert(ah != nullptr);

  struct ibv_send_wr wr[kAppMaxPostlist], *bad_send_wr;
  struct ibv_wc wc[kAppMaxPostlist];
  struct ibv_sge sgl[kAppMaxPostlist];
  size_t rolling_iter = 0;  // For throughput measurement
  size_t nb_tx = 0;
  size_t qp_i = 0;

  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &start);

  while (true) {
    if (rolling_iter >= MB(2)) {
      clock_gettime(CLOCK_REALTIME, &end);
      double seconds = (end.tv_sec - start.tv_sec) +
                       (end.tv_nsec - start.tv_nsec) / 1000000000.0;
      printf("main: Client %zu: %.2f IOPS\n", clt_gid, rolling_iter / seconds);
      rolling_iter = 0;

      clock_gettime(CLOCK_REALTIME, &start);
    }

    for (size_t w_i = 0; w_i < FLAGS_postlist; w_i++) {
      wr[w_i].wr.ud.ah = ah;
      wr[w_i].wr.ud.remote_qpn = srv_qp[qp_i]->qpn;
      wr[w_i].wr.ud.remote_qkey = kHrdDefaultQKey;

      wr[w_i].opcode = IBV_WR_SEND_WITH_IMM;
      wr[w_i].num_sge = 1;
      wr[w_i].next = (w_i == FLAGS_postlist - 1) ? nullptr : &wr[w_i + 1];
      wr[w_i].imm_data = 3185;
      wr[w_i].sg_list = &sgl[w_i];

      // kAppUnsigBatch >= 2 * postlist ensures that we poll for a
      // completed send() only after we have performed a signaled send().
      wr[w_i].send_flags = nb_tx % kAppUnsigBatch == 0 ? IBV_SEND_SIGNALED : 0;
      if (nb_tx % kAppUnsigBatch == kAppUnsigBatch - 1) {
        hrd_poll_cq(cb->dgram_send_cq[0], 1, wc);
      }

      wr[w_i].send_flags |= IBV_SEND_INLINE;

      sgl[w_i].addr = reinterpret_cast<uint64_t>(cb->dgram_buf);
      sgl[w_i].length = FLAGS_size;

      rolling_iter++;
      nb_tx++;
    }

    int ret = ibv_post_send(cb->dgram_qp[0], &wr[0], &bad_send_wr);
    rt_assert(ret == 0);

    qp_i++;
    if (qp_i == kAppNumQPs) qp_i = 0;
  }
}

int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  double* tput = nullptr;  // Leaked

  rt_assert(FLAGS_num_threads >= 1, "Invalid num_threads");
  rt_assert(FLAGS_dual_port <= 1, "Invalid dual_port");
  rt_assert(FLAGS_is_client <= 1, "Invalid is_client");
  rt_assert(FLAGS_postlist >= 1 && FLAGS_postlist <= kAppMaxPostlist,
            "Invalid postlist");
  rt_assert(FLAGS_size > 0 && FLAGS_size + 40 <= kAppBufSize,
            "Invalid transfer size");

  if (FLAGS_is_client == 1) {
    rt_assert(FLAGS_machine_id != std::numeric_limits<size_t>::max(),
              "Invalid machine_id");

    rt_assert(FLAGS_postlist <= kAppUnsigBatch, "Postlist check failed");
    static_assert(kHrdSQDepth >= 2 * kAppUnsigBatch, "Queue capacity check");
  } else {
    rt_assert(FLAGS_machine_id == std::numeric_limits<size_t>::max(), "");
    rt_assert(FLAGS_postlist <= kHrdRQDepth / 2, "RECV pollbatch too large");

    tput = new double[FLAGS_num_threads];
    for (size_t i = 0; i < FLAGS_num_threads; i++) tput[i] = 0;
  }

  // Launch a single server thread or multiple client threads
  printf("main: Using %zu threads\n", FLAGS_num_threads);
  std::vector<thread_params_t> param_arr(FLAGS_num_threads);
  std::vector<std::thread> thread_arr(FLAGS_num_threads);

  for (size_t i = 0; i < FLAGS_num_threads; i++) {
    if (FLAGS_is_client == 1) {
      param_arr[i].id = (FLAGS_machine_id * FLAGS_num_threads) + i;
      thread_arr[i] = std::thread(run_client, &param_arr[i]);
    } else {
      param_arr[i].id = i;
      param_arr[i].tput = tput;
      thread_arr[i] = std::thread(run_server, &param_arr[i]);
    }
  }

  for (auto& t : thread_arr) t.join();
  return 0;
}
