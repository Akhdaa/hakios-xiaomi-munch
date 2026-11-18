/* SPDX-License-Identifier: APSL-2.0
 * H4BC Skywalk transport for bluetoothd. Adapted from redwood_btsk.c at
 * oosexgang/hakios-xnu-kernel c36d664121623009962254a7e7ff06ec91530884.
 * The UART and controller initialization are in hakios.munch_bt.c.
 */
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <sys/systm.h>
#include <sys/random.h>
#include <skywalk/os_skywalk_private.h>

extern bool munch_bt_queue_tx(const uint8_t *bytes, unsigned length);

bool munch_btsk_init(void);
void munch_btsk_close(void);
bool munch_btsk_uuid(uuid_t out);
void munch_btsk_deliver(const uint8_t *bytes, unsigned length);

static LCK_GRP_DECLARE(btsk_lck_grp, "munch-btsk");
static LCK_MTX_DECLARE(btsk_lock, &btsk_lck_grp);

#define BTSK_RXQ        16384u
static uint8_t btsk_rxq[BTSK_RXQ];
static unsigned btsk_rxq_head, btsk_rxq_count;

static nexus_controller_t btsk_ncd;
static uuid_t btsk_dom_uuid, btsk_prov_uuid, btsk_nx_uuid;
static bool btsk_ready;
static uintptr_t btsk_nx_ctx;
static kern_channel_ring_t btsk_txring, btsk_rxring;
static kern_pbufpool_t btsk_tx_pp, btsk_rx_pp;

static errno_t
btsk_dom_init(__unused kern_nexus_domain_provider_t domprov)
{
	return 0;
}

static void
btsk_dom_fini(__unused kern_nexus_domain_provider_t domprov)
{
}

static errno_t
btsk_pre_connect(__unused kern_nexus_provider_t nxprov, __unused proc_t p, kern_nexus_t nexus, __unused nexus_port_t port,
    kern_channel_t channel, void **ch_ctx)
{
	errno_t error = kern_nexus_get_pbufpool(nexus, &btsk_tx_pp, &btsk_rx_pp);

	*ch_ctx = channel;
	return error;
}

static errno_t
btsk_connected(__unused kern_nexus_provider_t nxprov, __unused kern_nexus_t nexus, __unused kern_channel_t channel)
{
	return 0;
}

static void
btsk_pre_disconnect(__unused kern_nexus_provider_t nxprov, __unused kern_nexus_t nexus, __unused kern_channel_t channel)
{
}

static void
btsk_disconnected(__unused kern_nexus_provider_t nxprov, __unused kern_nexus_t nexus, __unused kern_channel_t channel)
{
}

static errno_t
btsk_ring_init(__unused kern_nexus_provider_t nxprov, __unused kern_nexus_t nexus, __unused kern_channel_t channel,
    kern_channel_ring_t ring, boolean_t is_tx_ring, void **ring_ctx)
{
	lck_mtx_lock(&btsk_lock);
	if (is_tx_ring) {
		btsk_txring = ring;
	} else {
		btsk_rxring = ring;
	}
	lck_mtx_unlock(&btsk_lock);
	*ring_ctx = ring;
	return 0;
}

static void
btsk_ring_fini(__unused kern_nexus_provider_t nxprov, __unused kern_nexus_t nexus, kern_channel_ring_t ring)
{
	lck_mtx_lock(&btsk_lock);
	if (ring == btsk_txring) {
		btsk_txring = NULL;
	} else if (ring == btsk_rxring) {
		btsk_rxring = NULL;
		btsk_rxq_head = 0;
		btsk_rxq_count = 0;
	}
	lck_mtx_unlock(&btsk_lock);
}

static errno_t
btsk_slot_init(__unused kern_nexus_provider_t nxprov, __unused kern_nexus_t nexus, __unused kern_channel_ring_t ring,
    channel_slot_t slot, __unused uint32_t slot_id, struct kern_slot_prop **slot_prop_addr, void **pslot_ctx)
{
	*slot_prop_addr = NULL;
	*pslot_ctx = slot;
	return 0;
}

static void
btsk_slot_fini(__unused kern_nexus_provider_t nxprov, __unused kern_nexus_t nexus, __unused kern_channel_ring_t ring,
    __unused channel_slot_t slot, __unused uint32_t slot_id)
{
}

/* the user's packets (H4, one a slot) to the controller */
static errno_t
btsk_sync_tx(__unused kern_nexus_provider_t nxprov, __unused kern_nexus_t nexus, kern_channel_ring_t ring, __unused uint32_t flags)
{
	kern_channel_slot_t slot = kern_channel_get_next_slot(ring, NULL, NULL), last = NULL;

	while (slot != NULL) {
		kern_packet_t ph = kern_channel_slot_get_packet(ring, slot);

		if (ph != 0) {
			kern_buflet_t buf = kern_packet_get_next_buflet(ph, NULL);

			if (buf != NULL) {
				uint8_t *base = kern_buflet_get_data_address(buf);
				uint16_t length = kern_buflet_get_data_length(buf), offset = kern_buflet_get_data_offset(buf);

				if (base != NULL && length > 0) {
					if (!munch_bt_queue_tx(base + offset, length)) {
						break;
					}
				}
			}
			(void)kern_channel_slot_detach_packet(ring, slot, ph);
			if (btsk_tx_pp != NULL) {
				kern_pbufpool_free(btsk_tx_pp, ph);
			}
		}
		last = slot;
		slot = kern_channel_get_next_slot(ring, slot, NULL);
	}
	if (last != NULL) {
		kern_channel_advance_slot(ring, last);
	}
	(void)kern_channel_reclaim(ring);
	return 0;
}

/* the controller's bytes to the user's ring, an H4 packet a slot */
static errno_t
btsk_sync_rx(__unused kern_nexus_provider_t nxprov, __unused kern_nexus_t nexus, kern_channel_ring_t ring, __unused uint32_t flags)
{
	kern_channel_slot_t slot, last = NULL;

	(void)kern_channel_reclaim(ring);
	slot = kern_channel_get_next_slot(ring, NULL, NULL);
	lck_mtx_lock(&btsk_lock);
	while (slot != NULL && btsk_rxq_count > 0 && btsk_rx_pp != NULL) {
		kern_packet_t ph = 0;
		kern_buflet_t buf;
		uint8_t *base;
		unsigned room, n = btsk_rxq_count;

		if (kern_pbufpool_alloc_nosleep(btsk_rx_pp, 1, &ph) != 0 || ph == 0) {
			break;
		}
		buf = kern_packet_get_next_buflet(ph, NULL);
		base = buf != NULL ? kern_buflet_get_data_address(buf) : NULL;
		room = buf != NULL ? kern_buflet_get_data_limit(buf) : 0;
		if (base == NULL || room == 0) {
			kern_pbufpool_free(btsk_rx_pp, ph);
			break;
		}
		if (n > room) {
			n = room;
		}
		for (unsigned i = 0; i < n; i++) {
			base[i] = btsk_rxq[(btsk_rxq_head + i) % BTSK_RXQ];
		}
		(void)kern_buflet_set_data_offset(buf, 0);
		(void)kern_buflet_set_data_length(buf, (uint16_t)n);
		(void)kern_packet_finalize(ph);
		if (kern_channel_slot_attach_packet(ring, slot, ph) != 0) {
			kern_pbufpool_free(btsk_rx_pp, ph);
			break;
		}
		btsk_rxq_head = (btsk_rxq_head + n) % BTSK_RXQ;
		btsk_rxq_count -= n;
		last = slot;
		slot = kern_channel_get_next_slot(ring, slot, NULL);
	}
	lck_mtx_unlock(&btsk_lock);
	if (last != NULL) {
		kern_channel_advance_slot(ring, last);
	}
	return 0;
}

/* what the controller sent; wakes the user's ring */
void
munch_btsk_deliver(const uint8_t *bytes, unsigned length)
{
	lck_mtx_lock(&btsk_lock);
	if (btsk_rxring != NULL && length <= BTSK_RXQ - btsk_rxq_count) {
		for (unsigned i = 0; i < length; i++) {
			btsk_rxq[(btsk_rxq_head + btsk_rxq_count + i) % BTSK_RXQ] = bytes[i];
		}
		btsk_rxq_count += length;
		(void)kern_channel_notify(btsk_rxring, 0);
	}
	lck_mtx_unlock(&btsk_lock);
}

/* the channel's UUID, for bluetoothd's getsockopt; false until the nexus is made */
bool
munch_btsk_uuid(uuid_t out)
{
	if (!__atomic_load_n(&btsk_ready, __ATOMIC_ACQUIRE)) {
		return false;
	}
	uuid_copy(out, btsk_nx_uuid);
	return true;
}

bool
munch_btsk_init(void)
{
	nexus_attr_t nxa = NULL;
	errno_t error;
	struct kern_nexus_domain_provider_init dom_init = {
		.nxdpi_version = KERN_NEXUS_DOMAIN_PROVIDER_CURRENT_VERSION,
		.nxdpi_flags = 0,
		.nxdpi_init = btsk_dom_init,
		.nxdpi_fini = btsk_dom_fini,
	};
	struct kern_nexus_provider_init prov_init = {
		.nxpi_version = KERN_NEXUS_DOMAIN_PROVIDER_CURRENT_VERSION,
		.nxpi_flags = NXPIF_VIRTUAL_DEVICE,
		.nxpi_pre_connect = btsk_pre_connect,
		.nxpi_connected = btsk_connected,
		.nxpi_pre_disconnect = btsk_pre_disconnect,
		.nxpi_disconnected = btsk_disconnected,
		.nxpi_ring_init = btsk_ring_init,
		.nxpi_ring_fini = btsk_ring_fini,
		.nxpi_slot_init = btsk_slot_init,
		.nxpi_slot_fini = btsk_slot_fini,
		.nxpi_sync_tx = btsk_sync_tx,
		.nxpi_sync_rx = btsk_sync_rx,
		.nxpi_tx_doorbell = NULL,
	};
	struct kern_nexus_init nx_init = {
		.nxi_version = KERN_NEXUS_CURRENT_VERSION,
		.nxi_flags = 0,
		.nxi_tx_pbufpool = NULL,
		.nxi_rx_pbufpool = NULL,
	};

	read_random(&btsk_nx_ctx, sizeof(btsk_nx_ctx));
	error = kern_nexus_register_domain_provider(NEXUS_TYPE_KERNEL_PIPE, (const uint8_t *)"munch_bt", &dom_init, sizeof(dom_init),
	    &btsk_dom_uuid);
	if (error != 0) {
		printf("munch-bt: Skywalk domain provider failed (%d)\n", error);
		return false;
	}
	if ((error = kern_nexus_controller_create(&btsk_ncd)) != 0 ||
	    (error = kern_nexus_attr_create(&nxa)) != 0) {
		goto fail;
	}
	(void)kern_nexus_attr_set(nxa, NEXUS_ATTR_ANONYMOUS, 1);
	(void)kern_nexus_attr_set(nxa, NEXUS_ATTR_TX_RINGS, 1);
	(void)kern_nexus_attr_set(nxa, NEXUS_ATTR_RX_RINGS, 1);
	(void)kern_nexus_attr_set(nxa, NEXUS_ATTR_TX_SLOTS, 64);
	(void)kern_nexus_attr_set(nxa, NEXUS_ATTR_RX_SLOTS, 64);
	(void)kern_nexus_attr_set(nxa, NEXUS_ATTR_SLOT_BUF_SIZE, 2048);
	error = kern_nexus_controller_register_provider(btsk_ncd, btsk_dom_uuid,
	    (const uint8_t *)"com.apple.nexus.munch_bt", &prov_init, sizeof(prov_init), nxa, &btsk_prov_uuid);
	kern_nexus_attr_destroy(nxa);
	if (error != 0) {
		goto fail;
	}
	error = kern_nexus_controller_alloc_provider_instance(btsk_ncd, btsk_prov_uuid,
	    (const void *)btsk_nx_ctx, NULL, &btsk_nx_uuid, &nx_init);
	if (error != 0) {
		goto fail;
	}
	__atomic_store_n(&btsk_ready, true, __ATOMIC_RELEASE);
	return true;
fail:
	printf("munch-bt: Skywalk setup failed (%d)\n", error);
	munch_btsk_close();
	return false;
}

void
munch_btsk_close(void)
{
	__atomic_store_n(&btsk_ready, false, __ATOMIC_RELEASE);
	if (btsk_ncd != NULL) {
		kern_nexus_controller_destroy(btsk_ncd);
		btsk_ncd = NULL;
	}
	(void)kern_nexus_deregister_domain_provider(btsk_dom_uuid);
}
