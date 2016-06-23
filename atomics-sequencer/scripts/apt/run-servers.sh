# A function to echo in blue color
function blue() {
	es=`tput setaf 4`
	ee=`tput sgr0`
	echo "${es}$1${ee}"
}

num_processes=3

for i in `seq 1 $num_processes`; do
	port=`expr 18515 + $i`
	blue "Running server $i on port $port"
	#ib_atomic_bw --ib-dev=mlx5_0 --run_infinitely --port=$port &
	ib_atomic_bw --run_infinitely --port=$port --qp 1 &
done

