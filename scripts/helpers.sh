#!/bin/bash

# A function to echo in blue color
function blue() {
	es=`tput setaf 4`
	ee=`tput sgr0`
	echo "${es}$1${ee}"
}

function drop_shm()
{
	echo "Dropping SHM entries"

	for i in $(ipcs -m | awk '{ print $1; }'); do
		if [[ $i =~ 0x.* ]]; then
			sudo ipcrm -M $i 2>/dev/null
		fi
	done
}

# Check if SHMMAX and SHMMIN are large. This doesn't work.
function check_shm_limits() {
	shmmax=`cat /proc/sys/kernel/shmmax`
	shmall=`cat /proc/sys/kernel/shmall`

	if [ "$shmmax" -lt 100000000000 ]; then
		blue "Error: SHMMAX too small"
		exit
	fi

	if [ "$shmall" -lt 100000000000 ]; then
		blue "Error: SHMALL too small"
		exit
	fi
}
