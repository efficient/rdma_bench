# libmlx5 for rdma_bench
A modified driver for Connect-IB and ConnectX-4 instrumented to measure PCIe
bandwidth use. This driver is based on `libmlx5-1.0.2mlnx1` from Mellanox OFED
3.2.

## Instructions to install
Warning: This replaces the existing driver.
```
	./autogen.sh
	./configure
	make
	sudo cp src/.libs/*-rdmav2.so /usr/lib/libibverbs/
```

## Implementation details
 * Base commit = f6dfece0176149bfd6c414475e75bcaedb6d830b
 * Initialization for stats is done during driver initialization in
   `mlx5_driver_init()`. This function gets called once per multi-threaded
   process.
 * To avoid modifying the `verbs.h` header file, we use a rarely-used verbs
   function to print stats. The user calls `ibv_detach_mcast(*qp, *gid, lid)`
   with a valid queue pair pointer, `gid = NULL`, and `lid = 0xffff`. Using a
   valid GID and LID lead to regular behavior.

## Limitations
 * Assumptions:
    * RDMA reads are not issued.
    * RDMA write and SEND payloads are inlined.
    * PCIe MaxPayloadSize and MaxReadReq ignored. (For payloads that can be
      inlined, these parameters should not matter.)
    * Inline RECV and CQE compression is disabled.
 * The measured PCIe bandwidth use is fairly, but not completely, accurate.
   Using hardware PCIe counters can give more confidence in a measurement.
   Two factors that are impossible to account for at the driver, and lead to
   measurement inaccuracy, are:
    * **RECV descriptor fetch**: The number of RECV descriptors fetched by the
      NIC in one DMA read depends on the NIC firmware and workload. We assume
      that all the RECV descriptors posted in one `post_recv()` call will be
      fetched in one DMA read.
    * **WQE re-fetch**: When BlueFlame is enabled and postlist is used, the
      driver writes the last WQE in the postlist to the NIC using MMIO. We
      assume that the NIC re-reads this WQE later. This is not always true, and
      can be checked using PCIe counters.
