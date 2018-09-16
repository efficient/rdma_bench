#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <thread>
#include <vector>
#include "libhrd_cpp/hrd.h"

/// The value of counter is x iff the client has completed writing x to the
/// server's memory. This is shared between the client and server.
std::atomic<size_t> shared_counter;

/// The client and server ports must be on different NICs for the
/// proof-of-concept to work.
static size_t kClientRDMAPort = 0;
static size_t kServerRDMAPort = 1;

static size_t kBufSize = MB(32);          // Size of registered RDMA buffer
static size_t kWriteSize = kBufSize / 4;  // Size of RDMA write
static bool kDoRead = true;               // Do RDMA read after write

void run_server() {
  // Establish a QP with the client
  struct hrd_conn_config_t conn_config;
  conn_config.num_qps = 1;
  conn_config.use_uc = false;
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = kBufSize;
  conn_config.buf_shm_key = -1;

  auto* cb = hrd_ctrl_blk_init(0 /* id */, kServerRDMAPort, kHrdInvalidNUMANode,
                               &conn_config, nullptr /* datagram config */);
  memset(const_cast<uint8_t*>(cb->conn_buf), 0, kBufSize);

  hrd_publish_conn_qp(cb, 0, "server");
  printf("main: Server published. Waiting for client.\n");

  hrd_qp_attr_t* clt_qp = nullptr;
  while (clt_qp == nullptr) {
    clt_qp = hrd_get_published_qp("client");
    if (clt_qp == nullptr) usleep(200000);
  }

  printf("main: Server found client! Connecting..\n");
  hrd_connect_qp(cb, 0, clt_qp);
  hrd_publish_ready("server");
  printf("main: Server ready\n");

  // Start real work
  auto* ptr = reinterpret_cast<volatile size_t*>(
      &cb->conn_buf[kWriteSize - sizeof(size_t)]);

  size_t iters = 0;
  while (true) {
    size_t minimum_allowed = shared_counter;

    asm volatile("" ::: "memory");        // Compiler barrier
    asm volatile("mfence" ::: "memory");  // Hardware barrier

    // Check if the client's write is actually visible
    size_t actual = ptr[0];
    if (actual < minimum_allowed) {
      printf("client: violation: actual = %zu, minimum_allowed = %zu\n", actual,
             minimum_allowed);
    }
    iters++;
    if (iters % MB(128) == 0) {
      printf("client: no violation in %zu iterations\n", iters);
    }
  }
}

void run_client() {
  // Establish a QP with the server
  hrd_conn_config_t conn_config;
  conn_config.num_qps = 1;
  conn_config.use_uc = false;
  conn_config.prealloc_buf = nullptr;
  conn_config.buf_size = kBufSize;
  conn_config.buf_shm_key = -1;

  auto* cb = hrd_ctrl_blk_init(1 /* id */, kClientRDMAPort, kHrdInvalidNUMANode,
                               &conn_config, nullptr /* datagram config */);

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

  // Start real work
  struct ibv_send_wr wr[2], *bad_send_wr;
  struct ibv_sge sge[2];
  struct ibv_wc wc;

  auto* ptr = reinterpret_cast<volatile size_t*>(&cb->conn_buf[0]);
  while (true) {
    const size_t to_write = shared_counter + 1;
    for (size_t i = 0; i < kWriteSize / sizeof(size_t); i++) ptr[i] = to_write;

    // RDMA write
    sge[0].addr = reinterpret_cast<uint64_t>(cb->conn_buf);
    sge[0].length = kWriteSize;
    sge[0].lkey = cb->conn_buf_mr->lkey;

    wr[0].opcode = IBV_WR_RDMA_WRITE;
    wr[0].num_sge = 1;
    wr[0].next = kDoRead ? &wr[1] : nullptr;
    wr[0].sg_list = &sge[0];
    wr[0].send_flags = kDoRead ? 0 : IBV_SEND_SIGNALED;
    wr[0].wr.rdma.remote_addr = srv_qp->buf_addr;
    wr[0].wr.rdma.rkey = srv_qp->rkey;

    // RDMA read. Read from the last byte in the remote buffer. This likely does
    // not overlap with the RDMA write buffer that we wrote to.
    if (kDoRead) {
      const size_t offset = kBufSize - sizeof(size_t);
      sge[1].addr = reinterpret_cast<uint64_t>(cb->conn_buf) + offset;
      sge[1].length = sizeof(size_t);
      sge[1].lkey = cb->conn_buf_mr->lkey;

      wr[1].opcode = IBV_WR_RDMA_READ;
      wr[1].num_sge = 1;
      wr[1].next = nullptr;
      wr[1].sg_list = &sge[1];
      wr[1].send_flags = IBV_SEND_SIGNALED;
      wr[1].wr.rdma.remote_addr = srv_qp->buf_addr + offset;
      wr[1].wr.rdma.rkey = srv_qp->rkey;
    }

    int ret = ibv_post_send(cb->conn_qp[0], &wr[0], &bad_send_wr);
    rt_assert(ret == 0);
    hrd_poll_cq(cb->conn_cq[0], 1, &wc);  // Block till signaled op completes

    asm volatile("" ::: "memory");        // Compiler barrier
    asm volatile("mfence" ::: "memory");  // Hardware barrier

    shared_counter++;  // At this point, we expect the server to see update
  }
}

int main() {
  shared_counter = 0;

  auto thread_server = std::thread(run_server);
  auto thread_client = std::thread(run_client);

  thread_server.join();
  thread_client.join();
  return 0;
}
