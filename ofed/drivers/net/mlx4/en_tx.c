/*
 * Copyright (c) 2007, 2014 Mellanox Technologies. All rights reserved.
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
 *
 */

#include <linux/page.h>
#include <linux/mlx4/cq.h>
#include <linux/slab.h>
#include <linux/mlx4/qp.h>
#include <linux/if_vlan.h>
#include <linux/vmalloc.h>
#include <linux/moduleparam.h>

#ifdef CONFIG_RATELIMIT
#include <linux/delay.h>
#include <linux/bitops.h>
#endif

#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/tcp.h>
#include <netinet/tcp_lro.h>
#include <netinet/udp.h>

#include "mlx4_en.h"
#include "utils.h"

enum {
	MAX_INLINE = 104, /* 128 - 16 - 4 - 4 */
	MAX_BF = 256,
	MIN_PKT_LEN = 17,
};

static int inline_thold __read_mostly = MAX_INLINE;

module_param_named(inline_thold, inline_thold, uint, 0444);
MODULE_PARM_DESC(inline_thold, "threshold for using inline data");

int mlx4_en_create_tx_ring(struct mlx4_en_priv *priv,
			   struct mlx4_en_tx_ring **pring, u32 size,
			   u16 stride, int node, int queue_idx)
{
	struct mlx4_en_dev *mdev = priv->mdev;
	struct mlx4_en_tx_ring *ring;
	int tmp;
	int err;

	ring = kzalloc_node(sizeof(struct mlx4_en_tx_ring), GFP_KERNEL, node);
	if (!ring) {
		ring = kzalloc(sizeof(struct mlx4_en_tx_ring), GFP_KERNEL);
		if (!ring) {
			en_err(priv, "Failed allocating TX ring\n");
			return -ENOMEM;
		}
	}

	ring->size = size;
	ring->size_mask = size - 1;
	ring->stride = stride;
#ifdef CONFIG_RATELIMIT
	ring->rl_data.rate_index = 0;
	/* User_valid should be false in a rate_limit ring until the
	 * creation process of the ring is done, after the activation. */
	if (queue_idx < priv->native_tx_ring_num)
		ring->rl_data.user_valid = true;
	else
		ring->rl_data.user_valid = false;
#endif
	ring->full_size = ring->size - HEADROOM - MAX_DESC_TXBBS;
	ring->inline_thold = min(inline_thold, MAX_INLINE);
	mtx_init(&ring->tx_lock.m, "mlx4 tx", NULL, MTX_DEF);
	mtx_init(&ring->comp_lock.m, "mlx4 comp", NULL, MTX_DEF);

	/* Allocate the buf ring */
#ifdef CONFIG_RATELIMIT
	if (queue_idx < priv->native_tx_ring_num)
		ring->br = buf_ring_alloc(MLX4_EN_DEF_TX_QUEUE_SIZE, M_DEVBUF,
					  M_WAITOK, &ring->tx_lock.m);
	else
		ring->br = buf_ring_alloc(size / 4, M_DEVBUF, M_WAITOK,
					  &ring->tx_lock.m);
#else
	ring->br = buf_ring_alloc(MLX4_EN_DEF_TX_QUEUE_SIZE, M_DEVBUF,
				  M_WAITOK, &ring->tx_lock.m);
#endif
	if (ring->br == NULL) {
		en_err(priv, "Failed allocating tx_info ring\n");
		return -ENOMEM;
	}

	tmp = size * sizeof(struct mlx4_en_tx_info);
	ring->tx_info = vmalloc_node(tmp, node);
	if (!ring->tx_info) {
		ring->tx_info = vmalloc(tmp);
		if (!ring->tx_info) {
			err = -ENOMEM;
			goto err_ring;
		}
	}

	en_dbg(DRV, priv, "Allocated tx_info ring at addr:%p size:%d\n",
		 ring->tx_info, tmp);

	ring->bounce_buf = kmalloc_node(MAX_DESC_SIZE, GFP_KERNEL, node);
	if (!ring->bounce_buf) {
		ring->bounce_buf = kmalloc(MAX_DESC_SIZE, GFP_KERNEL);
		if (!ring->bounce_buf) {
			err = -ENOMEM;
			goto err_info;
		}
	}
	ring->buf_size = ALIGN(size * ring->stride, MLX4_EN_PAGE_SIZE);

	/* Allocate HW buffers on provided NUMA node */
	err = mlx4_alloc_hwq_res(mdev->dev, &ring->wqres, ring->buf_size,
				 2 * PAGE_SIZE);
	if (err) {
		en_err(priv, "Failed allocating hwq resources\n");
		goto err_bounce;
	}

	err = mlx4_en_map_buffer(&ring->wqres.buf);
	if (err) {
		en_err(priv, "Failed to map TX buffer\n");
		goto err_hwq_res;
	}

	ring->buf = ring->wqres.buf.direct.buf;

	en_dbg(DRV, priv, "Allocated TX ring (addr:%p) - buf:%p size:%d "
	       "buf_size:%d dma:%llx\n", ring, ring->buf, ring->size,
	       ring->buf_size, (unsigned long long) ring->wqres.buf.direct.map);

	err = mlx4_qp_reserve_range(mdev->dev, 1, 1, &ring->qpn,
				    MLX4_RESERVE_BF_QP);
	if (err) {
		en_err(priv, "failed reserving qp for TX ring\n");
		goto err_map;
	}

	err = mlx4_qp_alloc(mdev->dev, ring->qpn, &ring->qp);
	if (err) {
		en_err(priv, "Failed allocating qp %d\n", ring->qpn);
		goto err_reserve;
	}
	ring->qp.event = mlx4_en_sqp_event;

	err = mlx4_bf_alloc(mdev->dev, &ring->bf, node);
	if (err) {
		en_dbg(DRV, priv, "working without blueflame (%d)", err);
		ring->bf.uar = &mdev->priv_uar;
		ring->bf.uar->map = mdev->uar_map;
		ring->bf_enabled = false;
	} else
		ring->bf_enabled = true;
	ring->queue_index = queue_idx;
	if (queue_idx < priv->num_tx_rings_p_up )
		CPU_SET(queue_idx, &ring->affinity_mask);

	*pring = ring;
	return 0;

err_reserve:
	mlx4_qp_release_range(mdev->dev, ring->qpn, 1);
err_map:
	mlx4_en_unmap_buffer(&ring->wqres.buf);
err_hwq_res:
	mlx4_free_hwq_res(mdev->dev, &ring->wqres, ring->buf_size);
err_bounce:
	kfree(ring->bounce_buf);
err_info:
	vfree(ring->tx_info);
err_ring:
	buf_ring_free(ring->br, M_DEVBUF);
	kfree(ring);
	return err;
}

void mlx4_en_destroy_tx_ring(struct mlx4_en_priv *priv,
			     struct mlx4_en_tx_ring **pring)
{
	struct mlx4_en_dev *mdev = priv->mdev;
	struct mlx4_en_tx_ring *ring = *pring;
	en_dbg(DRV, priv, "Destroying tx ring, qpn: %d\n", ring->qpn);

	buf_ring_free(ring->br, M_DEVBUF);
	if (ring->bf_enabled)
		mlx4_bf_free(mdev->dev, &ring->bf);
	mlx4_qp_remove(mdev->dev, &ring->qp);
	mlx4_qp_free(mdev->dev, &ring->qp);
	mlx4_qp_release_range(priv->mdev->dev, ring->qpn, 1);
	mlx4_en_unmap_buffer(&ring->wqres.buf);
	mlx4_free_hwq_res(mdev->dev, &ring->wqres, ring->buf_size);
	kfree(ring->bounce_buf);
	vfree(ring->tx_info);
	mtx_destroy(&ring->tx_lock.m);
	mtx_destroy(&ring->comp_lock.m);
	kfree(ring);
	*pring = NULL;
}

int mlx4_en_activate_tx_ring(struct mlx4_en_priv *priv,
			     struct mlx4_en_tx_ring *ring,
			     int cq, int user_prio)
{
	struct mlx4_en_dev	*mdev = priv->mdev;
	int			err;

	ring->cqn = cq;
	ring->prod = 0;
	ring->cons = 0xffffffff;
	ring->last_nr_txbb = 1;
	ring->poll_cnt = 0;
	ring->blocked = 0;
	memset(ring->tx_info, 0, ring->size * sizeof(struct mlx4_en_tx_info));
	memset(ring->buf, 0, ring->buf_size);

	ring->qp_state = MLX4_QP_STATE_RST;
	ring->doorbell_qpn = ring->qp.qpn << 8;

#ifdef CONFIG_RATELIMIT
	if (ring->rl_data.rate_index) {
                /* Force rate limit user priority */
                user_prio = MLX4_EN_DEF_RL_USER_PRIO;
        }
#endif

	mlx4_en_fill_qp_context(priv, ring->size, ring->stride, 1, 0, ring->qpn,
				ring->cqn, user_prio, &ring->context);

#ifdef CONFIG_RATELIMIT
	if (ring->rl_data.rate_index) {
                ring->context.rate_limit_index = ring->rl_data.rate_index;
        }
#endif

	if (ring->bf_enabled)
		ring->context.usr_page = cpu_to_be32(ring->bf.uar->index);

	err = mlx4_qp_to_ready(mdev->dev, &ring->wqres.mtt, &ring->context,
			       &ring->qp, &ring->qp_state);
	return err;
}

#ifdef CONFIG_RATELIMIT
static int mlx4_en_find_available_tx_ring_index(struct mlx4_en_priv *priv)
{
	int					index = -1;
	struct mlx4_en_reuse_index_list_element	*reused_item;

	spin_lock(&priv->tx_ring_index_lock);
	/* Check for availble index in re-use list */
	if ((reused_item = STAILQ_FIRST(&priv->reuse_index_list_head))) {
		index = reused_item->val;
		/* Remove head index from re-use list */
		STAILQ_REMOVE_HEAD(&priv->reuse_index_list_head, entry);
	}
	else if (priv->tx_ring_num < MAX_TX_RINGS) {
		index = priv->tx_ring_num;
		priv->tx_ring_num++;
	} else /* Reached max resources capacity */
		index = -1;
	spin_unlock(&priv->tx_ring_index_lock);

	return index;
}

/* Check whether the requested rate is valid.
 * If so, retrieve the relevant rate index. */
static int mlx4_en_validate_rate_ctl_req(struct mlx4_en_priv *priv,
					 struct ifreq_hwtxring *rl_req, u8 *rate_index)
{
	int i;
	u32 rate;

	/* Kernel passes rate in bytes and the driver converts it to bits in order
	 * to communicate with the hardware. */
	rl_req->txringid_max_rate = rl_req->txringid_max_rate * BITS_PER_BYTE;
	rate = rl_req->txringid_max_rate;

	if (rate > priv->mdev->dev->caps.rl_caps.calc_max_val ||
	    (rate < priv->mdev->dev->caps.rl_caps.calc_min_val &&
	     rate != 0)) {
		en_err(priv, "Not valid rate limit : %u Bps %d\n",rate / BITS_PER_BYTE, priv->port);
		return (EINVAL);
	}

	/* Searching for the requested rate in the rate table */
	for (i = 0; i <= priv->num_rates_per_prio; i++) {
		if (priv->rate_limits[i].rate == rate) {
			*rate_index = i;
			return (0);
		}
	}

	en_err(priv, "Not existing rate limit %u Bps %d\n",rate / BITS_PER_BYTE, priv->port);
	return (EINVAL);
}

void mlx4_en_invalidate_rl_ring(struct mlx4_en_priv *priv, uint32_t ring_id)
{
	priv->tx_ring[ring_id]->rl_data.user_valid = false;
	sysctl_ctx_free(&priv->tx_ring[ring_id]->rl_data.rl_stats_ctx);
}

void mlx4_en_rl_reused_index_insert(struct mlx4_en_priv *priv, uint32_t ring_id)
{
	struct mlx4_en_reuse_index_list_element *reused_item;

        reused_item = priv->reuse_index_list_array + ring_id;
        spin_lock(&priv->tx_ring_index_lock);
        STAILQ_INSERT_TAIL(&priv->reuse_index_list_head, reused_item, entry);
        spin_unlock(&priv->tx_ring_index_lock);
}

static void mlx4_en_rate_limit_sysctl_stat(struct mlx4_en_priv *priv, int ring_id)
{
	struct mlx4_en_tx_ring *tx_ring;
	struct sysctl_ctx_list *ctx;
	struct sysctl_oid_list *head_node;
	struct sysctl_oid *ring_node;
	struct sysctl_oid_list *ring_list;
	char namebuf[128];

	tx_ring = priv->tx_ring[ring_id];
	ctx = &tx_ring->rl_data.rl_stats_ctx;
	snprintf(namebuf, sizeof(namebuf), "tx_ring%d", ring_id);
	head_node = SYSCTL_CHILDREN(priv->sysctl_stat);
	ring_node = SYSCTL_ADD_NODE(ctx, head_node, OID_AUTO, namebuf,
			CTLFLAG_RD, NULL, "TX Ring");
	ring_list = SYSCTL_CHILDREN(ring_node);
	SYSCTL_ADD_UINT(ctx, ring_list, OID_AUTO, "rate_limit_val",
			CTLFLAG_RD, &priv->rate_limits[tx_ring->rl_data.rate_index].rate, 0, "Rate Limit value");
	SYSCTL_ADD_ULONG(ctx, ring_list, OID_AUTO, "packets",
			CTLFLAG_RD, &tx_ring->packets, "TX packets");
	SYSCTL_ADD_ULONG(ctx, ring_list, OID_AUTO, "bytes",
			CTLFLAG_RD, &tx_ring->bytes, "TX bytes");
}

static int mlx4_en_defer_rl_op(struct mlx4_en_priv *priv,
				int ring_id, u8 rate_index,
				enum mlx4_en_rl_operation opp)
{
	struct mlx4_en_rl_task_list_element     *rl_item;

	rl_item = kmalloc(sizeof(struct mlx4_en_rl_task_list_element), M_NOWAIT);
	if (!rl_item) {
		en_err(priv, "Failed allocating rl_item\n");
		return (ENOMEM);
	}

	/* Saving recieved data from kernel in order to use it later in
	 * the defer function */
	rl_item->ring_id = ring_id;
	rl_item->rate_index = rate_index;
	rl_item->operation = opp;

	spin_lock(&priv->rl_op_lock);
	STAILQ_INSERT_TAIL(&priv->rl_op_list_head, rl_item, entry);
	spin_unlock(&priv->rl_op_lock);
	taskqueue_enqueue(priv->rl_tq, &priv->rl_task);
	return (0);
}

int mlx4_en_create_rate_limit_ring(struct mlx4_en_priv *priv,
				   struct ifreq_hwtxring *rl_req)
{
	int	err = 0;
	int	index = 0;
	u8	rate_index;

	/* Check for HW/FW support */
	if (!priv->mdev->dev->caps.rl_caps.enable) {
		en_err(priv, "No HW/FW support for rate limit rings\n");
		return (ENODEV);
	}

	/* Validate rate limit request */
	if(mlx4_en_validate_rate_ctl_req(priv, rl_req, &rate_index))
		return (EINVAL);

	/* Find available ring index */
	index = mlx4_en_find_available_tx_ring_index(priv);

	if (index < 0) {
		en_err(priv, "Failed to create Rate limit resources, "
					"Max capacity reached\n");
		return (EINVAL);
	}

	atomic_add_int(&priv->rate_limits[rate_index].ref, 1);
	rl_req->txringid = index;

	/* Defer ring creation */
	err = mlx4_en_defer_rl_op(priv, rl_req->txringid, rate_index, MLX4_EN_RL_ADD);

	return err;
}

int mlx4_en_modify_rate_limit_ring(struct mlx4_en_priv *priv,
				   struct ifreq_hwtxring *rl_req)
{
	u8	rate_index;
	int	err = 0;

	/* Validate rate limit request */
	if(mlx4_en_validate_rate_ctl_req(priv, rl_req, &rate_index))
		return (err);

	atomic_add_int(&priv->rate_limits[rate_index].ref, 1);

	/* Validation for ring index occurs at the deffered function
	 * in order to prevent failure when creation was not completed
	 * yet (defered actions are executed by one thread) */

	/* Defer ring modification */
	err = mlx4_en_defer_rl_op(priv, rl_req->txringid, rate_index, MLX4_EN_RL_MOD);

	return (err);
}

int mlx4_en_destroy_rate_limit_ring(struct mlx4_en_priv *priv,
				   struct ifreq_hwtxring *rl_req)
{
	uint32_t ring_id;
	int err = 0;

	ring_id = rl_req->txringid;

	/* Check that this is indeed a rate limit ring */
	if (ring_id < priv->native_tx_ring_num || ring_id >= priv->tx_ring_num) {
                en_err(priv, "Deleting ring %d: Permision denied: Not a rate limit ring\n", ring_id);
                return (EINVAL);
        }

	/* Defer ring destruction */
	/* There is no handling with new rate index when destroying a ring
	 * therefor, sending zero as a rate index. */
	err = mlx4_en_defer_rl_op(priv, rl_req->txringid, 0, MLX4_EN_RL_DEL);

	return err;
}

static void mlx4_en_create_rl_res(struct mlx4_en_priv *priv,
				  int ring_id, u8 rate_index)
{
	struct mlx4_en_cq 	*cq;
	struct mlx4_en_tx_ring 	*tx_ring;
	struct mlx4_en_dev	*mdev = priv->mdev;
	int 			err = 0;
	int			node = 0;
	int			j;


	if (priv->tx_ring[ring_id]) {
                /* Ring already exists, needs activation */
                /* Make sure drbr queue has no left overs from before */
                tx_ring = priv->tx_ring[ring_id];
                goto activate;
        }

	err = mlx4_en_create_cq(priv, &priv->tx_cq[ring_id],
                MLX4_EN_DEF_RL_TX_RING_SIZE, ring_id, TX, node);
        if (err) {
                en_err(priv, "Failed to create rate limit tx CQ, ring index %u, rate %u\n", ring_id, rate_index);
                goto err_create_cq;
        }

        err = mlx4_en_create_tx_ring(priv, &priv->tx_ring[ring_id],
                MLX4_EN_DEF_RL_TX_RING_SIZE, TXBB_SIZE, node, ring_id);
        if (err) {
                en_err(priv, "Failed to create rate limited tx ring %u, rate %u\n", ring_id, rate_index);
                goto err_create_ring;
        }

	tx_ring = priv->tx_ring[ring_id];

activate:

	sysctl_ctx_init(&tx_ring->rl_data.rl_stats_ctx);
	tx_ring->rl_data.rate_index = rate_index;

        /* Default moderation */
        cq = priv->tx_cq[ring_id];
        cq->moder_cnt = priv->tx_frames;
        cq->moder_time = priv->tx_usecs;

        mutex_lock(&mdev->state_lock);
        if (!priv->port_up) {
                /* No need activating resources, start_port will take care of that */
		tx_ring->rl_data.user_valid = true;
                mutex_unlock(&mdev->state_lock);
                return;
        }

        /* Activate resources */
        err = mlx4_en_activate_cq(priv, cq, ring_id);
        if (err) {
                en_err(priv, "Failed activating Rate Limit Tx CQ\n");
                goto err_activate_resources;
        }

	err = mlx4_en_set_cq_moder(priv, cq);
        if (err) {
                en_err(priv, "Failed setting cq moderation parameters");
                mlx4_en_deactivate_cq(priv, cq);
                goto err_activate_resources;
        }
        en_dbg(DRV, priv, "Resetting index of CQ:%d to -1\n", ring_id);
        cq->buf->wqe_index = cpu_to_be16(0xffff);

        err = mlx4_en_activate_tx_ring(priv, tx_ring, cq->mcq.cqn,
                                               MLX4_EN_DEF_RL_USER_PRIO);
        if (err) {
                en_err(priv, "Failed activating rate limit TX ring\n");
                mlx4_en_deactivate_cq(priv, cq);
                goto err_activate_resources;
        }

        /* Arm CQ for TX completions */
        mlx4_en_arm_cq(priv, cq);

        /* Set initial ownership of all Tx TXBBs to SW (1) */
	for (j = 0; j < tx_ring->buf_size; j += STAMP_STRIDE)
		*((u32 *) (tx_ring->buf + j)) = INIT_OWNER_BIT;

	/* Set ring as valid */
        tx_ring->rl_data.user_valid = true;
	mutex_unlock(&mdev->state_lock);

	priv->rate_limit_tx_ring_num++;

	/* Add rate limit statistics to sysctl if debug option was enabled */
	if (show_rl_sysctl_info)
		mlx4_en_rate_limit_sysctl_stat(priv, ring_id);
	return;

err_activate_resources:
	mlx4_en_invalidate_rl_ring(priv, ring_id);
        mlx4_en_rl_reused_index_insert(priv, ring_id);
	atomic_subtract_int(&priv->rate_limits[rate_index].ref, 1);
        mutex_unlock(&mdev->state_lock);
        return;

err_create_ring:
        if (priv->tx_cq[ring_id])
                mlx4_en_destroy_cq(priv, &priv->tx_cq[ring_id]);

err_create_cq:
	mlx4_en_rl_reused_index_insert(priv, ring_id);
	atomic_subtract_int(&priv->rate_limits[rate_index].ref, 1);
}

static void mlx4_en_modify_rl_res(struct mlx4_en_priv *priv,
			   int ring_id, u8 rate_index)
{
	struct mlx4_en_tx_ring *tx_ring;
	struct mlx4_update_qp_params update_params;
	int err;

	tx_ring = priv->tx_ring[ring_id];

	/* Ring validation */
	if(!TX_RING_USER_VALID(ring_id)) {
		en_err(priv, "Failed modifying new rate, ring %d doesn't exist\n", ring_id);
		/* If the modified ring does not exist, no need to add one
		 * to the reference count of the requested rate */
		atomic_subtract_int(&priv->rate_limits[rate_index].ref, 1);
		return;
	}

	if (priv->rate_limits[tx_ring->rl_data.rate_index].rate !=
				priv->rate_limits[rate_index].rate) {
		update_params.rl_index = rate_index;
		err = mlx4_update_qp(priv->mdev->dev, tx_ring->qpn, MLX4_UPDATE_QP_RATE_LIMIT,
				     &update_params);
		if (err) {
			en_err(priv, "Failed updating ring %d with new rate %uBytes/sec, err: %d\n",
			       ring_id, (priv->rate_limits[rate_index].rate/8), err);
			atomic_subtract_int(&priv->rate_limits[rate_index].ref, 1);
			return;
		}
	}
	atomic_subtract_int(&priv->rate_limits[tx_ring->rl_data.rate_index].ref, 1);
	tx_ring->rl_data.rate_index = rate_index;
}

static void mlx4_en_destroy_rl_res(struct mlx4_en_priv *priv,
                                    int ring_id)
{
	struct mlx4_en_tx_ring *ring;
	struct mlx4_en_dev *mdev = priv->mdev;

	ring = priv->tx_ring[ring_id];

	mutex_lock(&mdev->state_lock);

	/* Index was validated, thus ring is not NULL */
	spin_lock(&ring->tx_lock);
	if (ring->rl_data.user_valid == false) {
		en_err(priv, "ring %d doesn't exist\n", ring_id);
		spin_unlock(&ring->tx_lock);
		return;
	} else {
		ring->rl_data.user_valid = false;
	}
	if (!drbr_empty(priv->dev, ring->br)) {
		struct mbuf *m;
		while ((m = buf_ring_dequeue_sc(ring->br)) != NULL) {
			m_freem(m);
		}
	}
	spin_unlock(&ring->tx_lock);
	atomic_subtract_int(&priv->rate_limits[ring->rl_data.rate_index].ref, 1);

	/* Deactivate resources */
	if (priv->port_up) {
		mlx4_en_deactivate_tx_ring(priv, ring);
		mlx4_en_deactivate_cq(priv, priv->tx_cq[ring_id]);
		msleep(10);
		mlx4_en_free_tx_buf(priv->dev, ring);
	}
	mutex_unlock(&mdev->state_lock);

	/* clear statistics */
	ring->bytes = 0;
	ring->packets = 0;

	sysctl_ctx_free(&ring->rl_data.rl_stats_ctx);

	/* Add index to re-use list */
	priv->rate_limit_tx_ring_num--;
	mlx4_en_rl_reused_index_insert(priv, ring_id);
}

/* Called from the rl_task context, it acquires the first
 * task from the rl_op_list and calls the relevant functions according to
 * the needed operation. */
void mlx4_en_async_rl_operation(void *context, int pending)
{
        struct mlx4_en_priv			*priv;
	struct mlx4_en_rl_task_list_element	*rl_item;
	enum mlx4_en_rl_operation		rl_operation;
	int					ring_id;
	u8					rate_index;

        priv = context;

	while(pending){
	        /* Check for availble operation in the operation list */
		spin_lock(&priv->rl_op_lock);
	        if ((rl_item = STAILQ_FIRST(&priv->rl_op_list_head))) {
			ring_id = rl_item->ring_id;
			rl_operation = rl_item->operation;
			rate_index = rl_item->rate_index;
	                STAILQ_REMOVE_HEAD(&priv->rl_op_list_head, entry);
			spin_unlock(&priv->rl_op_lock);
			kfree(rl_item);
		}
		else {
			spin_unlock(&priv->rl_op_lock);
			pr_err("No avaliable rate limit item \n");
			return;
		}

		switch (rl_operation){
			case MLX4_EN_RL_ADD:
				mlx4_en_create_rl_res(priv, ring_id, rate_index);
				break;
			case MLX4_EN_RL_DEL:
				mlx4_en_destroy_rl_res(priv, ring_id);
				break;
			case MLX4_EN_RL_MOD:
				mlx4_en_modify_rl_res(priv, ring_id, rate_index);
				break;
			default:
				pr_err("Not supported operation - %d \n", rl_operation);
		}
		pending--;
	}
}
#endif

void mlx4_en_deactivate_tx_ring(struct mlx4_en_priv *priv,
				struct mlx4_en_tx_ring *ring)
{
	struct mlx4_en_dev *mdev = priv->mdev;

	mlx4_qp_modify(mdev->dev, NULL, ring->qp_state,
		       MLX4_QP_STATE_RST, NULL, 0, 0, &ring->qp);
}

#ifdef CONFIG_WQE_FORMAT_1
#define COPY_LSO_HEADER_EN(dst, src, hdr_sz)				\
	copy_lso_header(dst, src, hdr_sz, owner_bit)
static inline void copy_lso_header(__be32 *dst, caddr_t src, int hdr_sz,
				   __be32 owner_bit) {
	/* In WQE_FORMAT = 1 we need to split segments larger
	 * than 64 bytes, in this case: 64 - sizeof(ctrl) -
	 * sizeof(lso->mss_hdr_size) = 44
	 */
	if (likely(hdr_sz > 44)) {
		memcpy(dst, src, 44);

		/* Writing the rest of the header and leaving 4 byte
		 * for the inline header
		 */
		memcpy((dst + 12), src + 44, hdr_sz - 44);

		/* Make sure we write the rest of the segment before
		 * setting ownership bit to HW
		 */
		wmb();

		*(dst + 11) =
			cpu_to_be32((1 << 31) |
				    (hdr_sz - 44)) |
			owner_bit;
	} else {
		memcpy(dst, src, hdr_sz);
	}
}

/* The +8 is for mss_header and inline header */
#define GET_LSO_SEG_SIZE_EN(lso_header_size)			\
	(((lso_header_size) > 44) ?				\
	 ALIGN((lso_header_size) + 8, DS_SIZE_ALIGNMENT) :	\
	 ALIGN((lso_header_size) + 4, DS_SIZE_ALIGNMENT))

#else
#define COPY_LSO_HEADER_EN(dst, src, hdr_sz)	memcpy(dst, src, hdr_sz)
static void mlx4_en_stamp_wqe(struct mlx4_en_priv *priv,
		       struct mlx4_en_tx_ring *ring,
		       int index, u8 owner)
{
	struct mlx4_en_tx_info *tx_info = &ring->tx_info[index];
	struct mlx4_en_tx_desc *tx_desc = ring->buf + index * TXBB_SIZE;
	void *end = ring->buf + ring->buf_size;
	__be32 *ptr = (__be32 *)tx_desc;
	__be32 stamp = cpu_to_be32(STAMP_VAL | (!!owner << STAMP_SHIFT));
	int i;

	/* Optimize the common case when there are no wraparounds */
	if (likely((void *)tx_desc + tx_info->nr_txbb * TXBB_SIZE <= end))
		/* Stamp the freed descriptor */
		for (i = 0; i < tx_info->nr_txbb * TXBB_SIZE; i += STAMP_STRIDE) {
			*ptr = stamp;
			ptr += STAMP_DWORDS;
		}
	else
		/* Stamp the freed descriptor */
		for (i = 0; i < tx_info->nr_txbb * TXBB_SIZE; i += STAMP_STRIDE) {
			*ptr = stamp;
			ptr += STAMP_DWORDS;
			if ((void *)ptr >= end) {
				ptr = ring->buf;
				stamp ^= cpu_to_be32(0x80000000);
			}
		}
}

#define GET_LSO_SEG_SIZE_EN(lso_header_size)			\
	ALIGN(lso_header_size + 4, DS_SIZE_ALIGNMENT)
#endif

static u32 mlx4_en_free_tx_desc(struct mlx4_en_priv *priv,
				struct mlx4_en_tx_ring *ring,
				int index, u8 owner, u64 timestamp)
{
	struct mlx4_en_dev *mdev = priv->mdev;
	struct mlx4_en_tx_info *tx_info = &ring->tx_info[index];
	struct mlx4_en_tx_desc *tx_desc = ring->buf + index * TXBB_SIZE;
	struct mlx4_wqe_data_seg *data = (void *) tx_desc + tx_info->data_offset;
        struct mbuf *mb = tx_info->mb;
	void *end = ring->buf + ring->buf_size;
	int frags = tx_info->nr_segs;;
	int i;

	/* Optimize the common case when there are no wraparounds */
	if (likely((void *) tx_desc + tx_info->nr_txbb * TXBB_SIZE <= end)) {
		if (!tx_info->inl) {
			for (i = 0; i < frags; i++) {
                                pci_unmap_single(mdev->pdev,
                                                (dma_addr_t) be64_to_cpu(data[i].addr),
                                                data[i].byte_count, PCI_DMA_TODEVICE);
			}
		}
	} else {
		if (!tx_info->inl) {
			if ((void *) data >= end) {
				data = ring->buf + ((void *)data - end);
			}
			for (i = 0; i < frags; i++) {
				/* Check for wraparound before unmapping */
				if ((void *) data >= end)
					data = ring->buf;
                                pci_unmap_single(mdev->pdev,
                                                (dma_addr_t) be64_to_cpu(data->addr),
                                                data->byte_count, PCI_DMA_TODEVICE);
				++data;
			}
		}
	}
	/* Send a copy of the frame to the BPF listener */
        if (priv->dev && priv->dev->if_bpf)
                ETHER_BPF_MTAP(priv->dev, mb);
        m_freem(mb);
	return tx_info->nr_txbb;
}

int mlx4_en_free_tx_buf(struct net_device *dev, struct mlx4_en_tx_ring *ring)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	int cnt = 0;

	/* Skip last polled descriptor */
	ring->cons += ring->last_nr_txbb;
	en_dbg(DRV, priv, "Freeing Tx buf - cons:0x%x prod:0x%x\n",
		 ring->cons, ring->prod);

	if ((u32) (ring->prod - ring->cons) > ring->size) {
                en_warn(priv, "Tx consumer passed producer!\n");
		return 0;
	}

	while (ring->cons != ring->prod) {
		ring->last_nr_txbb = mlx4_en_free_tx_desc(priv, ring,
						ring->cons & ring->size_mask,
						!!(ring->cons & ring->size), 0);
		ring->cons += ring->last_nr_txbb;
		cnt++;
	}

	if (cnt)
		en_dbg(DRV, priv, "Freed %d uncompleted tx descriptors\n", cnt);

	return cnt;
}

static int mlx4_en_process_tx_cq(struct net_device *dev,
				 struct mlx4_en_cq *cq)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	struct mlx4_cq *mcq = &cq->mcq;
	struct mlx4_en_tx_ring *ring = priv->tx_ring[cq->ring];
	struct mlx4_cqe *cqe;
	u16 index;
	u16 new_index, ring_index, stamp_index;
	u32 txbbs_skipped = 0;
#ifndef CONFIG_WQE_FORMAT_1
	u32 txbbs_stamp = 0;
#endif
	u32 cons_index = mcq->cons_index;
	int size = cq->size;
	u32 size_mask = ring->size_mask;
	struct mlx4_cqe *buf = cq->buf;
	u32 packets = 0;
	u32 bytes = 0;
	int factor = priv->cqe_factor;
	u64 timestamp = 0;
	int done = 0;


	if (!priv->port_up)
		return 0;

	index = cons_index & size_mask;
	cqe = &buf[(index << factor) + factor];
	ring_index = ring->cons & size_mask;
	stamp_index = ring_index;

	/* Process all completed CQEs */
	while (XNOR(cqe->owner_sr_opcode & MLX4_CQE_OWNER_MASK,
			cons_index & size)) {
		/*
		 * make sure we read the CQE after we read the
		 * ownership bit
		 */
		rmb();

		if (unlikely((cqe->owner_sr_opcode & MLX4_CQE_OPCODE_MASK) ==
			     MLX4_CQE_OPCODE_ERROR)) {
			en_err(priv, "CQE completed in error - vendor syndrom: 0x%x syndrom: 0x%x\n",
			       ((struct mlx4_err_cqe *)cqe)->
				       vendor_err_syndrome,
			       ((struct mlx4_err_cqe *)cqe)->syndrome);
		}

		/* Skip over last polled CQE */
		new_index = be16_to_cpu(cqe->wqe_index) & size_mask;

		do {
			txbbs_skipped += ring->last_nr_txbb;
			ring_index = (ring_index + ring->last_nr_txbb) & size_mask;
			/* free next descriptor */
			ring->last_nr_txbb = mlx4_en_free_tx_desc(
					priv, ring, ring_index,
					!!((ring->cons + txbbs_skipped) &
					ring->size), timestamp);
#ifndef CONFIG_WQE_FORMAT_1
			mlx4_en_stamp_wqe(priv, ring, stamp_index,
					  !!((ring->cons + txbbs_stamp) &
						ring->size));
			stamp_index = ring_index;
			txbbs_stamp = txbbs_skipped;
#endif
			packets++;
			bytes += ring->tx_info[ring_index].nr_bytes;
		} while (ring_index != new_index);

		++cons_index;
		index = cons_index & size_mask;
		cqe = &buf[(index << factor) + factor];
	}


	/*
	 * To prevent CQ overflow we first update CQ consumer and only then
	 * the ring consumer.
	 */
	mcq->cons_index = cons_index;
	mlx4_cq_set_ci(mcq);
	wmb();
	ring->cons += txbbs_skipped;

	/* Wakeup Tx queue if it was stopped and ring is not full */
	if (unlikely(ring->blocked) &&
	    (ring->prod - ring->cons) <= ring->full_size) {
		ring->blocked = 0;
#ifdef CONFIG_RATELIMIT
		if (cq->ring < priv->native_tx_ring_num) {
			if (atomic_fetchadd_int(&priv->blocked, -1) == 1)
				atomic_clear_int(&dev->if_drv_flags ,IFF_DRV_OACTIVE);
			priv->port_stats.wake_queue++;
		}
#else
		if (atomic_fetchadd_int(&priv->blocked, -1) == 1)
			atomic_clear_int(&dev->if_drv_flags ,IFF_DRV_OACTIVE);
		priv->port_stats.wake_queue++;
#endif
		ring->wake_queue++;
	}
	return done;
}

void mlx4_en_tx_irq(struct mlx4_cq *mcq)
{
	struct mlx4_en_cq *cq = container_of(mcq, struct mlx4_en_cq, mcq);
	struct mlx4_en_priv *priv = netdev_priv(cq->dev);
	struct mlx4_en_tx_ring *ring = priv->tx_ring[cq->ring];

	if (!spin_trylock(&ring->comp_lock))
		return;
	mlx4_en_process_tx_cq(cq->dev, cq);
	mod_timer(&cq->timer, jiffies + 1);
	spin_unlock(&ring->comp_lock);
}

void mlx4_en_poll_tx_cq(unsigned long data)
{
	struct mlx4_en_cq *cq = (struct mlx4_en_cq *) data;
	struct mlx4_en_priv *priv = netdev_priv(cq->dev);
	struct mlx4_en_tx_ring *ring = priv->tx_ring[cq->ring];
	u32 inflight;

	INC_PERF_COUNTER(priv->pstats.tx_poll);

	if (!spin_trylock(&ring->comp_lock)) {
		mod_timer(&cq->timer, jiffies + MLX4_EN_TX_POLL_TIMEOUT);
		return;
	}
	mlx4_en_process_tx_cq(cq->dev, cq);
	inflight = (u32) (ring->prod - ring->cons - ring->last_nr_txbb);

	/* If there are still packets in flight and the timer has not already
	 * been scheduled by the Tx routine then schedule it here to guarantee
	 * completion processing of these packets */
	if (inflight && priv->port_up)
		mod_timer(&cq->timer, jiffies + MLX4_EN_TX_POLL_TIMEOUT);

	spin_unlock(&ring->comp_lock);
}

static struct mlx4_en_tx_desc *mlx4_en_bounce_to_desc(struct mlx4_en_priv *priv,
						      struct mlx4_en_tx_ring *ring,
						      u32 index,
						      unsigned int desc_size)
{
	u32 copy = (ring->size - index) * TXBB_SIZE;
	int i;
#ifdef CONFIG_WQE_FORMAT_1
	__be32 owner_bit = (ring->prod & ring->size) ?
		cpu_to_be32(MLX4_EN_BIT_DESC_OWN) : 0;
#endif
	for (i = desc_size - copy - 4; i >= 0; i -= 4) {
		if ((i & (TXBB_SIZE - 1)) == 0) {
			wmb();
#ifdef CONFIG_WQE_FORMAT_1
			*((u32 *) (ring->buf + i)) =
				(*((u32 *) (ring->bounce_buf + copy + i)) &
				 WQE_FORMAT_1_MASK) | owner_bit;
			continue;
#endif
		}

		*((u32 *) (ring->buf + i)) =
			*((u32 *) (ring->bounce_buf + copy + i));
	}

	for (i = copy - 4; i >= 4 ; i -= 4) {
		if ((i & (TXBB_SIZE - 1)) == 0)
			wmb();

		*((u32 *) (ring->buf + index * TXBB_SIZE + i)) =
			*((u32 *) (ring->bounce_buf + i));
	}

	/* Return real descriptor location */
	return ring->buf + index * TXBB_SIZE;
}

static inline void mlx4_en_xmit_poll(struct mlx4_en_priv *priv, int tx_ind)
{
	struct mlx4_en_cq *cq = priv->tx_cq[tx_ind];
	struct mlx4_en_tx_ring *ring = priv->tx_ring[tx_ind];

	/* If we don't have a pending timer, set one up to catch our recent
	   post in case the interface becomes idle */
	if (!timer_pending(&cq->timer))
		mod_timer(&cq->timer, jiffies + MLX4_EN_TX_POLL_TIMEOUT);

	/* Poll the CQ every mlx4_en_TX_MODER_POLL packets */
	if ((++ring->poll_cnt & (MLX4_EN_TX_POLL_MODER - 1)) == 0)
		if (spin_trylock(&ring->comp_lock)) {
			mlx4_en_process_tx_cq(priv->dev, cq);
			spin_unlock(&ring->comp_lock);
		}
}

static int is_inline(struct mbuf *mb, int thold)
{
	if (thold && mb->m_pkthdr.len <= thold &&
		(mb->m_pkthdr.csum_flags & CSUM_TSO) == 0)
		return 1;

        return 0;
}

static int inline_size(struct mbuf *mb)
{
	int len;

	len = mb->m_pkthdr.len;
	if (len + CTRL_SIZE + sizeof(struct mlx4_wqe_inline_seg)
	    <= MLX4_INLINE_ALIGN)
		return ALIGN(len + CTRL_SIZE +
			     sizeof(struct mlx4_wqe_inline_seg), 16);
	else
		return ALIGN(len + CTRL_SIZE + 2 *
			     sizeof(struct mlx4_wqe_inline_seg), 16);
}

static int get_head_size(struct mbuf *mb)
{
	struct ether_vlan_header *eh;
        struct tcphdr *th;
        struct ip *ip;
        int ip_hlen, tcp_hlen;
	struct ip6_hdr *ip6;
	uint16_t eth_type;
	int eth_hdr_len;

	eh = mtod(mb, struct ether_vlan_header *);
	if (mb->m_len < ETHER_HDR_LEN)
		return (0);
	if (eh->evl_encap_proto == htons(ETHERTYPE_VLAN)) {
		eth_type = ntohs(eh->evl_proto);
		eth_hdr_len = ETHER_HDR_LEN + ETHER_VLAN_ENCAP_LEN;
	} else {
		eth_type = ntohs(eh->evl_encap_proto);
		eth_hdr_len = ETHER_HDR_LEN;
	}
	if (mb->m_len < eth_hdr_len)
		return (0);
	switch (eth_type) {
	case ETHERTYPE_IP:
		ip = (struct ip *)(mb->m_data + eth_hdr_len);
		if (mb->m_len < eth_hdr_len + sizeof(*ip))
			return (0);
		if (ip->ip_p != IPPROTO_TCP)
			return (0);
		ip_hlen = ip->ip_hl << 2;
		eth_hdr_len += ip_hlen;
		break;
	case ETHERTYPE_IPV6:
		ip6 = (struct ip6_hdr *)(mb->m_data + eth_hdr_len);
		if (mb->m_len < eth_hdr_len + sizeof(*ip6))
			return (0);
		if (ip6->ip6_nxt != IPPROTO_TCP)
			return (0);
		eth_hdr_len += sizeof(*ip6);
		break;
	default:
		return (0);
	}
	if (mb->m_len < eth_hdr_len + sizeof(*th))
		return (0);
	th = (struct tcphdr *)(mb->m_data + eth_hdr_len);
	tcp_hlen = th->th_off << 2;
	eth_hdr_len += tcp_hlen;
	if (mb->m_len < eth_hdr_len)
		return (0);
	return (eth_hdr_len);
}

static int get_real_size(struct mbuf *mb, struct net_device *dev, int *p_n_segs,
    int *lso_header_size, int inl)
{
        struct mbuf *m;
        int nr_segs = 0;

        for (m = mb; m != NULL; m = m->m_next)
                if (m->m_len)
                        nr_segs++;

        if (mb->m_pkthdr.csum_flags & CSUM_TSO) {
                *lso_header_size = get_head_size(mb);
                if (*lso_header_size) {
                        if (mb->m_len == *lso_header_size)
                                nr_segs--;
                        *p_n_segs = nr_segs;
                        return CTRL_SIZE + nr_segs * DS_SIZE +
				GET_LSO_SEG_SIZE_EN(*lso_header_size);
                }
        } else
                *lso_header_size = 0;
        *p_n_segs = nr_segs;
        if (inl)
                return inline_size(mb);
        return (CTRL_SIZE + nr_segs * DS_SIZE);
}

static struct mbuf *mb_copy(struct mbuf *mb, int *offp, char *data, int len)
{
        int bytes;
        int off;

        off = *offp;
        while (len) {
                bytes = min(mb->m_len - off, len);
                if (bytes)
                        memcpy(data, mb->m_data + off, bytes);
                len -= bytes;
                data += bytes;
                off += bytes;
                if (off == mb->m_len) {
                        off = 0;
                        mb = mb->m_next;
                }
        }
        *offp = off;
        return (mb);
}

static void build_inline_wqe(struct mlx4_en_tx_desc *tx_desc, struct mbuf *mb,
			     int real_size,
			     u16 *vlan_tag,
			     int tx_ind,
			     __be32 owner_bit)
{
	struct mlx4_wqe_inline_seg *inl = &tx_desc->inl;
	int spc = MLX4_INLINE_ALIGN - CTRL_SIZE - sizeof *inl;
	int len;
	int off;

	off = 0;
	len = mb->m_pkthdr.len;
	if (len <= spc) {
		inl->byte_count = SET_BYTE_COUNT(1 << 31 |
				(max_t(typeof(len), len, MIN_PKT_LEN)));
		mb_copy(mb, &off, (void *)(inl + 1), len);
		if (len < MIN_PKT_LEN)
                        memset(((void *)(inl + 1)) + len, 0,
                               MIN_PKT_LEN - len);
	} else {
		inl->byte_count = SET_BYTE_COUNT(1 << 31 | spc);
		mb = mb_copy(mb, &off, (void *)(inl + 1), spc);
		inl = (void *) (inl + 1) + spc;
		mb_copy(mb, &off, (void *)(inl + 1), len - spc);
		wmb();
		inl->byte_count = SET_BYTE_COUNT(1 << 31 | (len - spc));
	}
	tx_desc->ctrl.vlan_tag = cpu_to_be16(*vlan_tag);
	tx_desc->ctrl.ins_vlan = MLX4_WQE_CTRL_INS_VLAN * !!(*vlan_tag);
	tx_desc->ctrl.fence_size = (real_size / 16) & 0x3f;
}

static unsigned long hashrandom;
static void hashrandom_init(void *arg)
{
	hashrandom = random();
}
SYSINIT(hashrandom_init, SI_SUB_KLD, SI_ORDER_SECOND, &hashrandom_init, NULL);

u16 mlx4_en_select_queue(struct net_device *dev, struct mbuf *mb)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	u32 rings_p_up = priv->num_tx_rings_p_up;
	u32 vlan_tag = 0;
	u32 up = 0;
	u32 queue_index;

	/* Obtain VLAN information if present */
	if (mb->m_flags & M_VLANTAG) {
		vlan_tag = mb->m_pkthdr.ether_vtag;
	        up = (vlan_tag >> 13);
	}

	/* hash mbuf */
	queue_index = mlx4_en_hashmbuf(MLX4_F_HASHL3 | MLX4_F_HASHL4, mb, hashrandom);

	return ((queue_index % rings_p_up) + (up * rings_p_up));
}

static void mlx4_bf_copy(void __iomem *dst, unsigned long *src, unsigned bytecnt)
{
	__iowrite64_copy(dst, src, bytecnt / 8);
}

static u64 mlx4_en_mac_to_u64(u8 *addr)
{
        u64 mac = 0;
        int i;

        for (i = 0; i < ETHER_ADDR_LEN; i++) {
                mac <<= 8;
                mac |= addr[i];
        }
        return mac;
}

static int mlx4_en_xmit(struct net_device *dev, int tx_ind, struct mbuf **mbp)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	struct mlx4_en_dev *mdev = priv->mdev;
	struct mlx4_en_tx_ring *ring;
	struct mlx4_en_cq *cq;
	struct mlx4_en_tx_desc *tx_desc;
	struct mlx4_wqe_data_seg *data;
	struct mlx4_en_tx_info *tx_info;
	struct mbuf *m;
	int nr_txbb;
	int nr_segs;
	int desc_size;
	int real_size;
	dma_addr_t dma;
	u32 index, bf_index, ring_size;
	__be32 op_own;
	u16 vlan_tag = 0;
	int i;
	int lso_header_size;
	bool bounce = false;
	bool inl = false;
	struct mbuf *mb;
	mb = *mbp;
	int defrag = 1;
	__be32 owner_bit;
#ifdef CONFIG_WQE_FORMAT_1
	__be32  wqe_data_len[32];
#endif

	if (!priv->port_up)
		goto tx_drop;

	ring = priv->tx_ring[tx_ind];
	ring_size = ring->size;

	owner_bit = (ring->prod & ring->size) ?
		cpu_to_be32(MLX4_EN_BIT_DESC_OWN) : 0;

	inl = is_inline(mb, ring->inline_thold);

retry:
	real_size = get_real_size(mb, dev, &nr_segs, &lso_header_size, inl);
	if (unlikely(!real_size))
		goto tx_drop;

	/* Align descriptor to TXBB size */
	desc_size = ALIGN(real_size, TXBB_SIZE);
	nr_txbb = desc_size / TXBB_SIZE;
	if (unlikely(nr_txbb > MAX_DESC_TXBBS)) {
		if (defrag) {
                        mb = m_defrag(*mbp, M_NOWAIT);
                        if (mb == NULL) {
                                mb = *mbp;
                                goto tx_drop;
                        }
                        *mbp = mb;
                        defrag = 0;
                        goto retry;
                }
		/*Oversized header or SG list*/
		ring->oversized_packets++;
		goto tx_drop;
	}

	/* Obtain VLAN information if present */
	if (mb->m_flags & M_VLANTAG) {
		vlan_tag = mb->m_pkthdr.ether_vtag;
	}

	/* Check available TXBBs and 2K spare for prefetch
	 * Even if netif_tx_stop_queue() will be called
	 * driver will send current packet to ensure
	 * that at least one completion will be issued after
	 * stopping the queue
	 */
	if (unlikely((int)(ring->prod - ring->cons) > ring->full_size)) {
#ifdef CONFIG_RATELIMIT
		if (tx_ind < priv->native_tx_ring_num) {
			/* every full native Tx ring stops queue */
			if (ring->blocked == 0)
				atomic_add_int(&priv->blocked, 1);
			/* Set HW-queue-is-full flag */
			atomic_set_int(&dev->if_drv_flags, IFF_DRV_OACTIVE);
			priv->port_stats.queue_stopped++;
		}
#else
		/* every full Tx ring stops queue */
		if (ring->blocked == 0)
			atomic_add_int(&priv->blocked, 1);
		/* Set HW-queue-is-full flag */
		atomic_set_int(&dev->if_drv_flags, IFF_DRV_OACTIVE);
		priv->port_stats.queue_stopped++;
#endif
		ring->blocked = 1;
		ring->queue_stopped++;

		/* Use interrupts to find out when queue opened */
		cq = priv->tx_cq[tx_ind];
		mlx4_en_arm_cq(priv, cq);
		return (ENOBUFS);
        }

	/* Track current inflight packets for performance analysis */
	AVG_PERF_COUNTER(priv->pstats.inflight_avg,
			 (u32) (ring->prod - ring->cons - 1));

	/* Packet is good - grab an index and transmit it */
	index = ring->prod & ring->size_mask;
	bf_index = ring->prod;

	/* See if we have enough space for whole descriptor TXBB for setting
	 * SW ownership on next descriptor; if not, use a bounce buffer. */
	if (likely(index + nr_txbb <= ring_size))
		tx_desc = ring->buf + index * TXBB_SIZE;
	else {
		tx_desc = (struct mlx4_en_tx_desc *) ring->bounce_buf;
		bounce = true;
	}

	/* Save mb in tx_info ring */
	tx_info = &ring->tx_info[index];
	tx_info->mb = mb;
	tx_info->nr_txbb = nr_txbb;
	tx_info->nr_segs = nr_segs;

	if (lso_header_size) {
		data = ((void *)&tx_desc->lso +
			GET_LSO_SEG_SIZE_EN(lso_header_size));
		/* lso header is part of m_data.
		 * need to omit when mapping DMA */
		mb->m_data += lso_header_size;
		mb->m_len -= lso_header_size;
	}
	else
		data = &tx_desc->data;

	/* valid only for none inline segments */
	tx_info->data_offset = (void *)data - (void *)tx_desc;

	if (inl) {
		tx_info->inl = 1;
	} else {
		for (i = 0, m = mb; i < nr_segs; i++, m = m->m_next) {
			if (m->m_len == 0) {
				i--;
				continue;
			}
			dma = pci_map_single(mdev->dev->pdev, m->m_data,
					     m->m_len, PCI_DMA_TODEVICE);
			data->addr = cpu_to_be64(dma);
			data->lkey = cpu_to_be32(mdev->mr.key);
#ifdef CONFIG_WQE_FORMAT_1
			/* Keep the len + owner bit in array to be used later.
			 * We need to make sure the owner bit is the last thing
			 * written to make sure the HW won't read the wrong
			 * data. we move down the chain (mbuf) without setting.
			 * Need to make sure we set the bit every 64B when going
			 * up
			 */
			wqe_data_len[i] = SET_BYTE_COUNT(m->m_len);
#else
			/* Need to make sure we write the data first and
			 * then overide the wqe stamp
			 */
			wmb();
			data->byte_count = SET_BYTE_COUNT(m->m_len);
#endif
			data++;
		}

#ifdef CONFIG_WQE_FORMAT_1
		/* Prepare to go up the chain.
		 * and set len and owner_bit
		 */
		data--;
		i--;
		while (i > -1) {
			/*Need to make sure we write owner_bit after
			 *data has been synced
			 */
			wmb();

			data->byte_count = wqe_data_len[i];
			data--;
			i--;
		}
#endif
                if (lso_header_size) {
                        mb->m_data -= lso_header_size;
                        mb->m_len += lso_header_size;
                }
                tx_info->inl = 0;
	}


	/* Prepare ctrl segement apart opcode+ownership, which depends on
	 * whether LSO is used */
	tx_desc->ctrl.vlan_tag = cpu_to_be16(vlan_tag);
	tx_desc->ctrl.ins_vlan = MLX4_WQE_CTRL_INS_VLAN *
		!!vlan_tag;
	tx_desc->ctrl.fence_size = (real_size / 16) & 0x3f;
	tx_desc->ctrl.srcrb_flags = priv->ctrl_flags;
	if (mb->m_pkthdr.csum_flags & (CSUM_IP | CSUM_TSO |
		CSUM_TCP | CSUM_UDP | CSUM_TCP_IPV6 | CSUM_UDP_IPV6)) {
		if (mb->m_pkthdr.csum_flags & (CSUM_IP | CSUM_TSO))
			tx_desc->ctrl.srcrb_flags |= cpu_to_be32(MLX4_WQE_CTRL_IP_CSUM);
		if (mb->m_pkthdr.csum_flags & (CSUM_TCP | CSUM_UDP |
		    CSUM_UDP_IPV6 | CSUM_TCP_IPV6 | CSUM_TSO))
			tx_desc->ctrl.srcrb_flags |= cpu_to_be32(MLX4_WQE_CTRL_TCP_UDP_CSUM);
		priv->port_stats.tx_chksum_offload++;
                ring->tx_csum++;
        }

	if (unlikely(priv->validate_loopback)) {
		/* Copy dst mac address to wqe */
                struct ether_header *ethh;
                u64 mac;
                u32 mac_l, mac_h;

                ethh = mtod(mb, struct ether_header *);
                mac = mlx4_en_mac_to_u64(ethh->ether_dhost);
                if (mac) {
                        mac_h = (u32) ((mac & 0xffff00000000ULL) >> 16);
                        mac_l = (u32) (mac & 0xffffffff);
                        tx_desc->ctrl.srcrb_flags |= cpu_to_be32(mac_h);
                        tx_desc->ctrl.imm = cpu_to_be32(mac_l);
                }
	}

	/* Handle LSO (TSO) packets */
	if (lso_header_size) {
		int segsz;
		/* Mark opcode as LSO */
		op_own = cpu_to_be32(MLX4_OPCODE_LSO | MLX4_WQE_CTRL_RR);

		/* Fill in the LSO prefix */
		tx_desc->lso.mss_hdr_size = cpu_to_be32(
			mb->m_pkthdr.tso_segsz << 16 | lso_header_size);

		COPY_LSO_HEADER_EN(tx_desc->lso.header, mb->m_data,
				   lso_header_size);

                priv->port_stats.tso_packets++;
                segsz = mb->m_pkthdr.tso_segsz;
                i = ((mb->m_pkthdr.len - lso_header_size + segsz - 1) / segsz);
                tx_info->nr_bytes= mb->m_pkthdr.len + (i - 1) * lso_header_size;
                ring->packets += i;
	} else {
		/* Normal (Non LSO) packet */
		op_own = cpu_to_be32(MLX4_OPCODE_SEND);
		tx_info->nr_bytes = max(mb->m_pkthdr.len,
                    (unsigned int)ETHER_MIN_LEN - ETHER_CRC_LEN);
		ring->packets++;

	}
	ring->bytes += tx_info->nr_bytes;
	AVG_PERF_COUNTER(priv->pstats.tx_pktsz_avg, mb->m_pkthdr.len);

	if (tx_info->inl) {
		build_inline_wqe(tx_desc, mb, real_size, &vlan_tag, tx_ind,
				 owner_bit);
		tx_info->inl = 1;
	}
	op_own |= owner_bit;
	ring->prod += nr_txbb;


	/* If we used a bounce buffer then copy descriptor back into place */
	if (unlikely(bounce))
		tx_desc = mlx4_en_bounce_to_desc(priv, ring, index, desc_size);
	if (ring->bf_enabled && desc_size <= MAX_BF && !bounce && !vlan_tag) {
		*(__be32 *) (&tx_desc->ctrl.vlan_tag) |= cpu_to_be32(ring->doorbell_qpn);
		op_own |= htonl((bf_index & 0xffff) << 8);
		/* Ensure new descirptor hits memory
		* before setting ownership of this descriptor to HW */
		wmb();
		tx_desc->ctrl.owner_opcode = op_own;

		wmb();

		mlx4_bf_copy(ring->bf.reg + ring->bf.offset, (unsigned long *) &tx_desc->ctrl,
		     desc_size);

		wmb();

		ring->bf.offset ^= ring->bf.buf_size;
	} else {
		/* Ensure new descirptor hits memory
		* before setting ownership of this descriptor to HW */
		wmb();
		tx_desc->ctrl.owner_opcode = op_own;
		wmb();
		writel(cpu_to_be32(ring->doorbell_qpn), ring->bf.uar->map + MLX4_SEND_DOORBELL);
	}

	return 0;
tx_drop:
	*mbp = NULL;
	m_freem(mb);
	return EINVAL;
}

static int
mlx4_en_transmit_locked(struct ifnet *dev, int tx_ind, struct mbuf *m)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	struct mlx4_en_tx_ring *ring;
	struct mbuf *next;
	int enqueued, err = 0;

	ring = priv->tx_ring[tx_ind];

#ifdef CONFIG_RATELIMIT
	if ((dev->if_drv_flags & (IFF_DRV_RUNNING | IFF_DRV_OACTIVE)) !=
	    IFF_DRV_RUNNING || priv->port_up == 0 || ring->blocked == 1) {
#else
	if ((dev->if_drv_flags & (IFF_DRV_RUNNING | IFF_DRV_OACTIVE)) !=
	    IFF_DRV_RUNNING || priv->port_up == 0) {
#endif
		if (m != NULL)
			err = drbr_enqueue(dev, ring->br, m);
		return (err);
	}

	enqueued = 0;
	if (m != NULL)
		/* If we can't insert mbuf into drbr, try to xmit anyway.
		 * We keep the error we got so we could return that after xmit.
		 */
		err = drbr_enqueue(dev, ring->br, m);

	/* Process the queue */
	while ((next = drbr_peek(dev, ring->br)) != NULL) {
		if (mlx4_en_xmit(dev, tx_ind, &next) != 0) {
			if (next == NULL) {
				drbr_advance(dev, ring->br);
			} else {
				drbr_putback(dev, ring->br, next);
			}
			break;
		}
		drbr_advance(dev, ring->br);
		enqueued++;
		if ((dev->if_drv_flags & IFF_DRV_RUNNING) == 0)
			break;
	}

	if (enqueued > 0)
		ring->watchdog_time = ticks;

	return (err);
}

void
mlx4_en_tx_que(void *context, int pending)
{
	struct mlx4_en_tx_ring *ring;
	struct mlx4_en_priv *priv;
	struct net_device *dev;
	struct mlx4_en_cq *cq;
	int tx_ind;
	cq = context;
	dev = cq->dev;
	priv = dev->if_softc;
	tx_ind = cq->ring;
	ring = priv->tx_ring[tx_ind];

        if (dev->if_drv_flags & IFF_DRV_RUNNING) {
		mlx4_en_xmit_poll(priv, tx_ind);
		spin_lock(&ring->tx_lock);
                if (!drbr_empty(dev, ring->br))
#ifdef CONFIG_RATELIMIT
			if (ring->rl_data.user_valid)
#endif
				mlx4_en_transmit_locked(dev, tx_ind, NULL);
		spin_unlock(&ring->tx_lock);
	}
}

int
mlx4_en_transmit(struct ifnet *dev, struct mbuf *m)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	struct mlx4_en_tx_ring *ring;
	struct mlx4_en_cq *cq;
	int i = 0, err = 0;

#ifdef CONFIG_RATELIMIT
	/*Check mbuf if this is a rate limit packet*/
	if (M_HASHTYPE_TEST(m, M_HASHTYPE_HWTXRING)) {
		/*make sure the ring is allocated*/
		if ( priv->tx_ring[m->m_pkthdr.flowid] )
			i = m->m_pkthdr.flowid;
		else
			i = mlx4_en_select_queue(dev, m);
	}
	else
#endif
	if ((m->m_flags & (M_FLOWID | M_VLANTAG)) == M_FLOWID) {
		i = m->m_pkthdr.flowid % (priv->tx_ring_num - 1);
	}
	else {
		i = mlx4_en_select_queue(dev, m);
	}

#ifdef CONFIG_RATELIMIT
lock_and_transmit:
#endif
	ring = priv->tx_ring[i];
	if (spin_trylock(&ring->tx_lock)) {
#ifdef CONFIG_RATELIMIT
		if (ring->rl_data.user_valid == false) {
			/* Rate limit ring is not active */
			spin_unlock(&ring->tx_lock);
			i = mlx4_en_select_queue(dev, m);
			goto lock_and_transmit;

		}
#endif
		err = mlx4_en_transmit_locked(dev, i, m);
		spin_unlock(&ring->tx_lock);
		/* Poll CQ here */
		mlx4_en_xmit_poll(priv, i);
	} else {
#ifdef CONFIG_RATELIMIT
		/* This is the only place where we check user_valid without tx_lock
		 * It is ok because the design is that destroy and transmit will not happen in parallel on the same ring (tcp_output code).
		 */
		if (ring->rl_data.user_valid == false) {
			/* Rate limit ring is not active */
			i = mlx4_en_select_queue(dev, m);
			goto lock_and_transmit;
		}
#endif
		err = drbr_enqueue(dev, ring->br, m);
		cq = priv->tx_cq[i];
		taskqueue_enqueue(cq->tq, &cq->cq_task);
	}

	return (err);
}

/*
 * Flush ring buffers.
 */
void
mlx4_en_qflush(struct ifnet *dev)
{
	struct mlx4_en_priv *priv = netdev_priv(dev);
	struct mlx4_en_tx_ring *ring;
	struct mbuf *m;

	for (int i = 0; i < priv->tx_ring_num; i++) {
		ring = priv->tx_ring[i];
#ifdef CONFIG_RATELIMIT
		if (!ring)
			continue;
#endif
		spin_lock(&ring->tx_lock);
		while ((m = buf_ring_dequeue_sc(ring->br)) != NULL)
			m_freem(m);
		spin_unlock(&ring->tx_lock);
	}
	if_qflush(dev);
}
