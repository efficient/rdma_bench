# A function to echo in blue color
function blue() {
	es=`tput setaf 4`
	ee=`tput sgr0`
	echo "${es}$1${ee}"
}

blue "Removing hugepages"
shm-rm.sh 1>/dev/null 2>/dev/null

: ${HRD_REGISTRY_IP:?"Need to set HRD_REGISTRY_IP non-empty"}

blue "Reset server QP registry"
sudo pkill memcached
memcached -l 0.0.0.0 1>/dev/null 2>/dev/null &
sleep 1

blue "Starting $num_server_threads server threads"

sudo LD_LIBRARY_PATH=/usr/local/lib/ -E \
	numactl --physcpubind=0 ./main \
	--num-threads 1 \
	--dual-port 0 \
	--use-uc 0 \
	--is-client 0 \
	--size 8 \
	--postlist 16 \
	--do-read 0 &
