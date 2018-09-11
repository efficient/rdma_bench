# A function to echo in blue color
function blue() {
	es=`tput setaf 4`
	ee=`tput sgr0`
	echo "${es}$1${ee}"
}

blue "Removing hugepages"
shm-rm.sh 1>/dev/null 2>/dev/null

blue "Removing hugepage used by server's registered buffer"
sudo ipcrm -M 24

num_server_threads=16
#num_client_machines=1
: ${HRD_REGISTRY_IP:?"Need to set HRD_REGISTRY_IP non-empty"}

blue "Reset server QP registry"
sudo pkill memcached
memcached -l 0.0.0.0 1>/dev/null 2>/dev/null &
sleep 1

blue "Starting $num_server_threads server threads"

sudo LD_LIBRARY_PATH=/usr/local/lib/ -E \
	numactl --cpunodebind=0 --membind=0 ./main \
	--num-threads $num_server_threads \
	--dual-port 0 \
	--use-uc 0 \
	--is-client 0 \
	--size 8 \
	--window 64 \
	--do-read 1 &
