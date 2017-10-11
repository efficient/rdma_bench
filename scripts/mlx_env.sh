#!/usr/bin/env bash
# Config for Mellanox userspace driver

export MLX4_SINGLE_THREADED=1
export MLX5_SINGLE_THREADED=1
export MLX_QP_ALLOC_TYPE="HUGE"
export MLX_CQ_ALLOC_TYPE="HUGE"
