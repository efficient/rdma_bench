# A function to echo in blue color
function blue() {
	es=`tput setaf 4`
	ee=`tput sgr0`
	echo "${es}$1${ee}"
}

blue "Removing hugepages with SHM keys 24 and 25"
sudo ipcrm -M 24
sudo ipcrm -M 25

num_worker_threads=5
: ${HRD_REGISTRY_IP:?"Need to set HRD_REGISTRY_IP non-empty"}

blue "Reset server QP registry"
sudo pkill memcached
memcached -l 0.0.0.0 1>/dev/null 2>/dev/null &
sleep 1

blue "Starting master process"
sudo LD_LIBRARY_PATH=/usr/local/lib/ -E \
	numactl --cpunodebind=0 --membind=0 ./main \
	--master 1 \
	--dual-port 1 &

# Allow the master process to create an register request regions
sleep 1

blue "Starting $num_worker_threads worker threads"
sudo LD_LIBRARY_PATH=/usr/local/lib/ -E \
	numactl --cpunodebind=0 --membind=0 ./main \
	--num-threads $num_worker_threads \
	--dual-port 1 \
	--is-client 0 \
	--size 24 \
	--postlist 32 &

#numactl --physcpubind=0,2,4,6,8,10,12,14 --membind=0 ./main \
