#include <sstream>
#include "hrd.h"

// Every thread creates a TCP connection to the registry only once.
__thread memcached_st* memc = nullptr;

static std::string link_layer_str(uint8_t link_layer) {
  switch (link_layer) {
    case IBV_LINK_LAYER_UNSPECIFIED:
      return "[Unspecified]";
    case IBV_LINK_LAYER_INFINIBAND:
      return "[InfiniBand]";
    case IBV_LINK_LAYER_ETHERNET:
      return "[Ethernet]";
    default:
      return "[Invalid]";
  }
}

// Print information about all IB devices in the system
void hrd_ibv_devinfo(void) {
  int num_devices = 0, dev_i;
  struct ibv_device** dev_list;
  struct ibv_context* ctx;
  struct ibv_device_attr device_attr;

  hrd_red_printf("HRD: printing IB dev info\n");

  dev_list = ibv_get_device_list(&num_devices);
  assert(dev_list != nullptr);

  for (dev_i = 0; dev_i < num_devices; dev_i++) {
    ctx = ibv_open_device(dev_list[dev_i]);
    assert(ctx != nullptr);

    memset(&device_attr, 0, sizeof(device_attr));
    if (ibv_query_device(ctx, &device_attr)) {
      printf("Could not query device: %d\n", dev_i);
      assert(false);
    }

    printf("IB device %d:\n", dev_i);
    printf("    Name: %s\n", dev_list[dev_i]->name);
    printf("    Device name: %s\n", dev_list[dev_i]->dev_name);
    printf("    GUID: %zx\n",
           static_cast<size_t>(ibv_get_device_guid(dev_list[dev_i])));
    printf("    Node type: %d (-1: UNKNOWN, 1: CA, 4: RNIC)\n",
           dev_list[dev_i]->node_type);
    printf("    Transport type: %d (-1: UNKNOWN, 0: IB, 1: IWARP)\n",
           dev_list[dev_i]->transport_type);

    printf("    fw: %s\n", device_attr.fw_ver);
    printf("    max_qp: %d\n", device_attr.max_qp);
    printf("    max_cq: %d\n", device_attr.max_cq);
    printf("    max_mr: %d\n", device_attr.max_mr);
    printf("    max_pd: %d\n", device_attr.max_pd);
    printf("    max_ah: %d\n", device_attr.max_ah);
    printf("    phys_port_cnt: %u\n", device_attr.phys_port_cnt);
  }
}

// Finds the port with rank `port_index` (0-based) in the list of ENABLED ports.
// Fills its device id and device-local port id (1-based) into the supplied
// control block.
void hrd_resolve_port_index(struct hrd_ctrl_blk_t* cb, size_t phy_port) {
  std::ostringstream xmsg;  // The exception message
  auto& resolve = cb->resolve;

  // Get the device list
  int num_devices = 0;
  struct ibv_device** dev_list = ibv_get_device_list(&num_devices);
  rt_assert(dev_list != nullptr, "Failed to get InfiniBand device list");

  // Traverse the device list
  int ports_to_discover = phy_port;

  for (int dev_i = 0; dev_i < num_devices; dev_i++) {
    struct ibv_context* ib_ctx = ibv_open_device(dev_list[dev_i]);
    rt_assert(ib_ctx != nullptr, "Failed to open dev " + std::to_string(dev_i));

    struct ibv_device_attr device_attr;
    memset(&device_attr, 0, sizeof(device_attr));
    if (ibv_query_device(ib_ctx, &device_attr) != 0) {
      xmsg << " Failed to query InfiniBand device " << std::to_string(dev_i);
      throw std::runtime_error(xmsg.str());
    }

    for (uint8_t port_i = 1; port_i <= device_attr.phys_port_cnt; port_i++) {
      // Count this port only if it is enabled
      struct ibv_port_attr port_attr;
      if (ibv_query_port(ib_ctx, port_i, &port_attr) != 0) {
        xmsg << "Failed to query port " << std::to_string(port_i)
             << " on device " << ib_ctx->device->name;
        throw std::runtime_error(xmsg.str());
      }

      if (port_attr.phys_state != IBV_PORT_ACTIVE &&
          port_attr.phys_state != IBV_PORT_ACTIVE_DEFER) {
        continue;
      }

      if (ports_to_discover == 0) {
        // Resolution succeeded. Check if the link layer matches.
        if (!kRoCE && port_attr.link_layer != IBV_LINK_LAYER_INFINIBAND) {
          throw std::runtime_error(
              "Transport type required is InfiniBand but port link layer is " +
              link_layer_str(port_attr.link_layer));
        }

        if (kRoCE && port_attr.link_layer != IBV_LINK_LAYER_ETHERNET) {
          throw std::runtime_error(
              "Transport type required is RoCE but port link layer is " +
              link_layer_str(port_attr.link_layer) +
              ". Try setting kRoCE to false.");
        }

        printf("HRD: port index %zu resolved to device %d, port %d. Name %s.\n",
               phy_port, dev_i, port_i, dev_list[dev_i]->name);

        resolve.device_id = dev_i;
        resolve.ib_ctx = ib_ctx;
        resolve.dev_port_id = port_i;
        resolve.port_lid = port_attr.lid;

        // Resolve and cache the ibv_gid struct for RoCE
        if (kRoCE) {
          int ret = ibv_query_gid(ib_ctx, resolve.dev_port_id, 0, &resolve.gid);
          rt_assert(ret == 0, "Failed to query GID");
        }

        return;
      }

      ports_to_discover--;
    }

    // Thank you Mario, but our port is in another device
    if (ibv_close_device(ib_ctx) != 0) {
      xmsg << "Failed to close device" << ib_ctx->device->name;
      throw std::runtime_error(xmsg.str());
    }
  }

  // If we are here, port resolution has failed
  assert(resolve.ib_ctx == nullptr);
  xmsg << "Failed to resolve InfiniBand port index "
       << std::to_string(phy_port);
  throw std::runtime_error(xmsg.str());
}

// Allocate SHM with @shm_key, and save the shmid into @shm_id_ret
uint8_t* hrd_malloc_socket(int shm_key, size_t size, size_t socket_id) {
  int shmid = shmget(shm_key, size, IPC_CREAT | IPC_EXCL | 0666 | SHM_HUGETLB);
  if (shmid == -1) {
    switch (errno) {
      case EACCES:
        hrd_red_printf(
            "HRD: SHM malloc error: Insufficient permissions."
            " (SHM key = %d)\n",
            shm_key);
        break;
      case EEXIST:
        hrd_red_printf(
            "HRD: SHM malloc error: Already exists."
            " (SHM key = %d)\n",
            shm_key);
        break;
      case EINVAL:
        hrd_red_printf(
            "HRD: SHM malloc error: SHMMAX/SHMIN mismatch."
            " (SHM key = %d, size = %d)\n",
            shm_key, size);
        break;
      case ENOMEM:
        hrd_red_printf(
            "HRD: SHM malloc error: Insufficient memory."
            " (SHM key = %d, size = %d)\n",
            shm_key, size);
        break;
      default:
        hrd_red_printf("HRD: SHM malloc error: A wild SHM error: %s.\n",
                       strerror(errno));
        break;
    }
    assert(false);
  }

  uint8_t* buf = static_cast<uint8_t*>(shmat(shmid, nullptr, 0));
  if (buf == nullptr) {
    printf("HRD: SHM malloc error: shmat() failed for key %d\n", shm_key);
    exit(-1);
  }

  // Bind the buffer to this socket
  const unsigned long nodemask = (1ull << socket_id);
  int ret = mbind(buf, size, MPOL_BIND, &nodemask, 32, 0);
  if (ret != 0) {
    printf("HRD: SHM malloc error. mbind() failed for key %d\n", shm_key);
    exit(-1);
  }

  return buf;
}

// Free shm @shm_key and @shm_buf. Return 0 on success, else -1.
int hrd_free(int shm_key, void* shm_buf) {
  int ret;
  int shmid = shmget(shm_key, 0, 0);
  if (shmid == -1) {
    switch (errno) {
      case EACCES:
        printf(
            "HRD: SHM free error: Insufficient permissions."
            " (SHM key = %d)\n",
            shm_key);
        break;
      case ENOENT:
        printf("HRD: SHM free error: No such SHM key. (SHM key = %d)\n",
               shm_key);
        break;
      default:
        printf("HRD: SHM free error: A wild SHM error: %s\n", strerror(errno));
        break;
    }
    return -1;
  }

  ret = shmctl(shmid, IPC_RMID, nullptr);
  if (ret != 0) {
    printf("HRD: SHM free error: shmctl() failed for key %d\n", shm_key);
    exit(-1);
  }

  ret = shmdt(shm_buf);
  if (ret != 0) {
    printf("HRD: SHM free error: shmdt() failed for key %d\n", shm_key);
    exit(-1);
  }

  return 0;
}

// Like printf, but red. Limited to 1000 characters.
void hrd_red_printf(const char* format, ...) {
#define RED_LIM 1000
  va_list args;
  int i;

  char buf1[RED_LIM], buf2[RED_LIM];
  memset(buf1, 0, RED_LIM);
  memset(buf2, 0, RED_LIM);

  va_start(args, format);

  // Marshal the stuff to print in a buffer
  vsnprintf(buf1, RED_LIM, format, args);

  // Probably a bad check for buffer overflow
  for (i = RED_LIM - 1; i >= RED_LIM - 50; i--) {
    assert(buf1[i] == 0);
  }

  // Add markers for red color and reset color
  snprintf(buf2, 1000, "\033[31m%s\033[0m", buf1);

  // Probably another bad check for buffer overflow
  for (i = RED_LIM - 1; i >= RED_LIM - 50; i--) {
    assert(buf2[i] == 0);
  }

  printf("%s", buf2);

  va_end(args);
}

void hrd_nano_sleep(size_t ns) {
  size_t start = hrd_get_cycles();
  size_t end = start;
  size_t upp = (2.1 * ns);
  while (end - start < upp) end = hrd_get_cycles();
}

// Get the LID of a port on the device specified by @ctx
uint16_t hrd_get_local_lid(struct ibv_context* ctx, int dev_port_id) {
  assert(ctx != nullptr && dev_port_id >= 1);

  struct ibv_port_attr attr;
  if (ibv_query_port(ctx, dev_port_id, &attr)) {
    printf("HRD: ibv_query_port on port %d of device %s failed! Exiting.\n",
           dev_port_id, ibv_get_device_name(ctx->device));
    assert(false);
  }

  return attr.lid;
}

// Return the environment variable @name if it is set. Exit if not.
char* hrd_getenv(const char* name) {
  char* env = getenv(name);
  if (env == nullptr) {
    fprintf(stderr, "Environment variable %s not set\n", name);
    assert(false);
  }

  return env;
}

// Record the current time in @timebuf. @timebuf must have at least 50 bytes.
void hrd_get_formatted_time(char* timebuf) {
  assert(timebuf != nullptr);
  time_t timer;
  struct tm* tm_info;

  time(&timer);
  tm_info = localtime(&timer);

  strftime(timebuf, 26, "%Y:%m:%d %H:%M:%S", tm_info);
}

memcached_st* hrd_create_memc() {
  memcached_server_st* servers = nullptr;
  memcached_st* memc = memcached_create(nullptr);
  memcached_return rc;

  memc = memcached_create(nullptr);
  char* registry_ip = hrd_getenv("HRD_REGISTRY_IP");

  // We run the memcached server on the default memcached port
  servers = memcached_server_list_append(servers, registry_ip,
                                         MEMCACHED_DEFAULT_PORT, &rc);
  rc = memcached_server_push(memc, servers);
  rt_assert(rc == MEMCACHED_SUCCESS, "Couldn't add memcached server");

  memcached_server_list_free(servers);
  return memc;
}

void hrd_close_memcached() {
  assert(memc != nullptr);
  memcached_free(memc);
}

// Insert key -> value mapping into memcached running at HRD_REGISTRY_IP.
void hrd_publish(const char* key, void* value, size_t len) {
  assert(key != nullptr && value != nullptr && len > 0);
  if (memc == nullptr) memc = hrd_create_memc();

  memcached_return rc;
  rc = memcached_set(memc, key, strlen(key),
                     reinterpret_cast<const char*>(value), len,
                     static_cast<time_t>(0), static_cast<uint32_t>(0));
  if (rc != MEMCACHED_SUCCESS) {
    char* registry_ip = hrd_getenv("HRD_REGISTRY_IP");
    fprintf(stderr,
            "\tHRD: Failed to publish key %s. Error %s. "
            "Reg IP = %s\n",
            key, memcached_strerror(memc, rc), registry_ip);
    exit(-1);
  }
}

// Get the value associated with "key" into "value", and return the length
// of the value. If the key is not found, return nullptr and len -1. For all
// other errors, terminate.
//
// This function sometimes gets called in a polling loop - ensure that there
// are no memory leaks or unterminated memcached connections! We don't need
// to free() the resul of getenv() since it points to a string in the process
// environment.
int hrd_get_published(const char* key, void** value) {
  assert(key != nullptr);
  if (memc == nullptr) memc = hrd_create_memc();

  memcached_return rc;
  size_t value_length;
  uint32_t flags;

  *value = memcached_get(memc, key, strlen(key), &value_length, &flags, &rc);

  if (rc == MEMCACHED_SUCCESS) {
    return static_cast<int>(value_length);
  } else if (rc == MEMCACHED_NOTFOUND) {
    assert(*value == nullptr);
    return -1;
  } else {
    char* registry_ip = hrd_getenv("HRD_REGISTRY_IP");
    fprintf(stderr,
            "HRD: Error finding value for key \"%s\": %s. "
            "Reg IP = %s\n",
            key, memcached_strerror(memc, rc), registry_ip);
    exit(-1);
  }

  // Never reached
  assert(false);
}

// To advertise a queue pair with name qp_name as ready, we publish this
// key-value mapping: "HRD_RESERVED_NAME_PREFIX-qp_name" -> "hrd_ready". This
// requires that a qp_name never starts with HRD_RESERVED_NAME_PREFIX.
//
// This avoids overwriting the memcached entry for qp_name which might still
// be needed by the remote peer.
void hrd_publish_ready(const char* qp_name) {
  char value[kHrdQPNameSize];
  assert(qp_name != nullptr && strlen(qp_name) < kHrdQPNameSize);

  char new_name[2 * kHrdQPNameSize];
  sprintf(new_name, "%s", kHrdReservedNamePrefix);
  strcat(new_name, qp_name);

  sprintf(value, "%s", "hrd_ready");
  hrd_publish(new_name, value, strlen(value));
}

// To check if a queue pair with name qp_name is ready, we check if this
// key-value mapping exists: "HRD_RESERVED_NAME_PREFIX-qp_name" -> "hrd_ready".
void hrd_wait_till_ready(const char* qp_name) {
  char* value;
  char exp_value[kHrdQPNameSize];
  sprintf(exp_value, "%s", "hrd_ready");

  char new_name[2 * kHrdQPNameSize];
  sprintf(new_name, "%s", kHrdReservedNamePrefix);
  strcat(new_name, qp_name);

  int tries = 0;
  while (true) {
    int ret = hrd_get_published(new_name, reinterpret_cast<void**>(&value));
    tries++;
    if (ret > 0) {
      if (strcmp(value, exp_value) == 0) {
        free(value);
        return;
      }
    }

    free(value);
    usleep(200000);

    if (tries > 100) {
      fprintf(stderr, "HRD: Waiting for QP %s to be ready\n", qp_name);
      tries = 0;
    }
  }
}

void hrd_post_dgram_recv(struct ibv_qp* qp, void* buf_addr, size_t len,
                         uint32_t lkey) {
  int ret;
  struct ibv_recv_wr* bad_wr;

  struct ibv_sge list;
  memset(&list, 0, sizeof(struct ibv_sge));
  list.length = len;
  list.lkey = lkey;

  struct ibv_recv_wr recv_wr;
  memset(&recv_wr, 0, sizeof(struct ibv_recv_wr));
  recv_wr.sg_list = &list;
  recv_wr.num_sge = 1;
  recv_wr.sg_list->addr = reinterpret_cast<uint64_t>(buf_addr);

  ret = ibv_post_recv(qp, &recv_wr, &bad_wr);
  if (ret) {
    fprintf(stderr, "HRD: Error %d posting datagram recv.\n", ret);
    exit(-1);
  }
}

void hrd_bind_to_core(std::thread& thread, size_t n) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(n, &cpuset);
  int rc = pthread_setaffinity_np(thread.native_handle(), sizeof(cpu_set_t),
                                  &cpuset);
  if (rc != 0) {
    throw std::runtime_error("Error setting thread affinity.\n");
  }
}
