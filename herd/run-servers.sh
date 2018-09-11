# A function to echo in blue color
function blue() {
	es=`tput setaf 4`
	ee=`tput sgr0`
	echo "${es}$1${ee}"
}

export HRD_REGISTRY_IP="10.113.1.47"
export MLX5_SINGLE_THREADED=1
export MLX4_SINGLE_THREADED=1

blue "Removing SHM key 24 (request region hugepages)"
sudo ipcrm -M 24

blue "Removing SHM keys used by MICA"
for i in `seq 0 28`; do
	key=`expr 3185 + $i`
	sudo ipcrm -M $key 2>/dev/null
	key=`expr 4185 + $i`
	sudo ipcrm -M $key 2>/dev/null
done

blue "Reset server QP registry"
sudo pkill memcached
memcached -l 0.0.0.0 1>/dev/null 2>/dev/null &
sleep 1

blue "Starting master process"
sudo LD_LIBRARY_PATH=/usr/local/lib/ -E \
	numactl --cpunodebind=0 --membind=0 ./main \
	--master 1 \
	--base-port-index 0 \
	--num-server-ports 2 &

# Give the master process time to create and register per-port request regions
sleep 1

blue "Starting worker threads"
sudo LD_LIBRARY_PATH=/usr/local/lib/ -E \
	numactl --cpunodebind=0 --membind=0 ./main \
	--is-client 0 \
	--base-port-index 0 \
	--num-server-ports 2 \
	--postlist 32 &
