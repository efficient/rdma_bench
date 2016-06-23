#!/bin/bash
source $(dirname $0)/../scripts/helpers.sh

export HRD_REGISTRY_IP="10.113.1.47"
export MLX5_SINGLE_THREADED=1

drop_shm

num_server_threads=13
: ${HRD_REGISTRY_IP:?"Need to set HRD_REGISTRY_IP non-empty"}

blue "Reset server QP registry"
sudo killall memcached
memcached -l 0.0.0.0 1>/dev/null 2>/dev/null &
sleep 1

blue "Starting $num_server_threads server threads"

sudo LD_LIBRARY_PATH=/usr/local/lib/ -E \
	numactl --cpunodebind=0 --membind=0 ./main \
	--num-threads $num_server_threads \
	--dual-port 0 \
	--is-client 0 \
	--size 16 \
	--postlist 1 &
