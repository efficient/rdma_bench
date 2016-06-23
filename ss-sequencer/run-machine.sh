#!/bin/bash
source $(dirname $0)/../scripts/helpers.sh
drop_shm

export HRD_REGISTRY_IP="10.113.1.47"
export MLX5_SINGLE_THREADED=1

if [ "$#" -ne 1 ]; then
    blue "Illegal number of parameters"
	blue "Usage: ./run-machine.sh <machine_number>"
	exit
fi

num_threads=14			# Threads per client machine

blue "Running $num_threads client threads"

sudo LD_LIBRARY_PATH=/usr/local/lib/ -E \
	numactl --cpunodebind=0 --membind=0 ./main \
	--num-threads $num_threads \
	--dual-port 1 \
	--is-client 1 \
	--machine-id $1 \
	--postlist 16 \

#debug: run --num-threads 1 --dual-port 1 --is-client 1 --machine-id 0 --size 32 --window 128
