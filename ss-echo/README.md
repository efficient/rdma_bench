 * The server machine runs `NUM_SERVER_THREADS` threads. Client `i` issues
   send()s to server thread `i % NUM_SERVER_THREADS`. In dual-port mode, both
   server `i` and client `i` create all their QPs on port `i % 2`.
