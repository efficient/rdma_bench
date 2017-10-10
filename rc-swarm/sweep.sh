# A function to echo in blue color
function blue() {
	es=`tput setaf 4`
	ee=`tput sgr0`
	echo "${es}$1${ee}"
}

# Sweep over params
# This is separate from run-all.sh, which does not sweep

# Empty existing sweep output
rm -f sweep/temp_out

# 6 machines on NetApp, so increment NUM_WORKERS by 6
for VM_PER_MACHINE in 1 2 3 4 5 6 7 8 9; do
	for WINDOW_SIZE in `seq 8 8 32`; do
		for UNSIG_BATCH in 1; do
			for NUM_WORKERS in 154; do
				# Do work for these params
				rm sweep.h
				touch sweep.h

				echo "#define SIZE 32" >> sweep.h
				echo "#define VM_PER_MACHINE $VM_PER_MACHINE" >> sweep.h
				echo "#define WINDOW_SIZE $WINDOW_SIZE" >> sweep.h
				echo "#define NUM_WORKERS $NUM_WORKERS" >> sweep.h
				echo "#define UNSIG_BATCH $UNSIG_BATCH" >> sweep.h

				make clean
				make

				blue "Starting run"
				./run-all.sh
			done
		done
	done
done
