#!/usr/bin/env bash
source $(dirname $0)/../scripts/utils.sh
source $(dirname $0)/../scripts/mlx_env.sh
export HRD_REGISTRY_IP="fawn-pluto0"

drop_shm

num_server_threads=32

blue "Reset server QP registry"
sudo killall memcached
memcached -l 0.0.0.0 1>/dev/null 2>/dev/null &
sleep 1

blue "Starting $num_server_threads server threads"

flags="
	--num_threads $num_server_threads \
	--dual_port 1 \
  --use_uc 1
"

# Check for non-gdb mode
if [ "$#" -eq 0 ]; then
  sudo LD_LIBRARY_PATH=/usr/local/lib/ -E \
    numactl --cpunodebind=0 --membind=0 ./main $flags
fi

# Check for gdb mode
if [ "$#" -eq 1 ]; then
  sudo LD_LIBRARY_PATH=/usr/local/lib/ -E \
    gdb -ex run --args ./main $flags
fi
