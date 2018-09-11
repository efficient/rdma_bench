#!/bin/bash

# If there are no arguments, the script is being called from command line
if [ "$#" -eq 0 ]; then
	# Running from the command line
	sudo pkill main
	sudo pkill memcached
	exit
fi

#
# If we are here, there are arguments
#

# Check status, but do not kill
if [[ $1 == "status" ]]; then
	main_ps_lines=`ps -afx | grep main | wc -l`

	if [ $main_ps_lines = "1" ]; then
		main_running="NO"
	else
		main_running="YES"
	fi

	ip=`ifconfig | grep -Eo 'inet (addr:)?([0-9]*\.){3}[0-9]*' | grep -Eo '([0-9]*\.){3}[0-9]*' | grep -v '127.0.0.1' | head -1`
	echo "Main running on $ip = $main_running (main_ps_lines = $main_ps_lines)"

fi

# Kill quietly
if [[ $1 == "quiet" ]]; then
	sudo pkill main 1>/dev/null 2>/dev/null
	sudo pkill memcached 1>/dev/null 2>/dev/null
fi
