# Description
This benchmark measures the affect of the number of queue pairs on sender
throughput. Each server thread connects to many clients.

## Connection logic
This benchmark is for a single NIC. If `dual_port == 1`,
each thread (client or server) uses port `id % 2`. In this case, server threads
connect to clients whose ID is the same as the server's modulo 2.

## Outstanding operations logic
For READs, we limit the number of outstanding
operations per-thread to `WINDOW_SIZE`. This is because too many outstanding
requests can cause WQE cache misses, and this benchmark focuses on QP cache
misses.

A similar restriction is harder for WRITEs because we cannot detect completion
by polling on memory. For WRITEs, we have a reasonable limit on the number of
outstanding operations per-QP (i.e., around `2 * UNSIG_BATCH`), not per-thread.
The per-thread limit is around `2 * UNSIG_BATCH * NUM_CLIENTS`.

## Testing with low fanout
The `rw-tput-sender` benchmark should be used to measure outbound performance
with low QP fanout.
