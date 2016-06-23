# A function to echo in blue color
function blue() {
	es=`tput setaf 4`
	ee=`tput sgr0`
	echo "${es}$1${ee}"
}

blue "Removing SHM keys used by MICA"
for i in `seq 0 16`; do
	key=`expr 3185 + $i`
	sudo ipcrm -M $key 2>/dev/null
	key=`expr 4185 + $i`
	sudo ipcrm -M $key 2>/dev/null
done

num_threads=1
sudo numactl --cpunodebind=0 --membind=0 ./test \
	--num-threads $num_threads
