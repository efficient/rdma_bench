#!/bin/bash
source $(dirname $0)/../scripts/helpers.sh
drop_shm

export HRD_REGISTRY_IP="10.113.1.47"
export MLX5_SINGLE_THREADED=1

: ${HRD_REGISTRY_IP:?"Need to set HRD_REGISTRY_IP non-empty"}

blue "Reset server QP registry"
sudo pkill memcached
memcached -l 0.0.0.0 1>/dev/null 2>/dev/null &
sleep 1

sudo LD_LIBRARY_PATH=/usr/local/lib/ -E \
	numactl --cpunodebind=0 --membind=0 ./main \
	--dual-port 1 \
	--is-client 0 \
	--postlist 32 &
