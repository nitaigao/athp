/*-
 * Copyright (c) 2015 Adrian Chadd <adrian@FreeBSD.org>
 * Copyright (c) 2005-2011 Atheros Communications Inc.
 * Copyright (c) 2011-2013 Qualcomm Atheros, Inc.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Playground for QCA988x chipsets.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_wlan.h"

#include <sys/param.h>
#include <sys/endian.h>
#include <sys/sockio.h>
#include <sys/mbuf.h>
#include <sys/kernel.h>
#include <sys/socket.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/bus.h>
#include <sys/rman.h>
#include <sys/firmware.h>
#include <sys/module.h>
#include <sys/taskqueue.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <net/bpf.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/if_arp.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_types.h>

#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/in_var.h>
#include <netinet/if_ether.h>
#include <netinet/ip.h>

#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_regdomain.h>
#include <net80211/ieee80211_radiotap.h>
#include <net80211/ieee80211_ratectl.h>
#include <net80211/ieee80211_input.h>
#ifdef	IEEE80211_SUPPORT_SUPERG
#include <net80211/ieee80211_superg.h>
#endif

#include "hal/linux_compat.h"
#include "hal/targaddrs.h"
#include "hal/core.h"
#include "hal/hw.h"
#include "hal/pci.h"

#include "if_athp_debug.h"
#include "if_athp_regio.h"
#include "if_athp_var.h"
#include "if_athp_desc.h"
#include "if_athp_pci_ce.h"
#include "if_athp_pci_pipe.h"
#include "if_athp_pci.h"
#include "if_athp_regio.h"
#include "if_athp_pci_chip.h"

/*
 * This is the PCI pipe related code from ath10k/pci.c.
 *
 * This implements the bottom level data pipe abstraction
 * used by the copyengine to do DMA and the task contexts
 * with which to do said DMA/interrupt handling.
 *
 * Each PCI pipe has a copy-engine ring; the copy engine
 * ring takes care of doing the TX/RX bits and this layer
 * just hands it buffers for transmit/receive.  The CE
 * will call the supplied TX/RX completion callback to notify
 * that things are done.
 *
 * The copy-engine code only "knows" about DMA memory enough
 * to setup and manage the descriptor rings.  The PCI pipe code
 * here handles mapping and unmapping actual buffer contents
 * into athp_buf entries and mbuf entries as appropriate.
 * I'm hoping that continuing this enforced split will make
 * it easier to port this driver to other BSDs and other operating
 * systems.
 */

/*
 * XXX TODO: make the functions take athp_pci_softc * as the top-level
 * state, instead of athp_softc * ?
 */

static const struct ce_attr host_ce_config_wlan[] = {
	/* CE0: host->target HTC control and raw streams */
	{
		.flags = CE_ATTR_FLAGS,
		.src_nentries = 16,
		.src_sz_max = 256,
		.dest_nentries = 0,
	},

	/* CE1: target->host HTT + HTC control */
	{
		.flags = CE_ATTR_FLAGS,
		.src_nentries = 0,
		.src_sz_max = 2048,
		.dest_nentries = 512,
	},

	/* CE2: target->host WMI */
	{
		.flags = CE_ATTR_FLAGS,
		.src_nentries = 0,
		.src_sz_max = 2048,
		.dest_nentries = 128,
	},

	/* CE3: host->target WMI */
	{
		.flags = CE_ATTR_FLAGS,
		.src_nentries = 32,
		.src_sz_max = 2048,
		.dest_nentries = 0,
	},

	/* CE4: host->target HTT */
	{
		.flags = CE_ATTR_FLAGS | CE_ATTR_DIS_INTR,
		.src_nentries = CE_HTT_H2T_MSG_SRC_NENTRIES,
		.src_sz_max = 256,
		.dest_nentries = 0,
	},

	/* CE5: unused */
	{
		.flags = CE_ATTR_FLAGS,
		.src_nentries = 0,
		.src_sz_max = 0,
		.dest_nentries = 0,
	},

	/* CE6: target autonomous hif_memcpy */
	{
		.flags = CE_ATTR_FLAGS,
		.src_nentries = 0,
		.src_sz_max = 0,
		.dest_nentries = 0,
	},

	/* CE7: ce_diag, the Diagnostic Window */
	{
		.flags = CE_ATTR_FLAGS,
		.src_nentries = 2,
		.src_sz_max = DIAG_TRANSFER_LIMIT,
		.dest_nentries = 2,
	},

	/* CE8: target->host pktlog */
	{
		.flags = CE_ATTR_FLAGS,
		.src_nentries = 0,
		.src_sz_max = 2048,
		.dest_nentries = 128,
	},

	/* CE9 target autonomous qcache memcpy */
	{
		.flags = CE_ATTR_FLAGS,
		.src_nentries = 0,
		.src_sz_max = 0,
		.dest_nentries = 0,
	},

	/* CE10: target autonomous hif memcpy */
	{
		.flags = CE_ATTR_FLAGS,
		.src_nentries = 0,
		.src_sz_max = 0,
		.dest_nentries = 0,
	},

	/* CE11: target autonomous hif memcpy */
	{
		.flags = CE_ATTR_FLAGS,
		.src_nentries = 0,
		.src_sz_max = 0,
		.dest_nentries = 0,
	},
};

static int
__ath10k_pci_rx_post_buf(struct ath10k_pci_pipe *pipe)
{
	struct athp_softc *sc = pipe->sc;
#if 0
	struct ath10k_pci *psc = pipe->psc;
	struct ath10k_ce_pipe *ce_pipe = pipe->ce_hdl;
	struct mbuf *m;
	int ret;

	ATHP_PCI_CE_LOCK_ASSERT(psc);

	m = m_get2(pipe->buf_sz, M_NOWAIT, MT_DATA, M_PKTHDR);
	if (m == NULL)
		return -ENOMEM;

	/* XXX TODO need to ensure it's correctly aligned */
	WARN_ONCE((unsigned long)skb->data & 3, "unaligned skb");

	/*
	 * Ok, here's the fun FreeBSD specific bit.
	 *
	 * Each slot in the ring needs to have the busdma state
	 * in order to map/unmap/etc as appropriate.
	 *
	 * The linux driver doesn't have it; it creates a physical
	 * mapping via dma_map_single() and stores the paddr;
	 * it doesn't have a per-slot struct to store state in.
	 * It then crafts some custom stuff in skb->cb to store
	 * the paddr in and some hash membership for doing later
	 * paddr -> vaddr lookups.  Odd, but okay.
	 *
	 * So, I need to go and craft that stuff (ie, what "ath_buf"
	 * partially covers in the FreeBSD ath(4) driver) as part
	 * of the transmit/receive infrastructure.
	 */

	/*
	 * That per-slot state also needs to be kept in the ring,
	 * like what's done in iwn for the RX ring management.
	 * There's the 'per_transfer_context' that's used by the
	 * CE code that I think we can piggyback this state struct
	 * into, which will change how things work a little compared
	 * to linux.
	 */

	paddr = dma_map_single(ar->dev, skb->data,
			       skb->len + skb_tailroom(skb),
			       DMA_FROM_DEVICE);
	if (unlikely(dma_mapping_error(ar->dev, paddr))) {
		ath10k_warn(ar, "failed to dma map pci rx buf\n");
		dev_kfree_skb_any(skb);
		return -EIO;
	}

	ATH10K_SKB_RXCB(skb)->paddr = paddr;

	/*
	 * Once the mapping is done and we've verified there's only
	 * a single physical segment, we can hand it to the copy engine
	 * to queue for receive.
	 */
	ret = __ath10k_ce_rx_post_buf(ce_pipe, skb, paddr);
	if (ret) {
		ath10k_warn(ar, "failed to post pci rx buf: %d\n", ret);
		dma_unmap_single(ar->dev, paddr, skb->len + skb_tailroom(skb),
				 DMA_FROM_DEVICE);
		dev_kfree_skb_any(skb);
		return ret;
	}
#else
	device_printf(sc->sc_dev, "%s: called\n", __func__);
#endif
	return 0;
}

static void
__ath10k_pci_rx_post_pipe(struct ath10k_pci_pipe *pipe)
{
	struct athp_softc *sc = pipe->sc;
#if 0
	struct ath10k_pci *ar_pci = ath10k_pci_priv(ar);
	struct ath10k_ce_pipe *ce_pipe = pipe->ce_hdl;
	int ret, num;

	lockdep_assert_held(&ar_pci->ce_lock);

	if (pipe->buf_sz == 0)
		return;

	if (!ce_pipe->dest_ring)
		return;

	num = __ath10k_ce_rx_num_free_bufs(ce_pipe);
	while (num--) {
		ret = __ath10k_pci_rx_post_buf(pipe);
		if (ret) {
			ath10k_warn(ar, "failed to post pci rx buf: %d\n", ret);
			mod_timer(&ar_pci->rx_post_retry, jiffies +
				  ATH10K_PCI_RX_POST_RETRY_MS);
			break;
		}
	}
#else
	device_printf(sc->sc_dev, "%s: called\n", __func__);
#endif
}

static void
ath10k_pci_rx_post_pipe(struct ath10k_pci_pipe *pipe)
{
//	struct athp_softc *sc = pipe->sc;
	struct athp_pci_softc *psc = pipe->psc;

	ATHP_PCI_CE_LOCK(psc);
	__ath10k_pci_rx_post_pipe(pipe);
	ATHP_PCI_CE_UNLOCK(psc);
}

static void
ath10k_pci_rx_post(struct athp_softc *sc)
{
	struct athp_pci_softc *psc = sc->sc_psc;
	int i;

	ATHP_PCI_CE_LOCK(psc);
	for (i = 0; i < CE_COUNT(sc); i++)
		__ath10k_pci_rx_post_pipe(&psc->pipe_info[i]);
	ATHP_PCI_CE_UNLOCK(psc);
}

/*
 * This is the deferred RX post taskqueue entry.
 * It checks /all/ RX pipes.
 */
static void
ath10k_pci_rx_replenish_retry(unsigned long ptr)
{
	struct athp_softc *sc = (void *)ptr;

	ath10k_pci_rx_post(sc);
}

/* Called by lower (CE) layer when a send to Target completes. */
static void
ath10k_pci_ce_send_done(struct ath10k_ce_pipe *ce_state)
{
	struct athp_softc *sc = ce_state->sc;
#if 0
	struct athp_softc *psc = ce_state->psc;
	struct ath10k_hif_cb *cb = &ar_pci->msg_callbacks_current;
	struct sk_buff_head list;
	struct sk_buff *skb;
	u32 ce_data;
	unsigned int nbytes;
	unsigned int transfer_id;

	__skb_queue_head_init(&list);
	while (ath10k_ce_completed_send_next(ce_state, (void **)&skb, &ce_data,
					     &nbytes, &transfer_id) == 0) {
		/* no need to call tx completion for NULL pointers */
		if (skb == NULL)
			continue;

		__skb_queue_tail(&list, skb);
	}

	while ((skb = __skb_dequeue(&list)))
		cb->tx_completion(ar, skb);
#else
	device_printf(sc->sc_dev, "%s: called\n", __func__);
#endif
}

/* Called by lower (CE) layer when data is received from the Target. */
static void
ath10k_pci_ce_recv_data(struct ath10k_ce_pipe *ce_state)
{
	struct athp_softc *sc = ce_state->sc;
#if 0
	struct ath10k_pci *ar_pci = ath10k_pci_priv(ar);
	struct ath10k_pci_pipe *pipe_info =  &ar_pci->pipe_info[ce_state->id];
	struct ath10k_hif_cb *cb = &ar_pci->msg_callbacks_current;
	struct sk_buff *skb;
	struct sk_buff_head list;
	void *transfer_context;
	u32 ce_data;
	unsigned int nbytes, max_nbytes;
	unsigned int transfer_id;
	unsigned int flags;

	__skb_queue_head_init(&list);
	while (ath10k_ce_completed_recv_next(ce_state, &transfer_context,
					     &ce_data, &nbytes, &transfer_id,
					     &flags) == 0) {
		skb = transfer_context;
		max_nbytes = skb->len + skb_tailroom(skb);
		dma_unmap_single(ar->dev, ATH10K_SKB_RXCB(skb)->paddr,
				 max_nbytes, DMA_FROM_DEVICE);

		if (unlikely(max_nbytes < nbytes)) {
			ath10k_warn(ar, "rxed more than expected (nbytes %d, max %d)",
				    nbytes, max_nbytes);
			dev_kfree_skb_any(skb);
			continue;
		}

		skb_put(skb, nbytes);
		__skb_queue_tail(&list, skb);
	}

	while ((skb = __skb_dequeue(&list))) {
		ath10k_dbg(ar, ATH10K_DBG_PCI, "pci rx ce pipe %d len %d\n",
			   ce_state->id, skb->len);
		ath10k_dbg_dump(ar, ATH10K_DBG_PCI_DUMP, NULL, "pci rx: ",
				skb->data, skb->len);

		cb->rx_completion(ar, skb);
	}

	ath10k_pci_rx_post_pipe(pipe_info);
#else
	device_printf(sc->sc_dev, "%s: called\n", __func__);
#endif
}

/*
 * TODO: This should be broken out into "kill per-pipe and rx post
 * retry task" routine and the "kill interrupts" routine.
 * That way the interrupts task can be killed in the bus code,
 * and we here kill the pipe/rx deferred tasks.
 */
static void
ath10k_pci_kill_tasklet(struct athp_softc *sc)
{
#if 0
	struct ath10k_pci *ar_pci = ath10k_pci_priv(ar);
	int i;

	tasklet_kill(&ar_pci->intr_tq);
	tasklet_kill(&ar_pci->msi_fw_err);

	for (i = 0; i < CE_COUNT; i++)
		tasklet_kill(&ar_pci->pipe_info[i].intr);

	del_timer_sync(&ar_pci->rx_post_retry);
#else
	device_printf(sc->sc_dev, "%s: called\n", __func__);
#endif
}

static void
ath10k_pci_rx_pipe_cleanup(struct ath10k_pci_pipe *pci_pipe)
{
	struct athp_softc *sc = pci_pipe->sc;
#if 0
	struct ath10k_ce_pipe *ce_pipe;
	struct ath10k_ce_ring *ce_ring;
	struct sk_buff *skb;
	int i;

	ar = pci_pipe->hif_ce_state;
	ce_pipe = pci_pipe->ce_hdl;
	ce_ring = ce_pipe->dest_ring;

	if (!ce_ring)
		return;

	if (!pci_pipe->buf_sz)
		return;

	for (i = 0; i < ce_ring->nentries; i++) {
		skb = ce_ring->per_transfer_context[i];
		if (!skb)
			continue;

		ce_ring->per_transfer_context[i] = NULL;

		dma_unmap_single(ar->dev, ATH10K_SKB_RXCB(skb)->paddr,
				 skb->len + skb_tailroom(skb),
				 DMA_FROM_DEVICE);
		dev_kfree_skb_any(skb);
	}
#else
	device_printf(sc->sc_dev, "%s: called\n", __func__);
#endif
}

static void ath10k_pci_tx_pipe_cleanup(struct ath10k_pci_pipe *pci_pipe)
{
	struct athp_softc *sc = pci_pipe->sc;
#if 0
	struct ath10k_pci *ar_pci;
	struct ath10k_ce_pipe *ce_pipe;
	struct ath10k_ce_ring *ce_ring;
	struct ce_desc *ce_desc;
	struct sk_buff *skb;
	int i;

	ar = pci_pipe->hif_ce_state;
	ar_pci = ath10k_pci_priv(ar);
	ce_pipe = pci_pipe->ce_hdl;
	ce_ring = ce_pipe->src_ring;

	if (!ce_ring)
		return;

	if (!pci_pipe->buf_sz)
		return;

	ce_desc = ce_ring->shadow_base;
	if (WARN_ON(!ce_desc))
		return;

	for (i = 0; i < ce_ring->nentries; i++) {
		skb = ce_ring->per_transfer_context[i];
		if (!skb)
			continue;

		ce_ring->per_transfer_context[i] = NULL;

		ar_pci->msg_callbacks_current.tx_completion(ar, skb);
	}
#else
	device_printf(sc->sc_dev, "%s: called\n", __func__);
#endif
}

/*
 * Cleanup residual buffers for device shutdown:
 *    buffers that were enqueued for receive
 *    buffers that were to be sent
 * Note: Buffers that had completed but which were
 * not yet processed are on a completion queue. They
 * are handled when the completion thread shuts down.
 */
static void
ath10k_pci_buffer_cleanup(struct athp_softc *sc)
{
	struct athp_pci_softc *psc = sc->sc_psc;
	int pipe_num;

	for (pipe_num = 0; pipe_num < CE_COUNT(sc); pipe_num++) {
		struct ath10k_pci_pipe *pipe_info;

		pipe_info = &psc->pipe_info[pipe_num];
		ath10k_pci_rx_pipe_cleanup(pipe_info);
		ath10k_pci_tx_pipe_cleanup(pipe_info);
	}
}

void
ath10k_pci_ce_deinit(struct athp_softc *sc)
{
	int i;

	for (i = 0; i < CE_COUNT(sc); i++)
		ath10k_ce_deinit_pipe(sc, i);
}

static void
ath10k_pci_flush(struct athp_softc *sc)
{
	ath10k_pci_kill_tasklet(sc);
	ath10k_pci_buffer_cleanup(sc);
}

int
ath10k_pci_alloc_pipes(struct athp_softc *sc)
{
	struct athp_pci_softc *psc = sc->sc_psc;
	struct ath10k_pci_pipe *pipe;
	int i, ret;
	int sz;

	for (i = 0; i < CE_COUNT(sc); i++) {
		pipe = &psc->pipe_info[i];
		pipe->ce_hdl = &psc->ce_states[i];
		pipe->pipe_num = i;
		pipe->sc = sc;
		pipe->psc = psc;

		ret = ath10k_ce_alloc_pipe(sc, i, &host_ce_config_wlan[i],
					   ath10k_pci_ce_send_done,
					   ath10k_pci_ce_recv_data);
		if (ret) {
			ATHP_ERR(sc,
			    "failed to allocate copy engine pipe %d: %d\n",
			    i, ret);
			return ret;
		}

		/* Last CE is Diagnostic Window */
		if (i == CE_DIAG_PIPE) {
			psc->ce_diag = pipe->ce_hdl;
			continue;
		}

		/*
		 * Set maximum transfer size for this pipe.
		 */
		pipe->buf_sz = (size_t)(host_ce_config_wlan[i].src_sz_max);

		/*
		 * And initialise a dmatag for this pipe that correctly
		 * represents the maximum DMA transfer size.
		 *
		 * XXX TODO: sigh; some of these pipes have buf_sz set to 0
		 * on the host side, but the target side has either 2k or 4k
		 * limits.  I don't understand why yet.  So, to work around it
		 * and hope that it doesn't come back to bite me - configure
		 * up a mapping, but for 4KB.
		 */
		if (pipe->buf_sz == 0) {
			sz = 4096;
			device_printf(sc->sc_dev,
			    "%s: WARNING: configuring 4k dmamap size for pipe %d; figure out what to do instead\n",
			    __func__, i);
		} else {
			sz = pipe->buf_sz;
		}
		ret = athp_dma_head_alloc(sc, &pipe->dmatag, sz);
		if (ret) {
			ATHP_ERR(sc, "%s: failed to create dma tag for pipe %d\n",
			    __func__,
			    i);
			return (ret);
		}
	}

	return 0;
}

void
ath10k_pci_free_pipes(struct athp_softc *sc)
{
	int i;
	struct athp_pci_softc *psc = sc->sc_psc;
	struct ath10k_pci_pipe *pipe;

	for (i = 0; i < CE_COUNT(sc); i++) {
		pipe = &psc->pipe_info[i];
		ath10k_ce_free_pipe(sc, i);
		athp_dma_head_free(sc, &pipe->dmatag);
	}
}

static int
ath10k_pci_init_pipes(struct athp_softc *sc)
{
	int i, ret;

	for (i = 0; i < CE_COUNT(sc); i++) {
		ret = ath10k_ce_init_pipe(sc, i, &host_ce_config_wlan[i]);
		if (ret) {
			ATHP_ERR(sc,
			    "failed to initialize copy engine pipe %d: %d\n",
			    i, ret);
			return ret;
		}
	}

	return 0;
}