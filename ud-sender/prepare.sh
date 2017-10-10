#!/bin/bash
source $(dirname $0)/../scripts/helpers.sh

# Prepare for debugging

echo "Checking HRD_REGISTRY_IP"
: ${HRD_REGISTRY_IP:?"Need to set HRD_REGISTRY_IP non-empty"}

echo "Removing SHM keys"
drop_shm

echo "Restarting memcached"
sudo killall memcached
memcached -l 0.0.0.0 1>/dev/null 2>/dev/null &

