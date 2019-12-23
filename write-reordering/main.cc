/**
 * @file main.cc
 *
 * @brief Tests left-to-right ordering of RDMA writes. Passes on ConnectX-3 and
 * ConnectX-5 InfiniBand
 *
 * The client writes fills a remote buffer with a counter value, with multiple
 * RDMA write transactions. The server constantly checks that a buffer location
 * to the right of another buffer location always has a smaller counter value.
 *
 * @author Anuj Kalia
 */

#include <gflags/gflags.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <vector>
#include "libhrd_cpp/hrd.h"

DEFINE_uint64(is_client, 0, "Is this process a client?");
static constexpr size_t kAppBufSize = KB(128);
static constexpr size_t kAppNumBufs = 16;

void run_server() {
  struct hrd_conn_config_t conn_config;
  conn_config.num_qps = 1;
  conn_config.use_uc = false;
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = kAppBufSize * kAppNumBufs;
  conn_config.buf_shm_key = 3185;

  auto* cb = hrd_ctrl_blk_init(0 /* id */, 0 /* port */, 0 /* numa */,
                               &conn_config, nullptr /* dgram config */);
  memset(const_cast<uint8_t*>(cb->conn_buf), 0, kAppBufSize);

  hrd_publish_conn_qp(cb, 0, "server");
  printf("main: Server published. Waiting for client.\n");

  hrd_qp_attr_t* clt_qp = nullptr;
  while (clt_qp == nullptr) {
    clt_qp = hrd_get_published_qp("client");
    if (clt_qp == nullptr) usleep(200000);
  }

  printf("main: Server %s found client! Connecting..\n", "server");
  hrd_connect_qp(cb, 0, clt_qp);
  hrd_publish_ready("server");
  printf("main: Server ready\n");

  uint64_t seed = 0xdeadbeef;
  auto* ptr = reinterpret_cast<volatile size_t*>(cb->conn_buf);

  size_t iters = 0;
  while (true) {
    size_t loc_1 =
        hrd_fastrand(&seed) % ((kAppBufSize * kAppNumBufs) / sizeof(size_t));
    size_t loc_2 =
        hrd_fastrand(&seed) % ((kAppBufSize * kAppNumBufs) / sizeof(size_t));

    if (loc_1 >= loc_2) continue;
    // Here, loc_1 < loc 2

    size_t val_2 = ptr[loc_2];
    asm volatile("" ::: "memory");  // Compiler barrier
    asm volatile("lfence" ::: "memory");
    asm volatile("sfence" ::: "memory");
    asm volatile("mfence" ::: "memory");  // Hardware barrier
    size_t val_1 = ptr[loc_1];

    if (val_2 > val_1) {
      printf("violation %zu %zu %zu %zu\n", loc_1, loc_2, val_1, val_2);
      rt_assert(val_2 == val_1 + 1);  // This is expected to fail eventually
    } else if (val_2 < val_1) {
      // printf("ok\n");
    }

    iters++;
    if (iters % MB(1) == 0) {
      printf("No violation in %zu iterations. Counter sample = %zu.\n", iters,
             val_2);
    }
  }
}

void run_client() {
  hrd_conn_config_t conn_config;
  conn_config.num_qps = 1;
  conn_config.use_uc = false;
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = kAppBufSize * kAppNumBufs;
  conn_config.buf_shm_key = 3185;

  auto* cb = hrd_ctrl_blk_init(0 /* id */, 0 /* port */, 0 /* numa */,
                               &conn_config, nullptr /* dgram config */);

  hrd_publish_conn_qp(cb, 0, "client");
  printf("main: Client published. Waiting for server.\n");

  hrd_qp_attr_t* srv_qp = nullptr;
  while (srv_qp == nullptr) {
    srv_qp = hrd_get_published_qp("server");
    if (srv_qp == nullptr) usleep(2000);
  }

  printf("main: Client found server. Connecting..\n");
  hrd_connect_qp(cb, 0, srv_qp);
  printf("main: Client connected!\n");

  hrd_wait_till_ready("server");

  struct ibv_send_wr wr[kAppNumBufs], *bad_send_wr;
  struct ibv_sge sge[kAppNumBufs];
  struct ibv_wc wc;
  size_t ctr = 0;

  auto* ptr = reinterpret_cast<volatile size_t*>(&cb->conn_buf[0]);
  while (true) {
    ctr++;

    // RDMA write an array with all 8-byte words = ctr
    for (size_t i = 0; i < (kAppBufSize * kAppNumBufs) / sizeof(size_t); i++) {
      ptr[i] = ctr;
    }

    for (size_t i = 0; i < kAppNumBufs; i++) {
      sge[i].addr = reinterpret_cast<uint64_t>(
          &cb->conn_buf[(kAppNumBufs - 1 - i) * kAppBufSize]);
      sge[i].length = kAppBufSize;
      sge[i].lkey = cb->conn_buf_mr->lkey;

      wr[i].opcode = IBV_WR_RDMA_WRITE;
      wr[i].num_sge = 1;
      wr[i].next = nullptr;
      wr[i].sg_list = &sge[i];
      wr[i].send_flags = IBV_SEND_SIGNALED;
      wr[i].wr.rdma.remote_addr = srv_qp->buf_addr + (i * kAppBufSize);
      wr[i].wr.rdma.rkey = srv_qp->rkey;

      int ret = ibv_post_send(cb->conn_qp[0], &wr[i], &bad_send_wr);
      rt_assert(ret == 0);
    }

    for (size_t i = 0; i < kAppNumBufs; i++) {
      // Wait for all kAppBufSize writes to complete
      hrd_poll_cq(cb->conn_cq[0], 1, &wc);
    }
  }
}

int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  FLAGS_is_client == 1 ? run_client() : run_server();
  return 0;
}
