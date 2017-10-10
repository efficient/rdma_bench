# A function to echo in blue color
function blue() {
	es=`tput setaf 4`
	ee=`tput sgr0`
	echo "${es}$1${ee}"
}

export HRD_REGISTRY_IP="10.113.1.47"
export MLX5_SINGLE_THREADED=1
#export MLX_QP_ALLOC_TYPE="HUGE"
#export MLX_CQ_ALLOC_TYPE="HUGE"

blue "Removing SHM keys 0--32"
for i in `seq 0 32`; do
	sudo ipcrm -M $i
done

: ${HRD_REGISTRY_IP:?"Need to set HRD_REGISTRY_IP non-empty"}

blue "Reset server QP registry"
sudo killall memcached
memcached -l 0.0.0.0 1>/dev/null 2>/dev/null &
sleep 1

blue "Starting server threads"

sudo LD_LIBRARY_PATH=/usr/local/lib/ -E \
	numactl --cpunodebind=0 --membind=0 ./main \
	--dual-port 1 \
	--use-uc 0 \
	--run-time 10 \
	--is-client 0 \
	--size 32 \
	--do-read 1 &
