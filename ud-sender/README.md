# UD SEND throughput
A benchmark to measure throughput of outbound UD SENDs.

## Important parameters
These parameters are defined in `main.h`, `run-servers.sh`, and `run-machine.sh`.

1. Client-server configuration
  * `num_server_threads`: Number of worker threads at the server machine
  * `NUM_CLIENTS`: Total number of client threads used
  * `num_threads`: Number of client threads at each client machine
2. RDMA optimizations
  * `postlist`: Number of SENDs issued by a server using one Doorbell
  * `NUM_UD_QPS`: Number of UD QPs used by each server thread for SENDs
  * `UNSIG_BATCH`: One SEND in every `UNSIG_BATCH` responses is signaled

## Running the benchmark
1. At the server machine: `./run-servers.sh`
2. Total number of client machines needed is `C = NUM_CLIENTS / num_threads`
   in single-port mode, and `C = 2 * NUM_CLIENTS / num_threads` in dual-port
   mode.
2. At machine `i` of `C` client machines: `./run-machine.sh i`

## Connection logic
 * Each server thread expects to connect with `NUM_CLIENTS` clients.
 * In single-port mode, each server connects with all clients.
 * In dual-port mode, a client with global ID = `C` connects only with servers
   whose global ID `S` satisfies `S = C mod 2`. Therefore, we need to launch
   `NUM_CLIENTS * 2` client threads.
