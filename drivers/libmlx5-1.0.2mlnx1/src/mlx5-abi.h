/*
 * Copyright (c) 2012 Mellanox Technologies, Inc.  All rights reserved.
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

#ifndef MLX5_ABI_H
#define MLX5_ABI_H

#include <infiniband/kern-abi.h>

#define MLX5_UVERBS_MIN_ABI_VERSION	1
#define MLX5_UVERBS_MAX_ABI_VERSION	1

enum {
	MLX5_QP_FLAG_SIGNATURE		= 1 << 0,
};

enum {
	MLX5_RWQ_FLAG_SIGNATURE		= 1 << 0,
};

enum {
	MLX5_NUM_UUARS_PER_PAGE = 2,
	MLX5_MAX_UAR_PAGES	= 1 << 8,
	MLX5_MAX_UUARS		= MLX5_MAX_UAR_PAGES * MLX5_NUM_UUARS_PER_PAGE,
	MLX5_DEF_TOT_UUARS	= 8 * MLX5_NUM_UUARS_PER_PAGE,
};

struct mlx5_alloc_ucontext {
	struct ibv_get_context		ibv_req;
	__u32				total_num_uuars;
	__u32				num_low_latency_uuars;
	__u32				flags;
	__u32				reserved;
};

struct mlx5_alloc_ucontext_resp {
	struct ibv_get_context_resp	ibv_resp;
	__u32				qp_tab_size;
	__u32				bf_reg_size;
	__u32				tot_uuars;
	__u32				cache_line_size;
	__u16				max_sq_desc_sz;
	__u16				max_rq_desc_sz;
	__u32				max_send_wqebb;
	__u32				max_recv_wr;
	__u32				max_srq_recv_wr;
	__u16				num_ports;
	__u16				reserved;
	__u32				max_desc_sz_sq_dc;
	__u32				atomic_sizes_dc;
	__u32				reserved1;
	__u32				flags;
	__u32				reserved2[5];
};

enum mlx5_exp_alloc_context_resp_mask {
	MLX5_EXP_ALLOC_CTX_RESP_MASK_CQE_COMP_MAX_NUM		= 1 << 0,
	MLX5_EXP_ALLOC_CTX_RESP_MASK_CQE_VERSION		= 1 << 1,
	MLX5_EXP_ALLOC_CTX_RESP_MASK_RROCE_UDP_SPORT_MIN	= 1 << 2,
	MLX5_EXP_ALLOC_CTX_RESP_MASK_RROCE_UDP_SPORT_MAX	= 1 << 3,
	MLX5_EXP_ALLOC_CTX_RESP_MASK_HCA_CORE_CLOCK_OFFSET	= 1 << 4,
};

struct mlx5_exp_alloc_ucontext_data_resp {
	__u32   comp_mask; /* use mlx5_exp_alloc_context_resp_mask */
	__u16   cqe_comp_max_num;
	__u8    cqe_version;
	__u8    reserved;
	__u16	rroce_udp_sport_min;
	__u16	rroce_udp_sport_max;
	__u32	hca_core_clock_offset;
};

struct mlx5_exp_alloc_ucontext_resp {
	struct ibv_get_context_resp			ibv_resp;
	__u32						qp_tab_size;
	__u32						bf_reg_size;
	__u32						tot_uuars;
	__u32						cache_line_size;
	__u16						max_sq_desc_sz;
	__u16						max_rq_desc_sz;
	__u32						max_send_wqebb;
	__u32						max_recv_wr;
	__u32						max_srq_recv_wr;
	__u16						num_ports;
	__u16						reserved;
	__u32						max_desc_sz_sq_dc;
	__u32						atomic_sizes_dc;
	__u32						reseved1;
	__u32						flags;
	__u32						reserved2[5];
	/* Some more reserved fields for future growth of
	 * mlx5_alloc_ucontext_resp */
	__u64						prefix_reserved[8];

	struct mlx5_exp_alloc_ucontext_data_resp	exp_data;
};

struct mlx5_alloc_pd_resp {
	struct ibv_alloc_pd_resp	ibv_resp;
	__u32				pdn;
};

struct mlx5_create_cq {
	struct ibv_create_cq		ibv_cmd;
	__u64				buf_addr;
	__u64				db_addr;
	__u32				cqe_size;
};

struct mlx5_create_cq_resp {
	struct ibv_create_cq_resp	ibv_resp;
	__u32				cqn;
	__u32				reserved;
};

enum mlx5_exp_creaet_cq_mask {
		MLX5_EXP_CREATE_CQ_MASK_CQE_COMP_EN		= 1 << 0,
		MLX5_EXP_CREATE_CQ_MASK_CQE_COMP_RECV_TYPE	= 1 << 1,
		MLX5_EXP_CREATE_CQ_MASK_RESERVED		= 1 << 2,
};

enum mlx5_exp_cqe_comp_recv_type {
	MLX5_CQE_FORMAT_HASH,
	MLX5_CQE_FORMAT_CSUM,
};

struct mlx5_exp_create_cq_data {
	__u32   comp_mask; /* use mlx5_exp_creaet_cq_mask */
	__u8    cqe_comp_en;
	__u8    cqe_comp_recv_type; /* use mlx5_exp_cqe_comp_recv_type */
	__u16   reserved;
};

struct mlx5_exp_create_cq {
	struct ibv_exp_create_cq	ibv_cmd;
	__u64				buf_addr;
	__u64				db_addr;
	__u32				cqe_size;
	__u32				reserved;
	/* Some more reserved fields for future growth of mlx5_create_cq */
	__u64   prefix_reserved[8];

	/* sizeof prefix aligned with mlx5_create_cq */
	__u64   size_of_prefix;

	struct mlx5_exp_create_cq_data exp_data;
};

struct mlx5_create_srq {
	struct ibv_create_srq		ibv_cmd;
	__u64				buf_addr;
	__u64				db_addr;
	__u32				flags;
};

struct mlx5_create_srq_resp {
	struct ibv_create_srq_resp	ibv_resp;
	__u32				srqn;
	__u32				reserved;
};

struct mlx5_create_srq_ex {
	struct ibv_create_xsrq		ibv_cmd;
	__u64				buf_addr;
	__u64				db_addr;
	__u32				flags;
	__u32				reserved;
	__u32                           uidx;
	__u32                           reserved1;
};

struct mlx5_drv_create_qp {
	__u64				buf_addr;
	__u64				db_addr;
	__u32				sq_wqe_count;
	__u32				rq_wqe_count;
	__u32				rq_wqe_shift;
	__u32				flags;
};

enum mlx5_exp_drv_create_qp_mask {
	MLX5_EXP_CREATE_QP_MASK_UIDX		= 1 << 0,
	MLX5_EXP_CREATE_QP_MASK_SQ_BUFF_ADD	= 1 << 1,
	MLX5_EXP_CREATE_QP_MASK_WC_UAR_IDX	= 1 << 2,
	MLX5_EXP_CREATE_QP_MASK_FLAGS_IDX	= 1 << 3,
	MLX5_EXP_CREATE_QP_MASK_RESERVED	= 1 << 4,
};

enum mlx5_exp_create_qp_flags {
	MLX5_EXP_CREATE_QP_MULTI_PACKET_WQE_REQ_FLAG = 1 << 0,
};

enum mlx5_exp_drv_create_qp_uar_idx {
	MLX5_EXP_CREATE_QP_DB_ONLY_UUAR = -1
};

struct mlx5_exp_drv_create_qp_data {
	__u32   comp_mask; /* use mlx5_exp_ib_create_qp_mask */
	__u32   uidx;
	__u64	sq_buf_addr;
	__u32   wc_uar_index;
	__u32   flags; /* use mlx5_exp_create_qp_flags */
};

struct mlx5_exp_drv_create_qp {
	/* To allow casting to mlx5_drv_create_qp the prefix is the same as
	 * struct mlx5_drv_create_qp prefix
	 */
	__u64	buf_addr;
	__u64	db_addr;
	__u32	sq_wqe_count;
	__u32	rq_wqe_count;
	__u32	rq_wqe_shift;
	__u32	flags;

	/* Some more reserved fields for future growth of mlx5_drv_create_qp */
	__u64   prefix_reserved[8];

	/* sizeof prefix aligned with mlx5_drv_create_qp */
	__u64   size_of_prefix;

	/* Experimental data
	 * Add new experimental data only inside the exp struct
	 */
	struct mlx5_exp_drv_create_qp_data exp;
};

struct mlx5_create_qp {
	struct ibv_create_qp		ibv_cmd;
	struct mlx5_drv_create_qp	drv;
};

enum {
	MLX5_EXP_INVALID_UUAR = (-1),
};

struct mlx5_create_qp_resp {
	struct ibv_create_qp_resp	ibv_resp;
	__u32				uuar_index;
	__u32				rsvd;
};

struct mlx5_exp_create_qp {
	struct ibv_exp_create_qp	ibv_cmd;
	struct mlx5_exp_drv_create_qp	drv;
};

enum mlx5_exp_drv_create_qp_resp_mask {
	MLX5_EXP_CREATE_QP_RESP_MASK_FLAGS_IDX	= 1 << 0,
	MLX5_EXP_CREATE_QP_RESP_MASK_RESERVED	= 1 << 1,
};

enum mlx5_exp_create_qp_resp_flags {
	MLX5_EXP_CREATE_QP_RESP_MULTI_PACKET_WQE_FLAG = 1 << 0,
};

struct mlx5_exp_drv_create_qp_resp_data {
	__u32   comp_mask; /* use mlx5_exp_drv_create_qp_resp_mask */
	__u32   flags; /* use mlx5_exp_create_qp_resp_flags */
};


struct mlx5_exp_create_qp_resp {
	struct ibv_exp_create_qp_resp	ibv_resp;
	__u32				uuar_index;
	__u32				rsvd;

	/* Some more reserved fields for future growth of create qp resp */
	__u64   prefix_reserved[8];

	/* sizeof prefix aligned with create qp resp */
	__u64   size_of_prefix;

	/* Experimental data
	 * Add new experimental data only inside the exp struct
	 */
	struct mlx5_exp_drv_create_qp_resp_data exp;
};

struct mlx5_exp_drv_create_wq {
	__u64				buf_addr;
	__u64				db_addr;
	__u32				rq_wqe_count;
	__u32				rq_wqe_shift;
	__u32				user_index;
	__u32				flags;
};

struct mlx5_exp_create_wq {
	struct ibv_exp_create_wq	ibv_cmd;
	struct mlx5_exp_drv_create_wq	drv;
};

struct mlx5_exp_create_wq_resp {
	struct ibv_exp_create_wq_resp	ibv_resp;
};

struct mlx5_exp_modify_wq {
	struct ib_exp_modify_wq	ibv_cmd;
};

struct mlx5_exp_create_rwq_ind_table_resp {
	struct ibv_exp_create_rwq_ind_table_resp ibv_resp;
};

struct mlx5_exp_destroy_rwq_ind_table {
	struct ibv_exp_destroy_rwq_ind_table ibv_cmd;
};

struct mlx5_resize_cq {
	struct ibv_resize_cq		ibv_cmd;
	__u64				buf_addr;
	__u16				cqe_size;
	__u16				reserved0;
	__u32				reserved1;
};

struct mlx5_resize_cq_resp {
	struct ibv_resize_cq_resp	ibv_resp;
};

struct mlx5_drv_create_dct {
	__u32                           uidx;
	__u32                           reserved;
};

struct mlx5_create_dct {
	struct ibv_exp_create_dct	ibv_cmd;
	struct mlx5_drv_create_dct      drv;
};

struct mlx5_create_dct_resp {
	struct ibv_exp_create_dct_resp	ibv_resp;
};

struct mlx5_destroy_dct {
	struct ibv_exp_destroy_dct	ibv_cmd;
};

struct mlx5_destroy_dct_resp {
	struct ibv_exp_destroy_dct_resp	ibv_resp;
};

struct mlx5_query_dct {
	struct ibv_exp_query_dct	ibv_cmd;
};

struct mlx5_query_dct_resp {
	struct ibv_exp_query_dct_resp	ibv_resp;
};

struct mlx5_arm_dct {
	struct ibv_exp_arm_dct	ibv_cmd;
	__u64			reserved0;
	__u64			reserved1;
};

struct mlx5_arm_dct_resp {
	struct ibv_exp_arm_dct_resp	ibv_resp;
	__u64			reserved0;
	__u64			reserved1;
};

struct mlx5_query_mkey {
	struct ibv_exp_query_mkey	ibv_cmd;
};

struct mlx5_query_mkey_resp {
	struct ibv_exp_query_mkey_resp	ibv_resp;
};

struct mlx5_create_mr {
	struct ibv_exp_create_mr	ibv_cmd;
};

struct mlx5_create_mr_resp {
	struct ibv_exp_create_mr_resp	ibv_resp;
};

#endif /* MLX5_ABI_H */
