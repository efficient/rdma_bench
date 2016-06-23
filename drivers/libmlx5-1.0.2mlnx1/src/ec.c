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

#include <signal.h>
#include "ec.h"
#include "doorbell.h"

static struct mlx5_ec_decode *
mlx5_get_ec_decode(struct mlx5_ec_calc *calc,
		   uint8_t *decode_matrix,
		   int k, int m)
{
	struct mlx5_ec_decode_pool *pool = &calc->decode_pool;
	struct mlx5_ec_decode *decode;
	uint8_t *buf;
	int cols = MLX5_EC_NOUTPUTS(m);
	int i, j;

	mlx5_lock(&pool->lock);
	decode = list_first_entry(&pool->list, struct mlx5_ec_decode, node);
	list_del(&decode->node);
	mlx5_unlock(&pool->lock);

	buf = (uint8_t *)(uintptr_t)decode->sge.addr;
	for (i = 0; i < k; i++)
		for (j = 0; j < cols; j++)
			/* Crazy HW formatting, bit 5 is on */
			buf[i*cols+j] = decode_matrix[i*m+j] | 0x10;

	/* Three outputs, zero the last column */
	if (m == 3)
		for (i = 0; i < k; i++)
			buf[i*cols+3] = 0x0;

	return decode;
}

static void
mlx5_put_ec_decode(struct mlx5_ec_calc *calc,
		   struct mlx5_ec_decode *decode)
{
	struct mlx5_ec_decode_pool *pool = &calc->decode_pool;

	mlx5_lock(&pool->lock);
	list_add(&decode->node, &pool->list);
	mlx5_unlock(&pool->lock);
}

static void handle_ec_comp(struct mlx5_ec_calc *calc, struct ibv_wc *wc)
{
	struct ibv_exp_ec_comp *ec_comp;

	if (wc->opcode == IBV_WC_SEND) {
		struct mlx5_ec_decode *decode =
			(struct mlx5_ec_decode *)(uintptr_t)wc->wr_id;
		if (decode)
			mlx5_put_ec_decode(calc, decode);
		return;
	}

	ec_comp = (struct ibv_exp_ec_comp *)(uintptr_t)wc->wr_id;
	if (ec_comp == NULL) {
		/* Caller didn't want a comp notification */
		return;
	}

	if (likely(wc->status == IBV_WC_SUCCESS))
		ec_comp->status = IBV_EXP_EC_CALC_SUCCESS;
	else
		ec_comp->status = IBV_EXP_EC_CALC_FAIL;

	ec_comp->done(ec_comp);
}

static int ec_poll_cq(struct mlx5_ec_calc *calc, int budget)
{
	int i, n, count = 0;

	while ((n = ibv_poll_cq(calc->cq, EC_POLL_BATCH, calc->wcs)) > 0) {
		if (unlikely(n < 0)) {
			fprintf(stderr, "poll CQ failed\n");
			return n;
		}

		for (i = 0; i < n; i++)
			handle_ec_comp(calc, &calc->wcs[i]);

		count += n;
		if (count >= budget)
			break;
	}

	return count;
}

static int mlx5_ec_poll_cq(struct mlx5_ec_calc *calc)
{
	struct ibv_cq *ev_cq;
	void *ev_ctx;
	int err, count;

	err = ibv_get_cq_event(calc->channel, &ev_cq, &ev_ctx);
	if (unlikely(err))
		return err;

	if (unlikely(ev_cq != calc->cq)) {
		fprintf(stderr, "CQ event for unknown CQ %p\n", ev_cq);
		return -1;
	}

	if (ibv_req_notify_cq(calc->cq, 0)) {
		fprintf(stderr, "Couldn't request CQ notification\n");
		return -1;
	}

	do {
		count = ec_poll_cq(calc, EC_POLL_BUDGET);
	} while (count > 0);

	return 0;
}

static void ec_sig_handler(int signo)
{
}

void *handle_comp_events(void *data)
{
	struct mlx5_ec_calc *calc = data;
	int n = 0;
	struct sigaction sa = { };

	sigemptyset(&sa.sa_mask);
	sa.sa_handler = ec_sig_handler;
	sigaction(SIGINT, &sa, 0);

	while (!calc->stop_ec_poller) {
		if(unlikely(mlx5_ec_poll_cq(calc)))
			break;
		if (n++ == EC_ACK_NEVENTS) {
			ibv_ack_cq_events(calc->cq, n);
			n = 0;
		}
	}

	ibv_ack_cq_events(calc->cq, n);

	return NULL;
}

struct ibv_qp *alloc_calc_qp(struct mlx5_ec_calc *calc)
{
	struct ibv_qp_init_attr qp_init_attr;
	struct ibv_qp_attr qp_attr;
	struct ibv_qp *ibqp;
	struct mlx5_qp *qp;
	struct ibv_port_attr attr;
	union ibv_gid gid;
	int err;

	memset(&attr, 0, sizeof(attr));
	err = ibv_query_port(calc->pd->context, 1, &attr);
	if (err) {
		perror("failed to query port");
		return NULL;
	};

	err = ibv_query_gid(calc->pd->context, 1, 0, &gid);
	if (err) {
		perror("failed to query gid");
		return NULL;
	};

	memset(&qp_init_attr, 0, sizeof(qp_init_attr));
	qp_init_attr.send_cq = calc->cq;
	qp_init_attr.recv_cq = calc->cq;
	/* FIXME: should really communicate that we do UMRs */
	qp_init_attr.cap.max_send_wr = calc->max_inflight_calcs * MLX5_EC_MAX_WQE_BBS;
	qp_init_attr.cap.max_recv_wr = calc->max_inflight_calcs;
	qp_init_attr.cap.max_send_sge = 1;
	qp_init_attr.cap.max_recv_sge = 1;
	qp_init_attr.qp_type = IBV_QPT_RC;
	ibqp = ibv_create_qp(calc->pd, &qp_init_attr);
	if (!ibqp) {
		fprintf(stderr, "failed to alloc calc qp\n");
		return NULL;
	};

	/* modify to INIT */
	memset(&qp_attr, 0, sizeof(qp_attr));
	qp_attr.qp_state = IBV_QPS_INIT;
	qp_attr.port_num = 1;
	qp_attr.pkey_index = 0;
	qp_attr.qp_access_flags = 0;
	err = ibv_modify_qp(ibqp, &qp_attr, IBV_QP_STATE      |
					    IBV_QP_PORT	      |
					    IBV_QP_PKEY_INDEX |
					    IBV_QP_ACCESS_FLAGS);
	if (err) {
		perror("failed to modify calc qp to INIT");
		goto clean_qp;
	}

	qp = to_mqp(ibqp);
	/* Don't track SQ overflow - we are covered with the RQ flow-ctrl */
	qp->gen_data.create_flags |= IBV_EXP_QP_CREATE_IGNORE_SQ_OVERFLOW;

	/* modify to RTR */
	memset(&qp_attr, 0, sizeof(qp_attr));
	qp_attr.qp_state = IBV_QPS_RTR;
	qp_attr.path_mtu = IBV_MTU_4096; /* FIXME: Is this correct? */
	qp_attr.dest_qp_num = ibqp->qp_num;
	qp_attr.rq_psn = 0;
	qp_attr.max_dest_rd_atomic = 0;
	qp_attr.min_rnr_timer = 12;
	qp_attr.ah_attr.is_global = 1;
	qp_attr.ah_attr.grh.hop_limit = 1;
	qp_attr.ah_attr.grh.dgid = gid;
	qp_attr.ah_attr.grh.sgid_index = 0;
	qp_attr.ah_attr.dlid = attr.lid;
	qp_attr.ah_attr.sl = 0;
	qp_attr.ah_attr.src_path_bits = 0;
	qp_attr.ah_attr.port_num = 1;
	err = ibv_modify_qp(ibqp, &qp_attr, IBV_QP_STATE	      |
					    IBV_QP_AV		      |
					    IBV_QP_PATH_MTU	      |
					    IBV_QP_DEST_QPN	      |
					    IBV_QP_RQ_PSN	      |
					    IBV_QP_MAX_DEST_RD_ATOMIC |
					    IBV_QP_MIN_RNR_TIMER);
	if (err) {
		perror("failed to modify calc qp to RTR");
		goto clean_qp;
	}
	calc->log_chunk_size = 0;

	/* modify to RTS */
	memset(&qp_attr, 0, sizeof(qp_attr));
	qp_attr.qp_state = IBV_QPS_RTS;
	qp_attr.timeout = 14;
	qp_attr.retry_cnt = 7;
	qp_attr.rnr_retry = 7;
	qp_attr.sq_psn = 0;
	qp_attr.max_rd_atomic  = 1;
	err = ibv_modify_qp(ibqp, &qp_attr, IBV_QP_STATE     |
					    IBV_QP_TIMEOUT   |
					    IBV_QP_RETRY_CNT |
					    IBV_QP_RNR_RETRY |
					    IBV_QP_SQ_PSN    |
					    IBV_QP_MAX_QP_RD_ATOMIC);
	if (err) {
		perror("failed to modify calc qp to RTS");
		goto clean_qp;
	}

	return ibqp;

clean_qp:
	ibv_destroy_qp(ibqp);

	return NULL;
}

static void dereg_encode_matrix(struct mlx5_ec_calc *calc)
{
	ibv_dereg_mr(calc->mat_mr);
	free(calc->mat);
}

static int reg_encode_matrix(struct mlx5_ec_calc *calc, uint8_t *matrix)
{
	int k = calc->k;
	int m = calc->m;
	int cols = MLX5_EC_NOUTPUTS(m);
	int i, j, err;

	calc->mat = calloc(1, cols * k);
	if (!calc->mat) {
		fprintf(stderr, "failed to alloc calc matrix\n");
		return ENOMEM;
	}

	for (i = 0; i < k; i++)
		for (j = 0; j < cols; j++) {
			/* 3 outputs, don't set HW format */
			if (j == 3 && m == 3)
				continue;

			/* Crazy HW formatting, bit 5 is on */
			calc->mat[i*cols+j] = matrix[i*m+j] | 0x10;
		}

	calc->mat_mr = ibv_reg_mr(calc->pd, calc->mat,
				  cols * k, IBV_ACCESS_LOCAL_WRITE);
	if (!calc->mat_mr) {
		fprintf(stderr, "failed to alloc calc encode matrix mr\n");
		err = errno;
		goto free_mat;
	}

	return 0;

free_mat:
	free(calc->mat);

	return err;
}

static void free_decodes(struct mlx5_ec_calc *calc)
{
	struct mlx5_ec_decode_pool *pool = &calc->decode_pool;

	free(pool->decodes);
	ibv_dereg_mr(pool->decode_mr);
	free(pool->decode_buf);
}

static int alloc_decodes(struct mlx5_ec_calc *calc)
{
	struct mlx5_ec_decode_pool *pool = &calc->decode_pool;
	int mat_num = calc->max_inflight_calcs;
	int mat_size;
	int cols;
	int i, err;

	cols = MLX5_EC_NOUTPUTS(calc->m);
	mat_size = calc->k * cols;

	INIT_LIST_HEAD(&pool->list);
	mlx5_lock_init(&pool->lock, 1, mlx5_get_locktype());

	pool->decode_buf = calloc(mat_num, mat_size);
	if (!pool->decode_buf) {
		fprintf(stderr, "failed to allocate decode buffer\n");
		return ENOMEM;
	}

	pool->decode_mr = ibv_reg_mr(calc->pd, pool->decode_buf,
				     mat_size * mat_num,
				     IBV_ACCESS_LOCAL_WRITE);
	if (!pool->decode_mr) {
		fprintf(stderr, "failed to alloc calc decode matrix mr\n");
		err = errno;
		goto err_decode_buf;
	}

	pool->decodes = calloc(mat_num, sizeof(*pool->decodes));
	if (!pool->decodes) {
		fprintf(stderr, "failed to allocate decode bufs\n");
		err = ENOMEM;
		goto err_decode_mr;
	}

	for (i = 0; i < mat_num; i++) {
		struct mlx5_ec_decode *decode = &pool->decodes[i];

		decode->sge.lkey = pool->decode_mr->lkey;
		decode->sge.length = mat_size;
		decode->sge.addr = (uintptr_t)(pool->decode_buf + i * mat_size);
		list_add_tail(&decode->node, &pool->list);
	}

	return 0;

err_decode_mr:
	ibv_dereg_mr(pool->decode_mr);
err_decode_buf:
	free(pool->decode_buf);

	return err;
}

static int alloc_dump(struct mlx5_ec_calc *calc)
{
	int chunk_size = MLX5_CHUNK_SIZE(calc);
	int err;

	calc->dump = calloc(1, chunk_size);
	if (!calc->dump)
		return ENOMEM;

	calc->dump_mr = ibv_reg_mr(calc->pd, calc->dump,
	                           chunk_size, IBV_ACCESS_LOCAL_WRITE);
	if (!calc->dump_mr) {
		fprintf(stderr, "failed to alloc calc dump mr\n");
		err = errno;
		goto free_dump;
	}

	return 0;

free_dump:
	free(calc->dump);

	return err;
}

static int
ec_attr_sanity_checks(struct ibv_exp_ec_calc_init_attr *attr)
{
	if (attr->k <= 0 || attr->k > 16) {
		fprintf(stderr, "Bad K arg (%d)\n", attr->k);
		return EINVAL;
	}

	if (attr->m <= 0 || attr->m > 4) {
		fprintf(stderr, "Bad M arg (%d)\n", attr->m);
		return EINVAL;
	}

	if (attr->w != 1 && attr->w != 2 && attr->w != 4) {
		fprintf(stderr, "bad W arg (%d)\n", attr->w);
		return EINVAL;
	}

	if (attr->max_data_sge != attr->k) {
		fprintf(stderr, "Unsupported max_data_sge (%d) != k (%d)\n",
			attr->max_data_sge, attr->k);
		return EINVAL;
	}

	if (attr->max_code_sge != attr->m) {
		fprintf(stderr, "Unsupported max_code_sge (%d) != m (%d)\n",
			attr->max_code_sge, attr->m);
		return EINVAL;
	}
	return 0;
}

struct ibv_exp_ec_calc *
mlx5_alloc_ec_calc(struct ibv_pd *pd,
		   struct ibv_exp_ec_calc_init_attr *attr)
{
	struct mlx5_ec_calc *calc;
	struct ibv_exp_ec_calc *ibcalc;
	struct ibv_exp_create_mr_in in;
	void *status;
	int err;

	err = ec_attr_sanity_checks(attr);
	if (err) {
		errno = err;
		return NULL;
	}

	calc = calloc(1, sizeof(*calc));
	if (!calc) {
		fprintf(stderr, "failed to alloc calc\n");
		return NULL;
	}
	ibcalc = (struct ibv_exp_ec_calc *)&calc->ibcalc;

	calc->pd = ibcalc->pd = pd;
	calc->max_inflight_calcs = attr->max_inflight_calcs;
	calc->k = attr->k;
	calc->m = attr->m;

	calc->channel = ibv_create_comp_channel(calc->pd->context);
	if (!calc->channel) {
		fprintf(stderr, "failed to alloc calc channel\n");
		goto free_calc;
	};

	calc->cq = ibv_create_cq(calc->pd->context,
				 attr->max_inflight_calcs * MLX5_EC_CQ_FACTOR,
				 NULL, calc->channel, attr->affinity_hint);
	if (!calc->cq) {
		fprintf(stderr, "failed to alloc calc cq\n");
		goto free_channel;
	};

	err = ibv_req_notify_cq(calc->cq, 0);
	if (err) {
		fprintf(stderr, "failed to req notify cq\n");
		goto free_cq;
	}

	err = pthread_create(&calc->ec_poller, NULL,
			     handle_comp_events, calc);
	if (err) {
		fprintf(stderr, "failed to create ec_poller\n");
		goto free_cq;
	}

	memset(&in, 0, sizeof(in));
	in.pd = calc->pd;
	in.attr.create_flags = IBV_EXP_MR_INDIRECT_KLMS;
	in.attr.exp_access_flags = IBV_EXP_ACCESS_LOCAL_WRITE;
	in.attr.max_klm_list_size = align(attr->m, 4);
	calc->outumr = mlx5_create_mr(&in);
	if (!calc->outumr) {
		fprintf(stderr, "failed to alloc calc out umr\n");
		goto free_ec_poller;
	};

	in.attr.max_klm_list_size = align(attr->k, 4);
	calc->inumr = mlx5_create_mr(&in);
	if (!calc->inumr) {
		fprintf(stderr, "failed to alloc calc in umr\n");
		goto free_outumr;
	};

	err = reg_encode_matrix(calc, attr->encode_matrix);
	if (err)
		goto free_inumr;

	calc->qp = alloc_calc_qp(calc);
	if (!calc->qp)
		goto encode_matrix;

	err = alloc_decodes(calc);
	if (err)
		goto calc_qp;

	err = alloc_dump(calc);
	if (err)
		goto calc_decodes;

	return ibcalc;

calc_decodes:
	free_decodes(calc);
calc_qp:
	ibv_destroy_qp(calc->qp);
encode_matrix:
	dereg_encode_matrix(calc);
free_inumr:
	mlx5_dereg_mr(calc->inumr);
free_outumr:
	mlx5_dereg_mr(calc->outumr);
free_ec_poller:
	calc->stop_ec_poller = 1;
	wmb();
	pthread_kill(calc->ec_poller, SIGINT);
	pthread_join(calc->ec_poller, &status);
free_cq:
	ibv_destroy_cq(calc->cq);
free_channel:
	ibv_destroy_comp_channel(calc->channel);
free_calc:
	free(calc);

	return NULL;
}

void
mlx5_dealloc_ec_calc(struct ibv_exp_ec_calc *ec_calc)
{
	struct mlx5_ec_calc *calc = to_mcalc(ec_calc);
	void *status;

	free_decodes(calc);
	ibv_destroy_qp(calc->qp);
	dereg_encode_matrix(calc);
	mlx5_dereg_mr(calc->inumr);
	mlx5_dereg_mr(calc->outumr);

	calc->stop_ec_poller = 1;
	wmb();
	pthread_kill(calc->ec_poller, SIGINT);
	pthread_join(calc->ec_poller, &status);

	ibv_destroy_cq(calc->cq);
	ibv_destroy_comp_channel(calc->channel);
	free(calc);
}

static void
set_ec_umr_ctrl_seg(struct mlx5_ec_calc *calc, int nklms,
		    int pat, struct mlx5_wqe_umr_ctrl_seg *umr)
{
	memset(umr, 0, sizeof(*umr));

	umr->flags = MLX5_UMR_CTRL_INLINE;
	umr->klm_octowords = htons(align(nklms + pat, 4));
	umr->mkey_mask =  htonll(MLX5_MKEY_MASK_LEN		|
				 MLX5_MKEY_MASK_START_ADDR	|
				 MLX5_MKEY_MASK_KEY		|
				 MLX5_MKEY_MASK_FREE		|
				 MLX5_MKEY_MASK_LR		|
				 MLX5_MKEY_MASK_LW);
}

static void
set_ec_mkey_seg(struct mlx5_ec_calc *calc,
		struct ibv_sge *klms,
		int nklms,
		uint32_t umr_key,
		int pat,
		struct mlx5_mkey_seg *seg)
{
	memset(seg, 0, sizeof(*seg));

	seg->flags = MLX5_PERM_LOCAL_READ  |
		     MLX5_PERM_LOCAL_WRITE |
		     MLX5_PERM_UMR_EN	   |
		     MLX5_ACCESS_MODE_KLM;
	seg->qpn_mkey7_0 = htonl(0xffffff00 | (umr_key & 0xff));
	seg->flags_pd = htonl(to_mpd(calc->pd)->pdn);
	seg->start_addr = htonll((uintptr_t)klms[0].addr);
	seg->len = htonll(klms[0].length * nklms);
	seg->xlt_oct_size = htonl(align(nklms + pat, 4));
}

static inline void *
rewind_sq(struct mlx5_qp *qp, void **seg, int *size, int *inc)
{
	void *start = mlx5_get_send_wqe(qp, 0);

	*seg = start;
	*size += MLX5_SEND_WQE_BB / 16;
	*inc -= MLX5_SEND_WQE_BB;

	return start;
}

static void
set_ec_umr_pattern_ds(struct mlx5_ec_calc *calc,
		      struct ibv_sge *klms,
		      int nklms, int nrklms,
		      void **seg, int *size)
{
	struct mlx5_qp *qp = to_mqp(calc->qp);
	struct mlx5_seg_repeat_block *rb;
	struct mlx5_seg_repeat_ent *re;
	int set, i, inc_size;
	int chunk_size = min(klms[0].length, MLX5_CHUNK_SIZE(calc));

	inc_size = align(sizeof(*rb) + nrklms * sizeof(*re), MLX5_SEND_WQE_BB);

	rb = *seg;
	rb->const_0x400 = htonl(0x400);
	rb->reserved = 0;
	rb->num_ent = htons(nrklms);
	rb->repeat_count = htonl(DIV_ROUND_UP(klms[0].length * nrklms,
				 chunk_size * nrklms));
	rb->byte_count = htonl(chunk_size * nrklms);
	re = rb->entries;
	for (i = 0; i < nklms; i++, re++) {
		if (re == qp->gen_data.sqend)
			re = rewind_sq(qp, seg, size, &inc_size);

		re->va = htonll(klms[i].addr);
		re->byte_count = htons(chunk_size);
		re->stride = htons(chunk_size);
		re->memkey = htonl(klms[i].lkey);
	}

	/* 3 outputs, set last KLM to our dump lkey */
	if (nklms == 3) {
		if (re == qp->gen_data.sqend)
			re = rewind_sq(qp, seg, size, &inc_size);

		re->va = htonll((uintptr_t)calc->dump);
		re->byte_count = htons(chunk_size);
		re->stride = 0;
		re->memkey = htonl(calc->dump_mr->lkey);
		re++;
	}

	set = align((nrklms + 1), 4) - nrklms - 1;
	if (set)
		memset(re, 0, set * sizeof(*re));

	*seg += inc_size;
	*size += inc_size / 16;
	if (unlikely((*seg == qp->gen_data.sqend)))
		*seg = mlx5_get_send_wqe(qp, 0);
}

static void
set_ec_umr_klm_ds(struct mlx5_ec_calc *calc,
		  struct ibv_sge *klms,
		  int nklms,
		  void **seg, int *size)
{
	struct mlx5_qp *qp = to_mqp(calc->qp);
	struct mlx5_klm *klm;
	int set, i, inc_size;

	inc_size = align(nklms * sizeof(*klm), MLX5_SEND_WQE_BB);

	klm = *seg;
	for (i = 0; i < nklms; i++, klm++) {
		if (klm == qp->gen_data.sqend)
			klm = rewind_sq(qp, seg, size, &inc_size);

		klm->va = htonll(klms[i].addr);
		klm->byte_count = htonl(klms[i].length);
		klm->key = htonl(klms[i].lkey);
	}

	set = align(nklms, 4) - nklms;
	if (set)
		memset(klm, 0, set * sizeof(*klm));

	*seg += inc_size;
	*size += inc_size / 16;
	if (unlikely((*seg == qp->gen_data.sqend)))
		*seg = mlx5_get_send_wqe(qp, 0);
}

static void
post_ec_umr(struct mlx5_ec_calc *calc,
	    struct ibv_sge *klms,
	    int nklms,
	    int block_size,
	    int pattern,
	    uint32_t umr_key,
	    void **seg, int *size)
{
	struct mlx5_qp *qp = to_mqp(calc->qp);
	struct mlx5_wqe_ctrl_seg *ctrl;
	int nrklms = MLX5_EC_NOUTPUTS(nklms);

	ctrl = *seg;
	*seg += sizeof(*ctrl);
	*size = sizeof(*ctrl) / 16;
	if (unlikely((*seg == qp->gen_data.sqend)))
		*seg = mlx5_get_send_wqe(qp, 0);

	set_ec_umr_ctrl_seg(calc, nrklms, pattern, *seg);
	*seg += sizeof(struct mlx5_wqe_umr_ctrl_seg);
	*size += sizeof(struct mlx5_wqe_umr_ctrl_seg) / 16;
	if (unlikely((*seg == qp->gen_data.sqend)))
		*seg = mlx5_get_send_wqe(qp, 0);

	set_ec_mkey_seg(calc, klms, nrklms, umr_key, pattern, *seg);
	*seg += sizeof(struct mlx5_mkey_seg);
	*size += (sizeof(struct mlx5_mkey_seg) / 16);
	if (unlikely((*seg == qp->gen_data.sqend)))
		*seg = mlx5_get_send_wqe(qp, 0);

	if (pattern)
		set_ec_umr_pattern_ds(calc, klms, nklms, nrklms, seg, size);
	else
		set_ec_umr_klm_ds(calc, klms, nklms, seg, size);

	set_ctrl_seg((uint32_t *)ctrl, &qp->ctrl_seg,
		     MLX5_OPCODE_UMR, qp->gen_data.scur_post, 0, *size,
		     0, htonl(umr_key));

	qp->gen_data.fm_cache = MLX5_FENCE_MODE_INITIATOR_SMALL;
}

static void
post_ec_vec_calc(struct mlx5_ec_calc *calc,
		 struct ibv_sge *klm,
		 int block_size,
		 int nvecs,
		 int noutputs,
		 void *matrix_addr,
		 uint32_t matrix_key,
		 int signal,
		 void *seg, int *size)
{
	struct mlx5_qp *qp = to_mqp(calc->qp);
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_vec_calc_seg *vc;
	uint8_t fm_ce_se;
	int i;

	ctrl = seg;
	vc = seg + sizeof(*ctrl);

	memset(vc, 0, sizeof(*vc));
	for (i = 0; i < noutputs; i++)
		vc->calc_op[i] = MLX5_CALC_OP_XOR;

	vc->mat_le_tag_cs = MLX5_CALC_MATRIX | calc->log_chunk_size;
	vc->vec_count = (uint8_t)nvecs;
	vc->cm_lkey = htonl(matrix_key);
	vc->cm_addr = htonll((uintptr_t)matrix_addr);
	vc->vec_size = htonl((block_size >> 2) << 2);
	vc->vec_lkey = htonl(klm->lkey);
	vc->vec_addr = htonll(klm->addr);

	*size = (sizeof(*ctrl) + sizeof(*vc)) / 16;

	fm_ce_se = qp->gen_data.fm_cache;
	if (signal)
		fm_ce_se |= MLX5_WQE_CTRL_CQ_UPDATE;

	set_ctrl_seg((uint32_t *)ctrl, &qp->ctrl_seg,
		     MLX5_OPCODE_SEND, qp->gen_data.scur_post, 0xff, *size,
		     fm_ce_se, 0);

	qp->gen_data.fm_cache = 0;
}

static int ec_post_recv(struct ibv_qp *qp,
			struct ibv_sge *sge,
			struct ibv_exp_ec_comp *comp)
{
	struct ibv_recv_wr wr, *bad_wr;
	int err;

	wr.next = NULL;
	wr.sg_list = sge;
	wr.num_sge = 1;
	wr.wr_id = (uintptr_t)comp;

	err = mlx5_post_recv(qp, &wr, &bad_wr);
	if (err) {
		fprintf(stderr, "failed to post recv calc\n");
		return err;
	}

	return 0;
}

static unsigned begin_wqe(struct mlx5_qp *qp, void **seg)
{
	int idx;

	idx = qp->gen_data.scur_post & (qp->sq.wqe_cnt - 1);
	*seg = mlx5_get_send_wqe(qp, idx);

	return idx;
}

static void finish_wqe(struct mlx5_qp *qp, int idx,
		       int size, void *wrid)
{
	qp->sq.wrid[idx] = (uintptr_t)wrid;
	qp->gen_data.wqe_head[idx] = qp->sq.head + 1;
	qp->gen_data.scur_post += DIV_ROUND_UP(size * 16, MLX5_SEND_WQE_BB);
}

static int mlx5_set_encode_code(struct mlx5_ec_calc *calc,
				struct ibv_exp_ec_mem *ec_mem,
				struct ibv_sge *klms,
				struct ibv_sge *out,
				struct ibv_sge **out_ptr)
{
	int i;

	/* Sanity check the code sges */
	if (unlikely(ec_mem->num_code_sge != calc->m)) {
		fprintf(stderr, "Unsupported num_code_sge %d != %d\n",
			ec_mem->num_code_sge, calc->m);
		return EINVAL;
	}

	/* Single output, just point to it */
	if (calc->m == 1) {
		*out_ptr = ec_mem->code_blocks;
		goto out;
	}

	for (i = 0; i < calc->m; i++) {
		klms[i].addr = ec_mem->code_blocks[i].addr;
		klms[i].lkey = ec_mem->code_blocks[i].lkey;
		klms[i].length = ec_mem->code_blocks[i].length;
		if (klms[i].length != ec_mem->block_size) {
			fprintf(stderr, "Unsupported code_block[%d] length %d\n",
				i, klms[i].length);
			return EINVAL;
		}
	}

	/* Take care of 3 outputs dumping */
	if (calc->m == 3) {
		klms[3].addr = (uintptr_t)calc->dump;
		klms[3].lkey = calc->dump_mr->lkey;
		klms[3].length = ec_mem->block_size;
	}

	out->addr = klms[0].addr;
	out->length = ec_mem->block_size * MLX5_EC_NOUTPUTS(calc->m);
	out->lkey = calc->outumr->lkey;
	*out_ptr = out;
out:
	return 0;
}

static int mlx5_set_encode_data(struct mlx5_ec_calc *calc,
				struct ibv_exp_ec_mem *ec_mem,
				struct ibv_sge *in,
				int *contig)
{
	uint32_t lkey = ec_mem->data_blocks[0].lkey;
	int i;

	/* Sanity check the data sges */
	if (unlikely(ec_mem->num_data_sge != calc->k)) {
		fprintf(stderr, "Unsupported num_data_sge %d != %d\n",
			ec_mem->num_data_sge, calc->k);
		return EINVAL;
	}

	*contig = 1;
	for (i = 0; i < calc->k; i++) {
		if (ec_mem->data_blocks[i].length != ec_mem->block_size) {
			fprintf(stderr, "Unsupported data_block[%d] length %d\n",
				i, ec_mem->data_blocks[i].length);
			return EINVAL;
		}

		if (i && ((ec_mem->data_blocks[i].lkey !=
			  ec_mem->data_blocks[i - 1].lkey) ||
		    (ec_mem->data_blocks[i].addr !=
		     ec_mem->data_blocks[i - 1].addr +
		     ec_mem->data_blocks[i - 1].length))) {
			*contig = 0;
			lkey = calc->inumr->lkey;
		}
	}

	in->addr = ec_mem->data_blocks[0].addr;
	in->length = ec_mem->block_size * calc->k;
	in->lkey = lkey;

	return 0;
}

static int __mlx5_ec_encode_async(struct mlx5_ec_calc *calc,
			 struct ibv_exp_ec_mem *ec_mem,
			 struct ibv_exp_ec_comp *ec_comp)
{
	struct mlx5_qp *qp = to_mqp(calc->qp);
	struct ibv_sge klms[4];
	struct ibv_sge in, out, *out_ptr = NULL;
	void *uninitialized_var(seg);
	unsigned idx;
	int size, err, contig = 0, wqe_count = 0;

	err = mlx5_set_encode_code(calc, ec_mem, klms, &out, &out_ptr);
	if (unlikely(err))
		goto error;

	err = mlx5_set_encode_data(calc, ec_mem, &in, &contig);
	if (unlikely(err))
		goto error;

	/* post recv for calc SEND */
	err = ec_post_recv((struct ibv_qp *)&qp->verbs_qp, out_ptr, ec_comp);
	if (unlikely(err))
		goto error;

	if (calc->m > 1) {
		/* post pattern KLM - non-signaled */
		idx = begin_wqe(qp, &seg);
		post_ec_umr(calc, klms, calc->m, ec_mem->block_size,
			    1, calc->outumr->lkey, &seg, &size);
		finish_wqe(qp, idx, size, NULL);
		wqe_count++;
	}

	if (!contig) {
		/* post UMR of input - non-signaled */
		idx = begin_wqe(qp, &seg);
		post_ec_umr(calc, ec_mem->data_blocks, calc->k,
			    ec_mem->block_size, 0, calc->inumr->lkey,
			    &seg, &size);
		finish_wqe(qp, idx, size, NULL);
		wqe_count++;
	}

	/* post vec_calc SEND - non-signaled */
	idx = begin_wqe(qp, &seg);
	post_ec_vec_calc(calc, &in, ec_mem->block_size,
			 calc->k, calc->m, calc->mat,
			 calc->mat_mr->lkey,
			 0, seg, &size);
	finish_wqe(qp, idx, size, NULL);
	wqe_count++;

	/* ring the DB */
	qp->sq.head += wqe_count;
	__ring_db(qp, qp->gen_data.bf->db_method,
		  qp->gen_data.scur_post & 0xffff,
		  seg, (size + 3) / 4);

	calc->cq_count += 1;

	return 0;

error:
	errno = err;

	return err;
}

int mlx5_ec_encode_async(struct ibv_exp_ec_calc *ec_calc,
			 struct ibv_exp_ec_mem *ec_mem,
			 struct ibv_exp_ec_comp *ec_comp)
{
	struct mlx5_ec_calc *calc = to_mcalc(ec_calc);
	struct mlx5_qp *qp = to_mqp(calc->qp);
	int ret;

	mlx5_lock(&qp->sq.lock);
	ret = __mlx5_ec_encode_async(calc, ec_mem, ec_comp);
	mlx5_unlock(&qp->sq.lock);

	return ret;
}

static int set_decode_klms(struct mlx5_ec_calc *calc,
			   struct ibv_exp_ec_mem *ec_mem,
			   uint32_t erasures,
			   struct ibv_sge *in,
			   struct ibv_sge *iklms,
			   int *in_num,
			   struct ibv_sge *out,
			   struct ibv_sge *oklms,
			   int *out_num)
{
	struct ibv_sge *data = ec_mem->data_blocks;
	struct ibv_sge *code = ec_mem->code_blocks;
	int i, k = 0, m = 0;

	/* XXX: This is just to make the compiler happy */
	iklms[0].addr = 0;

	for (i = 0; i < calc->k + calc->m; i++) {
		if (erasures & (1 << i)) {
			oklms[m].length = ec_mem->block_size;
			if (i < calc->k) {
				if (data[i].length != ec_mem->block_size) {
					fprintf(stderr, "Unsupported data_block[%d] length %d\n",
						i, data[i].length);
					return EINVAL;
				}
				oklms[m].lkey = data[i].lkey;
				oklms[m].addr = data[i].addr;
			} else if (i < calc->k + calc->m) {
				if (code[i - calc->k].length != ec_mem->block_size) {
					fprintf(stderr, "Unsupported code_block[%d] length %d\n",
						i, code[i - calc->k].length);
					return EINVAL;
				}
				oklms[m].lkey = code[i - calc->k].lkey;
				oklms[m].addr = code[i - calc->k].addr;
			} else {
				fprintf(stderr, "bad erasure %d\n", i);
				return EINVAL;
			}
			m++;

			if (unlikely(m > 4)) {
				fprintf(stderr, "more than 4 erasures are not supported\n");
				return EINVAL;
			}
		} else {
			iklms[k].length = ec_mem->block_size;
			if (i < calc->k) {
				if (data[i].length != ec_mem->block_size) {
					fprintf(stderr, "Unsupported data_block[%d] length %d\n",
						i, data[i].length);
					return EINVAL;
				}
				iklms[k].lkey = data[i].lkey;
				iklms[k].addr = data[i].addr;
			} else if (i < calc->k + calc->m) {
				if (code[i - calc->k].length != ec_mem->block_size) {
					fprintf(stderr, "Unsupported code_block[%d] length %d\n",
						i, code[i - calc->k].length);
					return EINVAL;
				}
				iklms[k].lkey = code[i - calc->k].lkey;
				iklms[k].addr = code[i - calc->k].addr;
			} else {
				fprintf(stderr, "bad erasure %d\n", i);
				return EINVAL;
			}
			k++;
		}
	}

	k = calc->k;
	in->lkey = calc->inumr->lkey;
	in->addr = iklms[0].addr;
	in->length = ec_mem->block_size * k;
	*in_num = k;

	if (m > 1)
		out->lkey = calc->outumr->lkey;
	else
		out->lkey = oklms[0].lkey;
	out->addr = oklms[0].addr;
	out->length = ec_mem->block_size * MLX5_EC_NOUTPUTS(m);
	*out_num = m;

	return 0;
}

static int __mlx5_ec_decode_async(struct mlx5_ec_calc *calc,
			 struct ibv_exp_ec_mem *ec_mem,
			 uint32_t erasures,
			 uint8_t *decode_matrix,
			 struct ibv_exp_ec_comp *ec_comp)
{
	struct mlx5_qp *qp = to_mqp(calc->qp);
	struct mlx5_ec_decode *decode;
	struct ibv_sge in_klms[16]; /* XXX: relief the stack? */
	struct ibv_sge out_klms[4];
	struct ibv_sge out, in;
	void *uninitialized_var(seg);
	unsigned idx;
	int err, size, k = 0, m = 0, wqe_count = 0;

	err = set_decode_klms(calc, ec_mem, erasures,
			      &in , in_klms, &k,
			      &out, out_klms, &m);
	if (unlikely(err) || unlikely(m == 0))
		goto error;

	/* post recv for calc SEND */
	err = ec_post_recv(calc->qp, &out, ec_comp);
	if (unlikely(err))
		goto error;

	decode = mlx5_get_ec_decode(calc, decode_matrix, k, m);

	if (m > 1) {
		/* post pattern KLM of output - non-signaled */
		idx = begin_wqe(qp, &seg);
		post_ec_umr(calc, out_klms, m, ec_mem->block_size,
			    1, calc->outumr->lkey, &seg, &size);
		finish_wqe(qp, idx, size, NULL);
		wqe_count++;
	}

	/* post UMR of input - non-signaled */
	idx = begin_wqe(qp, &seg);
	post_ec_umr(calc, in_klms, k, ec_mem->block_size,
		    0, calc->inumr->lkey, &seg, &size);
	finish_wqe(qp, idx, size, NULL);
	wqe_count++;

	/* post vec_calc SEND - signaled */
	idx = begin_wqe(qp, &seg);
	post_ec_vec_calc(calc, &in, ec_mem->block_size, k, m,
			 (void *)(uintptr_t)decode->sge.addr, decode->sge.lkey,
			 1, seg, &size);
	finish_wqe(qp, idx, size, decode);
	wqe_count++;

	/* ring the DB */
	qp->sq.head += wqe_count;
	__ring_db(qp, qp->gen_data.bf->db_method,
		  qp->gen_data.scur_post & 0xffff,
		  seg, (size + 3) / 4);

	calc->cq_count += 2;

	return 0;

error:
	errno = err;

	return err;
}

int mlx5_ec_decode_async(struct ibv_exp_ec_calc *ec_calc,
			 struct ibv_exp_ec_mem *ec_mem,
			 uint32_t erasures,
			 uint8_t *decode_matrix,
			 struct ibv_exp_ec_comp *ec_comp)
{
	struct mlx5_ec_calc *calc = to_mcalc(ec_calc);
	struct mlx5_qp *qp = to_mqp(calc->qp);
	int ret;

	mlx5_lock(&qp->sq.lock);
	ret = __mlx5_ec_decode_async(calc, ec_mem, erasures,
				decode_matrix, ec_comp);
	mlx5_unlock(&qp->sq.lock);

	return ret;
}

static void
mlx5_sync_done(struct ibv_exp_ec_comp *comp)
{
	struct mlx5_ec_comp *def_comp = to_mcomp(comp);

	pthread_mutex_lock(&def_comp->mutex);
	pthread_cond_signal(&def_comp->cond);
	pthread_mutex_unlock(&def_comp->mutex);
}

int mlx5_ec_encode_sync(struct ibv_exp_ec_calc *ec_calc,
			struct ibv_exp_ec_mem *ec_mem)
{
	int err;
	struct mlx5_ec_comp def_comp = {
		.comp = {.done = mlx5_sync_done},
		.mutex = PTHREAD_MUTEX_INITIALIZER,
		.cond = PTHREAD_COND_INITIALIZER,
	};

	pthread_mutex_lock(&def_comp.mutex);
	err = mlx5_ec_encode_async(ec_calc, ec_mem, &def_comp.comp);
	if (err) {
		fprintf(stderr, "%s: failed\n", __func__);
		pthread_mutex_unlock(&def_comp.mutex);
		errno = err;
		return err;
	}

	pthread_cond_wait(&def_comp.cond, &def_comp.mutex);
	pthread_mutex_unlock(&def_comp.mutex);

	return (int)def_comp.comp.status;
}

int mlx5_ec_decode_sync(struct ibv_exp_ec_calc *ec_calc,
			struct ibv_exp_ec_mem *ec_mem,
			uint32_t erasures,
			uint8_t *decode_matrix)
{
	int err;
	struct mlx5_ec_comp def_comp = {
		.comp = {.done = mlx5_sync_done},
		.mutex = PTHREAD_MUTEX_INITIALIZER,
		.cond = PTHREAD_COND_INITIALIZER,
	};

	pthread_mutex_lock(&def_comp.mutex);
	err = mlx5_ec_decode_async(ec_calc, ec_mem, erasures,
				   decode_matrix, &def_comp.comp);
	if (err) {
		fprintf(stderr, "%s: failed\n", __func__);
		pthread_mutex_unlock(&def_comp.mutex);
		errno = err;
		return err;
	}

	pthread_cond_wait(&def_comp.cond, &def_comp.mutex);
	pthread_mutex_unlock(&def_comp.mutex);

	return (int)def_comp.comp.status;
}

int mlx5_ec_poll(struct ibv_exp_ec_calc *ec_calc, int n)
{
	struct mlx5_ec_calc *calc = to_mcalc(ec_calc);

	return ec_poll_cq(calc, n);
}

int mlx5_ec_encode_send(struct ibv_exp_ec_calc *ec_calc,
			struct ibv_exp_ec_mem *ec_mem,
			struct ibv_exp_ec_stripe *data_stripes,
			struct ibv_exp_ec_stripe *code_stripes)
{
	struct mlx5_ec_calc *calc = to_mcalc(ec_calc);
	struct mlx5_qp *qp = to_mqp(calc->qp);
	struct ibv_exp_send_wr wait_wr;
	struct ibv_exp_send_wr *bad_exp_wr;
	struct ibv_send_wr *bad_wr;
	int i, err;

	/* stripe data */
	for (i = 0; i < calc->k; i++) {
		err = ibv_post_send(data_stripes[i].qp,
				    data_stripes[i].wr, &bad_wr);
		if (unlikely(err)) {
			fprintf(stderr, "ibv_post_send(%d) failed\n", i);
			return err;
		}
	}

	mlx5_lock(&qp->sq.lock);
	/* post async encode */
	err = __mlx5_ec_encode_async(calc, ec_mem, NULL);
	if (unlikely(err)) {
		fprintf(stderr, "mlx5_ec_encode_async failed\n");
		goto out;
	}

	/* stripe code */
	wait_wr.exp_opcode = IBV_EXP_WR_CQE_WAIT;
	wait_wr.exp_send_flags = IBV_EXP_SEND_WAIT_EN_LAST;
	wait_wr.num_sge = 0;
	wait_wr.sg_list = NULL;
	wait_wr.task.cqe_wait.cq = calc->cq;
	wait_wr.task.cqe_wait.cq_count = calc->cq_count;
	calc->cq_count = 0;
	wait_wr.next = NULL;
	for (i = 0; i < calc->m; i++) {
		wait_wr.wr_id = code_stripes[i].wr->wr_id;

		/*
		 * XXX: I can't post a wr chain because mlx5_exp_post_send
		 * assumes ibv_exp_send_wr which is different than ibv_send_wr.
		 * Bleh...
		 */
		err = ibv_exp_post_send(code_stripes[i].qp,
					&wait_wr, &bad_exp_wr);
		if (unlikely(err)) {
			fprintf(stderr, "ibv_exp_post_send(%d) failed err=%d\n",
				i, err);
			goto out;
		}
		wait_wr.task.cqe_wait.cq_count = 0;

		err = ibv_post_send(code_stripes[i].qp,
				    code_stripes[i].wr, &bad_wr);
		if (unlikely(err)) {
			fprintf(stderr, "ibv_post_send(%d) failed err=%d\n",
				i, err);
			goto out;
		}
	}

out:
	mlx5_unlock(&qp->sq.lock);

	return err;
}
