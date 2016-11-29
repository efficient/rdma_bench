# A function to echo in blue color
function blue() {
	es=`tput setaf 4`
	ee=`tput sgr0`
	echo "${es}$1${ee}"
}

export HRD_REGISTRY_IP="128.110.96.101"
export MLX5_SINGLE_THREADED=1

blue "Removing hugepages"
shm-rm.sh 1>/dev/null 2>/dev/null

num_server_threads=16
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
	--use-uc 0 \
	--is-client 0 &
