/*
 * Copyright (c) 2015 Mellanox Technologies, Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef EC_H
#define EC_H

#include "mlx5.h"

#define EC_ACK_NEVENTS		100
#define EC_POLL_BATCH		4
#define EC_POLL_BUDGET		65536

/* CQ factor, fit 1 recv completion and 3 send completions */
#define MLX5_EC_CQ_FACTOR	4

/*
 * maximum WQE_BBs per EC operation:
 * - output sges UMR: 4 BBs (ctrl + umr_ctrl(1), mkey_ctx(1), pattern ds(2))
 * - input sges UMR:  6 BBs (ctrl + umr_ctrl(1), mkey_ctx(1), klm ds(4))
 * - vector calc: 1 BB
 */
#define MLX5_EC_MAX_WQE_BBS	11

#define MLX5_CHUNK_SIZE(calc)	64 * (1 << calc->log_chunk_size)
#define MLX5_EC_NOUTPUTS(m)	(m == 3 ? 4 : m)

struct mlx5_ec_decode {
	struct ibv_sge		sge;
	struct list_head	node;
};

struct mlx5_ec_decode_pool {
	struct mlx5_lock	lock;
	uint8_t                 *decode_buf;
	struct ibv_mr		*decode_mr;
	struct mlx5_ec_decode	*decodes;
	struct list_head	list;
};

struct mlx5_ec_calc {
	struct ibv_exp_ec_calc	ibcalc;
	struct ibv_pd		*pd;
	struct ibv_qp		*qp;
	struct ibv_cq		*cq;
	struct ibv_wc		wcs[EC_POLL_BATCH];
	struct ibv_comp_channel *channel;
	uint8_t			log_chunk_size;
	uint16_t		cq_count;
	struct ibv_mr		*outumr;
	struct ibv_mr		*inumr;
	uint8_t			*mat;
	struct ibv_mr		*mat_mr;
	struct mlx5_ec_decode_pool decode_pool;
	pthread_t		ec_poller;
	int			stop_ec_poller;
	uint8_t			*dump;
	struct ibv_mr		*dump_mr;
	int			k;
	int			m;
	int			max_inflight_calcs;
};

static inline struct mlx5_ec_calc *to_mcalc(struct ibv_exp_ec_calc *ec_calc)
{
	return (void *)ec_calc - offsetof(struct mlx5_ec_calc, ibcalc);
}

struct mlx5_ec_comp {
	struct ibv_exp_ec_comp	comp;
	pthread_mutex_t		mutex;
	pthread_cond_t		cond;
};

static inline struct mlx5_ec_comp *to_mcomp(struct ibv_exp_ec_comp *ec_comp)
{
	return (void *)ec_comp - offsetof(struct mlx5_ec_comp, comp);
}

struct ibv_exp_ec_calc *
mlx5_alloc_ec_calc(struct ibv_pd *pd,
		   struct ibv_exp_ec_calc_init_attr *attr);

void
mlx5_dealloc_ec_calc(struct ibv_exp_ec_calc *ec_calc);

int mlx5_ec_encode_async(struct ibv_exp_ec_calc *ec_calc,
			 struct ibv_exp_ec_mem *ec_mem,
			 struct ibv_exp_ec_comp *ec_comp);

int mlx5_ec_encode_sync(struct ibv_exp_ec_calc *ec_calc,
			struct ibv_exp_ec_mem *ec_mem);

int mlx5_ec_decode_async(struct ibv_exp_ec_calc *ec_calc,
			 struct ibv_exp_ec_mem *ec_mem,
			 uint32_t erasures,
			 uint8_t *decode_matrix,
			 struct ibv_exp_ec_comp *ec_comp);

int mlx5_ec_decode_sync(struct ibv_exp_ec_calc *ec_calc,
			struct ibv_exp_ec_mem *ec_mem,
			uint32_t erasures,
			uint8_t *decode_matrix);

int mlx5_ec_poll(struct ibv_exp_ec_calc *ec_calc, int n);

int mlx5_ec_encode_send(struct ibv_exp_ec_calc *ec_calc,
			struct ibv_exp_ec_mem *ec_mem,
			struct ibv_exp_ec_stripe *data_stripes,
			struct ibv_exp_ec_stripe *code_stripes);
#endif /* EC_H */
