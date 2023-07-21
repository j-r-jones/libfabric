/*
 * Copyright (c) Intel Corporation. All rights reserved
 * Copyright (c) Amazon.com, Inc. or its affiliates. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
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

#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>

#include "ofi_atom.h"
#include "ofi_hmem.h"
#include "ofi_iov.h"
#include "ofi_mr.h"
#include "sm2.h"
#include "sm2_fifo.h"

static inline int sm2_cma_loop(pid_t pid, struct iovec *local,
			       unsigned long local_cnt, struct iovec *remote,
			       unsigned long remote_cnt, size_t total,
			       bool write)
{
	ssize_t ret;
	struct sm2_ep_allocation_entry *entries = sm2_mmap_entries(ep->mmap);
	struct sm2_cma_data *cma_data =
		(struct sm2_cma_data *) xfer_entry->user_data;

	pid_t pid = entries[xfer_entry->hdr.sender_gid].pid;

	while (1) {
		if (write)
			ret = ofi_process_vm_writev(pid, local, local_cnt,
						    remote, remote_cnt, 0);
		else
			ret = ofi_process_vm_readv(pid, local, local_cnt,
						   remote, remote_cnt, 0);
		if (ret < 0) {
			FI_WARN(&sm2_prov, FI_LOG_EP_CTRL, "CMA error %d\n",
				errno);
			return -FI_EIO;
		}

		total -= ret;
		if (!total)
			return FI_SUCCESS;

		ofi_consume_iov(local, &local_cnt, (size_t) ret);
		ofi_consume_iov(remote, &remote_cnt, (size_t) ret);
	}
}

static int sm2_progress_cma(struct sm2_xfer_entry *xfer_entry,
			    struct iovec *iov, size_t iov_count,
			    size_t *total_len, struct sm2_ep *ep, int err)
{
	struct sm2_cma_data *cma_data =
		(struct sm2_cma_data *) xfer_entry->user_data;
	struct sm2_ep_allocation_entry *entries = sm2_mmap_entries(ep->mmap);
	int ret;

	/* TODO Need to update last argument for RMA support (as well as generic
	 * format) */
	ret = sm2_cma_loop(entries[xfer_entry->hdr.sender_gid].pid, iov,
			   iov_count, cma_data->iov, cma_data->iov_count,
			   xfer_entry->hdr.size, false);
	if (!ret)
		*total_len = xfer_entry->hdr.size;

	return -ret;
}

static int sm2_progress_inject(struct sm2_xfer_entry *xfer_entry,
			       struct ofi_mr **mr, struct iovec *iov,
			       size_t iov_count, size_t *total_len,
			       struct sm2_ep *ep, int err)
{
	ssize_t hmem_copy_ret;

	hmem_copy_ret =
		ofi_copy_to_mr_iov(mr, iov, iov_count, 0, xfer_entry->user_data,
				   xfer_entry->hdr.size);

	if (hmem_copy_ret < 0) {
		FI_WARN(&sm2_prov, FI_LOG_EP_CTRL,
			"Inject recv failed with code %d\n",
			(int) (-hmem_copy_ret));
		return hmem_copy_ret;
	} else if (hmem_copy_ret != xfer_entry->hdr.size) {
		FI_WARN(&sm2_prov, FI_LOG_EP_CTRL, "Inject recv truncated\n");
		return -FI_ETRUNC;
	}

	*total_len = hmem_copy_ret;

	return FI_SUCCESS;
}

static int sm2_start_common(struct sm2_ep *ep,
			    struct sm2_xfer_entry *xfer_entry,
			    struct fi_peer_rx_entry *rx_entry)
{
	size_t total_len = 0;
	uint64_t comp_flags;
	void *comp_buf;
	int err = 0, ret = 0;
	struct sm2_xfer_entry *new_xfer_entry;

	switch (xfer_entry->hdr.proto) {
	case sm2_proto_inject:
		err = sm2_progress_inject(
			xfer_entry, (struct ofi_mr **) rx_entry->desc,
			rx_entry->iov, rx_entry->count, &total_len, ep, 0);
		break;
	case sm2_proto_cma:
		err = sm2_progress_cma(xfer_entry, rx_entry->iov,
				       rx_entry->count, &total_len, ep, 0);
		break;
	default:
		FI_WARN(&sm2_prov, FI_LOG_EP_CTRL,
			"Unidentified operation type\n");
		err = -FI_EINVAL;
	}

	comp_buf = rx_entry->iov[0].iov_base;
	comp_flags = sm2_rx_cq_flags(xfer_entry->hdr.op, rx_entry->flags,
				     xfer_entry->hdr.op_flags);

	if (err) {
		FI_WARN(&sm2_prov, FI_LOG_EP_CTRL, "Error processing op\n");
		ret = sm2_write_err_comp(ep->util_ep.rx_cq, rx_entry->context,
					 comp_flags, rx_entry->tag, err);
	} else {
		ret = sm2_complete_rx(
			ep, rx_entry->context, xfer_entry->hdr.op, comp_flags,
			total_len, comp_buf, xfer_entry->hdr.sender_gid,
			xfer_entry->hdr.tag, xfer_entry->hdr.cq_data);
	}

	if (ret) {
		FI_WARN(&sm2_prov, FI_LOG_EP_CTRL,
			"Unable to process rx completion\n");
	}

	if (!sm2_proto_imm_send_comp(xfer_entry->hdr.proto))
		xfer_entry->hdr.proto_flags |= SM2_GENERATE_COMPLETION;

	if (!(xfer_entry->hdr.proto_flags & SM2_UNEXP)) {
		sm2_fifo_write_back(ep, xfer_entry);
	} else if (!sm2_proto_imm_send_comp(xfer_entry->hdr.proto)) {
		/* The receiver has completed an unexpected receive with
		 * an xfer_entry from the xfer_ctx_pool but the sender
		 * has not generated a send completion yet. So we need
		 * to send a new xfer_entry from the receiver's
		 * freestack with the SM2_GENERATE_COMPLETION flag, so
		 * that the sender can generate a send completion
		 *
		 * TODO - Add retry. The sender will miss completions
		 * if the receiver is sending many messages and is out of
		 * xfer_entries
		 * */
		ret = sm2_pop_xfer_entry(ep, &new_xfer_entry);
		if (ret) {
			FI_WARN(&sm2_prov, FI_LOG_EP_CTRL,
				"Unable to send xfer_entry back to "
				"sender. Sender has requested "
				"FI_DELIVERY_COMPLETE but will not be "
				"able to generate a send "
				"completion!\n");
			return ret;
		}

		memcpy(new_xfer_entry, xfer_entry,
		       sizeof(struct sm2_xfer_entry));
		new_xfer_entry->hdr.proto_flags |= SM2_RETURN;
		new_xfer_entry->hdr.sender_gid = ep->gid;
		sm2_fifo_write(ep, xfer_entry->hdr.sender_gid, new_xfer_entry);
	}

	sm2_get_peer_srx(ep)->owner_ops->free_entry(rx_entry);

	return 0;
}

int sm2_unexp_start(struct fi_peer_rx_entry *rx_entry)
{
	struct sm2_xfer_ctx *xfer_ctx = rx_entry->peer_context;
	int ret;

	ret = sm2_start_common(xfer_ctx->ep, &xfer_ctx->xfer_entry, rx_entry);
	ofi_buf_free(xfer_ctx);

	return ret;
}

static int sm2_alloc_xfer_entry_ctx(struct sm2_ep *ep,
				    struct fi_peer_rx_entry *rx_entry,
				    struct sm2_xfer_entry *xfer_entry)
{
	struct sm2_xfer_ctx *xfer_ctx;

	xfer_ctx = ofi_buf_alloc(ep->xfer_ctx_pool);
	if (!xfer_ctx) {
		FI_WARN(&sm2_prov, FI_LOG_EP_CTRL,
			"Error allocating xfer_entry ctx\n");
		return -FI_ENOMEM;
	}

	memcpy(&xfer_ctx->xfer_entry, xfer_entry, sizeof(*xfer_entry));
	xfer_ctx->ep = ep;

	rx_entry->size = xfer_entry->hdr.size;
	rx_entry->flags |= xfer_entry->hdr.op_flags & FI_REMOTE_CQ_DATA;
	rx_entry->cq_data = xfer_entry->hdr.cq_data;

	rx_entry->peer_context = xfer_ctx;

	return FI_SUCCESS;
}

static int sm2_progress_recv_msg(struct sm2_ep *ep,
				 struct sm2_xfer_entry *xfer_entry)
{
	struct fid_peer_srx *peer_srx = sm2_get_peer_srx(ep);
	struct fi_peer_rx_entry *rx_entry;
	struct sm2_av *sm2_av;
	fi_addr_t addr;
	int ret;

	sm2_av = container_of(ep->util_ep.av, struct sm2_av, util_av);
	addr = sm2_av->reverse_lookup[xfer_entry->hdr.sender_gid];

	if (xfer_entry->hdr.op == ofi_op_tagged) {
		ret = peer_srx->owner_ops->get_tag(
			peer_srx, addr, xfer_entry->hdr.tag, &rx_entry);
		if (ret == -FI_ENOENT) {
			xfer_entry->hdr.proto_flags |= SM2_UNEXP;
			ret = sm2_alloc_xfer_entry_ctx(ep, rx_entry,
						       xfer_entry);
			assert(!(xfer_entry->hdr.proto_flags &
				 SM2_GENERATE_COMPLETION));
			sm2_fifo_write_back(ep, xfer_entry);
			if (ret)
				return ret;

			ret = peer_srx->owner_ops->queue_tag(rx_entry);
			goto out;
		}
	} else {
		ret = peer_srx->owner_ops->get_msg(
			peer_srx, addr, xfer_entry->hdr.size, &rx_entry);
		if (ret == -FI_ENOENT) {
			xfer_entry->hdr.proto_flags |= SM2_UNEXP;
			ret = sm2_alloc_xfer_entry_ctx(ep, rx_entry,
						       xfer_entry);
			assert(!(xfer_entry->hdr.proto_flags &
				 SM2_GENERATE_COMPLETION));
			sm2_fifo_write_back(ep, xfer_entry);
			if (ret)
				return ret;

			ret = peer_srx->owner_ops->queue_msg(rx_entry);
			goto out;
		}
	}
	if (ret) {
		FI_WARN(&sm2_prov, FI_LOG_EP_CTRL, "Error getting rx_entry\n");
		return ret;
	}
	ret = sm2_start_common(ep, xfer_entry, rx_entry);

out:
	return ret < 0 ? ret : 0;
}

static void sm2_do_atomic(void *src, void *dst, void *cmp,
			  enum fi_datatype datatype, enum fi_op op, size_t cnt,
			  uint16_t proto_flags)
{
	char tmp_result[SM2_ATOMIC_INJECT_SIZE];

	if (ofi_atomic_isswap_op(op)) {
		ofi_atomic_swap_handler(op, datatype, dst, src, cmp, tmp_result,
					cnt);
	} else if (proto_flags & SM2_RMA_REQ && ofi_atomic_isreadwrite_op(op)) {
		ofi_atomic_readwrite_handler(op, datatype, dst, src, tmp_result,
					     cnt);
	} else if (ofi_atomic_iswrite_op(op)) {
		ofi_atomic_write_handler(op, datatype, dst, src, cnt);
	} else {
		FI_WARN(&sm2_prov, FI_LOG_EP_DATA,
			"invalid atomic operation\n");
	}

	if (proto_flags & SM2_RMA_REQ)
		memcpy(src, op == FI_ATOMIC_READ ? dst : tmp_result,
		       cnt * ofi_datatype_size(datatype));
}

static int sm2_progress_inject_atomic(struct sm2_xfer_entry *xfer_entry,
				      struct fi_ioc *ioc, size_t ioc_count,
				      size_t *len, struct sm2_ep *ep)
{
	struct sm2_atomic_entry *atomic_entry =
		(struct sm2_atomic_entry *) xfer_entry->user_data;
	uint8_t *src, *comp;
	int i;

	switch (xfer_entry->hdr.op) {
	case ofi_op_atomic_compare:
		src = atomic_entry->atomic_data.buf;
		comp = atomic_entry->atomic_data.comp;
		break;
	default:
		src = atomic_entry->atomic_data.data;
		comp = NULL;
		break;
	}

	for (i = *len = 0; i < ioc_count && *len < xfer_entry->hdr.size; i++) {
		sm2_do_atomic(&src[*len], ioc[i].addr,
			      comp ? &comp[*len] : NULL,
			      atomic_entry->atomic_hdr.datatype,
			      atomic_entry->atomic_hdr.atomic_op, ioc[i].count,
			      xfer_entry->hdr.proto_flags);
		*len += ioc[i].count *
			ofi_datatype_size(atomic_entry->atomic_hdr.datatype);
	}

	if (*len != xfer_entry->hdr.size) {
		FI_WARN(&sm2_prov, FI_LOG_EP_CTRL, "recv truncated");
		return -FI_ETRUNC;
	}

	return FI_SUCCESS;
}

static int sm2_progress_atomic(struct sm2_ep *ep,
			       struct sm2_xfer_entry *xfer_entry)
{
	struct sm2_atomic_entry *atomic_entry =
		(struct sm2_atomic_entry *) xfer_entry->user_data;
	struct sm2_domain *domain = container_of(
		ep->util_ep.domain, struct sm2_domain, util_domain);
	struct fi_ioc ioc[SM2_IOV_LIMIT];
	size_t i;
	size_t ioc_count = atomic_entry->atomic_hdr.rma_ioc_count;
	size_t total_len = 0;
	int err = 0, ret = 0;
	struct fi_rma_ioc *ioc_ptr;

	for (i = 0; i < ioc_count; i++) {
		ioc_ptr = &(atomic_entry->atomic_hdr.rma_ioc[i]);
		ret = ofi_mr_verify(
			&domain->util_domain.mr_map,
			ioc_ptr->count *
				ofi_datatype_size(
					atomic_entry->atomic_hdr.datatype),
			(uintptr_t *) &(ioc_ptr->addr), ioc_ptr->key,
			ofi_rx_mr_reg_flags(
				xfer_entry->hdr.op,
				atomic_entry->atomic_hdr.atomic_op));
		if (ret)
			break;

		ioc[i].addr = (void *) ioc_ptr->addr;
		ioc[i].count = ioc_ptr->count;
	}

	if (ret)
		goto out;

	err = sm2_progress_inject_atomic(xfer_entry, ioc, ioc_count, &total_len,
					 ep);

	if (err) {
		FI_WARN(&sm2_prov, FI_LOG_EP_CTRL,
			"error processing atomic op\n");
		ret = sm2_write_err_comp(
			ep->util_ep.rx_cq, NULL,
			sm2_rx_cq_flags(xfer_entry->hdr.op, 0,
					xfer_entry->hdr.op_flags),
			0, err);
	} else {
		ret = sm2_complete_rx(ep, NULL, xfer_entry->hdr.op,
				      sm2_rx_cq_flags(xfer_entry->hdr.op, 0,
						      xfer_entry->hdr.op_flags),
				      total_len, ioc_count ? ioc[0].addr : NULL,
				      xfer_entry->hdr.sender_gid, 0,
				      xfer_entry->hdr.cq_data);
	}

	if (ret) {
		FI_WARN(&sm2_prov, FI_LOG_EP_CTRL,
			"unable to process rx completion\n");
		err = ret;
	}
out:

	/* Send completion for ofi_op_atomic is generated immediately on the
	 * sender (unless FI_DELIVERY_COMPLETE flag is set). Other ops require
	 * delivery complete semantics, so set the SM2_GENERATE_COMPLETION flag
	 */
	switch (xfer_entry->hdr.op) {
	case ofi_op_atomic:
		if (xfer_entry->hdr.op_flags & FI_DELIVERY_COMPLETE)
			xfer_entry->hdr.proto_flags |= SM2_GENERATE_COMPLETION;
		break;
	default:
		xfer_entry->hdr.proto_flags |= SM2_GENERATE_COMPLETION;
		break;
	}

	sm2_fifo_write_back(ep, xfer_entry);
	return err;
}

static inline void sm2_progress_return(struct sm2_ep *ep,
				       struct sm2_xfer_entry *xfer_entry)
{
	struct sm2_atomic_entry *atomic_entry;
	int ret;

	if (xfer_entry->hdr.proto_flags & SM2_RMA_REQ) {
		atomic_entry =
			(struct sm2_atomic_entry *) xfer_entry->user_data;
		ofi_copy_to_iov(atomic_entry->atomic_hdr.result_iov,
				atomic_entry->atomic_hdr.result_iov_count, 0,
				atomic_entry->atomic_data.data,
				xfer_entry->hdr.size);
	}

	if (xfer_entry->hdr.proto_flags & SM2_GENERATE_COMPLETION) {
		ret = sm2_complete_tx(ep, (void *) xfer_entry->hdr.context,
				      xfer_entry->hdr.op,
				      xfer_entry->hdr.op_flags);
		if (ret)
			FI_WARN(&sm2_prov, FI_LOG_EP_CTRL,
				"Unable to process FI_DELIVERY_COMPLETE "
				"completion\n");
		if (xfer_entry->hdr.proto_flags & SM2_UNEXP) {
			/* xfer_entry actually allocated on receiver side, so we
			 * need to return it */
			xfer_entry->hdr.proto_flags &= ~SM2_GENERATE_COMPLETION;
			sm2_fifo_write_back(ep, xfer_entry);
			return;
		}
	}

	smr_freestack_push(sm2_freestack(ep->self_region), xfer_entry);
}

void sm2_progress_recv(struct sm2_ep *ep)
{
	struct sm2_xfer_entry *xfer_entry;
	int ret = 0, i;

	for (i = 0; i < MAX_SM2_MSGS_PROGRESSED; i++) {
		xfer_entry = sm2_fifo_read(ep);
		if (!xfer_entry)
			break;

		if (xfer_entry->hdr.proto_flags & SM2_RETURN) {
			sm2_progress_return(ep, xfer_entry);
			continue;
		}

		assert(!(xfer_entry->hdr.proto_flags &
			 SM2_GENERATE_COMPLETION));

		switch (xfer_entry->hdr.op) {
		case ofi_op_msg:
		case ofi_op_tagged:
			ret = sm2_progress_recv_msg(ep, xfer_entry);
			break;
		case ofi_op_atomic:
		case ofi_op_atomic_fetch:
		case ofi_op_atomic_compare:
			ret = sm2_progress_atomic(ep, xfer_entry);
			break;
		default:
			FI_WARN(&sm2_prov, FI_LOG_EP_CTRL,
				"Unidentified operation type\n");
			ret = -FI_EINVAL;
		}
		if (ret) {
			if (ret != -FI_EAGAIN) {
				FI_WARN(&sm2_prov, FI_LOG_EP_CTRL,
					"Error processing command\n");
			}
			break;
		}
	}
}

void sm2_ep_progress(struct util_ep *util_ep)
{
	struct sm2_ep *ep;

	ep = container_of(util_ep, struct sm2_ep, util_ep);
	ofi_genlock_lock(&ep->util_ep.lock);
	sm2_progress_recv(ep);
	ofi_genlock_unlock(&ep->util_ep.lock);
}
