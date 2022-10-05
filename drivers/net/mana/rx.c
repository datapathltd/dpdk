/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2022 Microsoft Corporation
 */
#include <ethdev_driver.h>

#include <infiniband/verbs.h>
#include <infiniband/manadv.h>

#include "mana.h"

static uint8_t mana_rss_hash_key_default[TOEPLITZ_HASH_KEY_SIZE_IN_BYTES] = {
	0x2c, 0xc6, 0x81, 0xd1,
	0x5b, 0xdb, 0xf4, 0xf7,
	0xfc, 0xa2, 0x83, 0x19,
	0xdb, 0x1a, 0x3e, 0x94,
	0x6b, 0x9e, 0x38, 0xd9,
	0x2c, 0x9c, 0x03, 0xd1,
	0xad, 0x99, 0x44, 0xa7,
	0xd9, 0x56, 0x3d, 0x59,
	0x06, 0x3c, 0x25, 0xf3,
	0xfc, 0x1f, 0xdc, 0x2a,
};

int
mana_rq_ring_doorbell(struct mana_rxq *rxq)
{
	struct mana_priv *priv = rxq->priv;
	int ret;
	void *db_page = priv->db_page;

	if (rte_eal_process_type() == RTE_PROC_SECONDARY) {
		struct rte_eth_dev *dev =
			&rte_eth_devices[priv->dev_data->port_id];
		struct mana_process_priv *process_priv = dev->process_private;

		db_page = process_priv->db_page;
	}

	ret = mana_ring_doorbell(db_page, GDMA_QUEUE_RECEIVE,
				 rxq->gdma_rq.id,
				 rxq->gdma_rq.head *
					GDMA_WQE_ALIGNMENT_UNIT_SIZE);

	if (ret)
		DRV_LOG(ERR, "failed to ring RX doorbell ret %d", ret);

	return ret;
}

static int
mana_alloc_and_post_rx_wqe(struct mana_rxq *rxq)
{
	struct rte_mbuf *mbuf = NULL;
	struct gdma_sgl_element sgl[1];
	struct gdma_work_request request = {0};
	struct gdma_posted_wqe_info wqe_info = {0};
	struct mana_priv *priv = rxq->priv;
	int ret;
	struct mana_mr_cache *mr;

	mbuf = rte_pktmbuf_alloc(rxq->mp);
	if (!mbuf) {
		rxq->stats.nombuf++;
		return -ENOMEM;
	}

	mr = mana_find_pmd_mr(&rxq->mr_btree, priv, mbuf);
	if (!mr) {
		DRV_LOG(ERR, "failed to register RX MR");
		rte_pktmbuf_free(mbuf);
		return -ENOMEM;
	}

	request.gdma_header.struct_size = sizeof(request);
	wqe_info.gdma_header.struct_size = sizeof(wqe_info);

	sgl[0].address = rte_cpu_to_le_64(rte_pktmbuf_mtod(mbuf, uint64_t));
	sgl[0].memory_key = mr->lkey;
	sgl[0].size =
		rte_pktmbuf_data_room_size(rxq->mp) -
		RTE_PKTMBUF_HEADROOM;

	request.sgl = sgl;
	request.num_sgl_elements = 1;
	request.inline_oob_data = NULL;
	request.inline_oob_size_in_bytes = 0;
	request.flags = 0;
	request.client_data_unit = NOT_USING_CLIENT_DATA_UNIT;

	ret = gdma_post_work_request(&rxq->gdma_rq, &request, &wqe_info);
	if (!ret) {
		struct mana_rxq_desc *desc =
			&rxq->desc_ring[rxq->desc_ring_head];

		/* update queue for tracking pending packets */
		desc->pkt = mbuf;
		desc->wqe_size_in_bu = wqe_info.wqe_size_in_bu;
		rxq->desc_ring_head = (rxq->desc_ring_head + 1) % rxq->num_desc;
	} else {
		DRV_LOG(ERR, "failed to post recv ret %d", ret);
		return ret;
	}

	return 0;
}

/*
 * Post work requests for a Rx queue.
 */
static int
mana_alloc_and_post_rx_wqes(struct mana_rxq *rxq)
{
	int ret;
	uint32_t i;

	for (i = 0; i < rxq->num_desc; i++) {
		ret = mana_alloc_and_post_rx_wqe(rxq);
		if (ret) {
			DRV_LOG(ERR, "failed to post RX ret = %d", ret);
			return ret;
		}
	}

	mana_rq_ring_doorbell(rxq);

	return ret;
}

int
mana_stop_rx_queues(struct rte_eth_dev *dev)
{
	struct mana_priv *priv = dev->data->dev_private;
	int ret, i;

	if (priv->rwq_qp) {
		ret = ibv_destroy_qp(priv->rwq_qp);
		if (ret)
			DRV_LOG(ERR, "rx_queue destroy_qp failed %d", ret);
		priv->rwq_qp = NULL;
	}

	if (priv->ind_table) {
		ret = ibv_destroy_rwq_ind_table(priv->ind_table);
		if (ret)
			DRV_LOG(ERR, "destroy rwq ind table failed %d", ret);
		priv->ind_table = NULL;
	}

	for (i = 0; i < priv->num_queues; i++) {
		struct mana_rxq *rxq = dev->data->rx_queues[i];

		if (rxq->wq) {
			ret = ibv_destroy_wq(rxq->wq);
			if (ret)
				DRV_LOG(ERR,
					"rx_queue destroy_wq failed %d", ret);
			rxq->wq = NULL;
		}

		if (rxq->cq) {
			ret = ibv_destroy_cq(rxq->cq);
			if (ret)
				DRV_LOG(ERR,
					"rx_queue destroy_cq failed %d", ret);
			rxq->cq = NULL;
		}

		/* Drain and free posted WQEs */
		while (rxq->desc_ring_tail != rxq->desc_ring_head) {
			struct mana_rxq_desc *desc =
				&rxq->desc_ring[rxq->desc_ring_tail];

			rte_pktmbuf_free(desc->pkt);

			rxq->desc_ring_tail =
				(rxq->desc_ring_tail + 1) % rxq->num_desc;
		}
		rxq->desc_ring_head = 0;
		rxq->desc_ring_tail = 0;

		memset(&rxq->gdma_rq, 0, sizeof(rxq->gdma_rq));
		memset(&rxq->gdma_cq, 0, sizeof(rxq->gdma_cq));
	}
	return 0;
}

int
mana_start_rx_queues(struct rte_eth_dev *dev)
{
	struct mana_priv *priv = dev->data->dev_private;
	int ret, i;
	struct ibv_wq *ind_tbl[priv->num_queues];

	DRV_LOG(INFO, "start rx queues");
	for (i = 0; i < priv->num_queues; i++) {
		struct mana_rxq *rxq = dev->data->rx_queues[i];
		struct ibv_wq_init_attr wq_attr = {};

		manadv_set_context_attr(priv->ib_ctx,
			MANADV_CTX_ATTR_BUF_ALLOCATORS,
			(void *)((uintptr_t)&(struct manadv_ctx_allocators){
				.alloc = &mana_alloc_verbs_buf,
				.free = &mana_free_verbs_buf,
				.data = (void *)(uintptr_t)rxq->socket,
			}));

		rxq->cq = ibv_create_cq(priv->ib_ctx, rxq->num_desc,
					NULL, NULL, 0);
		if (!rxq->cq) {
			ret = -errno;
			DRV_LOG(ERR, "failed to create rx cq queue %d", i);
			goto fail;
		}

		wq_attr.wq_type = IBV_WQT_RQ;
		wq_attr.max_wr = rxq->num_desc;
		wq_attr.max_sge = 1;
		wq_attr.pd = priv->ib_parent_pd;
		wq_attr.cq = rxq->cq;

		rxq->wq = ibv_create_wq(priv->ib_ctx, &wq_attr);
		if (!rxq->wq) {
			ret = -errno;
			DRV_LOG(ERR, "failed to create rx wq %d", i);
			goto fail;
		}

		ind_tbl[i] = rxq->wq;
	}

	struct ibv_rwq_ind_table_init_attr ind_table_attr = {
		.log_ind_tbl_size = rte_log2_u32(RTE_DIM(ind_tbl)),
		.ind_tbl = ind_tbl,
		.comp_mask = 0,
	};

	priv->ind_table = ibv_create_rwq_ind_table(priv->ib_ctx,
						   &ind_table_attr);
	if (!priv->ind_table) {
		ret = -errno;
		DRV_LOG(ERR, "failed to create ind_table ret %d", ret);
		goto fail;
	}

	DRV_LOG(INFO, "ind_table handle %d num %d",
		priv->ind_table->ind_tbl_handle,
		priv->ind_table->ind_tbl_num);

	struct ibv_qp_init_attr_ex qp_attr_ex = {
		.comp_mask = IBV_QP_INIT_ATTR_PD |
			     IBV_QP_INIT_ATTR_RX_HASH |
			     IBV_QP_INIT_ATTR_IND_TABLE,
		.qp_type = IBV_QPT_RAW_PACKET,
		.pd = priv->ib_parent_pd,
		.rwq_ind_tbl = priv->ind_table,
		.rx_hash_conf = {
			.rx_hash_function = IBV_RX_HASH_FUNC_TOEPLITZ,
			.rx_hash_key_len = TOEPLITZ_HASH_KEY_SIZE_IN_BYTES,
			.rx_hash_key = mana_rss_hash_key_default,
			.rx_hash_fields_mask =
				IBV_RX_HASH_SRC_IPV4 | IBV_RX_HASH_DST_IPV4,
		},

	};

	/* overwrite default if rss key is set */
	if (priv->rss_conf.rss_key_len && priv->rss_conf.rss_key)
		qp_attr_ex.rx_hash_conf.rx_hash_key =
			priv->rss_conf.rss_key;

	/* overwrite default if rss hash fields are set */
	if (priv->rss_conf.rss_hf) {
		qp_attr_ex.rx_hash_conf.rx_hash_fields_mask = 0;

		if (priv->rss_conf.rss_hf & RTE_ETH_RSS_IPV4)
			qp_attr_ex.rx_hash_conf.rx_hash_fields_mask |=
				IBV_RX_HASH_SRC_IPV4 | IBV_RX_HASH_DST_IPV4;

		if (priv->rss_conf.rss_hf & RTE_ETH_RSS_IPV6)
			qp_attr_ex.rx_hash_conf.rx_hash_fields_mask |=
				IBV_RX_HASH_SRC_IPV6 | IBV_RX_HASH_SRC_IPV6;

		if (priv->rss_conf.rss_hf &
		    (RTE_ETH_RSS_NONFRAG_IPV4_TCP | RTE_ETH_RSS_NONFRAG_IPV6_TCP))
			qp_attr_ex.rx_hash_conf.rx_hash_fields_mask |=
				IBV_RX_HASH_SRC_PORT_TCP |
				IBV_RX_HASH_DST_PORT_TCP;

		if (priv->rss_conf.rss_hf &
		    (RTE_ETH_RSS_NONFRAG_IPV4_UDP | RTE_ETH_RSS_NONFRAG_IPV6_UDP))
			qp_attr_ex.rx_hash_conf.rx_hash_fields_mask |=
				IBV_RX_HASH_SRC_PORT_UDP |
				IBV_RX_HASH_DST_PORT_UDP;
	}

	priv->rwq_qp = ibv_create_qp_ex(priv->ib_ctx, &qp_attr_ex);
	if (!priv->rwq_qp) {
		ret = -errno;
		DRV_LOG(ERR, "rx ibv_create_qp_ex failed");
		goto fail;
	}

	for (i = 0; i < priv->num_queues; i++) {
		struct mana_rxq *rxq = dev->data->rx_queues[i];
		struct manadv_obj obj = {};
		struct manadv_cq dv_cq;
		struct manadv_rwq dv_wq;

		obj.cq.in = rxq->cq;
		obj.cq.out = &dv_cq;
		obj.rwq.in = rxq->wq;
		obj.rwq.out = &dv_wq;
		ret = manadv_init_obj(&obj, MANADV_OBJ_CQ | MANADV_OBJ_RWQ);
		if (ret) {
			DRV_LOG(ERR, "manadv_init_obj failed ret %d", ret);
			goto fail;
		}

		rxq->gdma_cq.buffer = obj.cq.out->buf;
		rxq->gdma_cq.count = obj.cq.out->count;
		rxq->gdma_cq.size = rxq->gdma_cq.count * COMP_ENTRY_SIZE;
		rxq->gdma_cq.id = obj.cq.out->cq_id;

		/* CQ head starts with count */
		rxq->gdma_cq.head = rxq->gdma_cq.count;

		DRV_LOG(INFO, "rxq cq id %u buf %p count %u size %u",
			rxq->gdma_cq.id, rxq->gdma_cq.buffer,
			rxq->gdma_cq.count, rxq->gdma_cq.size);

		priv->db_page = obj.rwq.out->db_page;

		rxq->gdma_rq.buffer = obj.rwq.out->buf;
		rxq->gdma_rq.count = obj.rwq.out->count;
		rxq->gdma_rq.size = obj.rwq.out->size;
		rxq->gdma_rq.id = obj.rwq.out->wq_id;

		DRV_LOG(INFO, "rxq rq id %u buf %p count %u size %u",
			rxq->gdma_rq.id, rxq->gdma_rq.buffer,
			rxq->gdma_rq.count, rxq->gdma_rq.size);
	}

	for (i = 0; i < priv->num_queues; i++) {
		ret = mana_alloc_and_post_rx_wqes(dev->data->rx_queues[i]);
		if (ret)
			goto fail;
	}

	return 0;

fail:
	mana_stop_rx_queues(dev);
	return ret;
}