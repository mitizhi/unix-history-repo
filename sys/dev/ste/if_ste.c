/*-
 * Copyright (c) 1997, 1998, 1999
 *	Bill Paul <wpaul@ctr.columbia.edu>.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by Bill Paul.
 * 4. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY Bill Paul AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL Bill Paul OR THE VOICES IN HIS HEAD
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#ifdef HAVE_KERNEL_OPTION_HEADERS
#include "opt_device_polling.h"
#endif

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/module.h>
#include <sys/rman.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>

#include <net/bpf.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <net/ethernet.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net/if_vlan_var.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <dev/mii/mii.h>
#include <dev/mii/miivar.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include <dev/ste/if_stereg.h>

/* "device miibus" required.  See GENERIC if you get errors here. */
#include "miibus_if.h"

#define STE_USEIOSPACE

MODULE_DEPEND(ste, pci, 1, 1, 1);
MODULE_DEPEND(ste, ether, 1, 1, 1);
MODULE_DEPEND(ste, miibus, 1, 1, 1);

/*
 * Various supported device vendors/types and their names.
 */
static struct ste_type ste_devs[] = {
	{ ST_VENDORID, ST_DEVICEID_ST201_1, "Sundance ST201 10/100BaseTX" },
	{ ST_VENDORID, ST_DEVICEID_ST201_2, "Sundance ST201 10/100BaseTX" },
	{ DL_VENDORID, DL_DEVICEID_DL10050, "D-Link DL10050 10/100BaseTX" },
	{ 0, 0, NULL }
};

static int	ste_attach(device_t);
static int	ste_detach(device_t);
static int	ste_probe(device_t);
static int	ste_shutdown(device_t);

static int	ste_dma_alloc(struct ste_softc *);
static void	ste_dma_free(struct ste_softc *);
static void	ste_dmamap_cb(void *, bus_dma_segment_t *, int, int);
static int 	ste_eeprom_wait(struct ste_softc *);
static int	ste_encap(struct ste_softc *, struct mbuf **,
		    struct ste_chain *);
static int	ste_ifmedia_upd(struct ifnet *);
static void	ste_ifmedia_upd_locked(struct ifnet *);
static void	ste_ifmedia_sts(struct ifnet *, struct ifmediareq *);
static void	ste_init(void *);
static void	ste_init_locked(struct ste_softc *);
static int	ste_init_rx_list(struct ste_softc *);
static void	ste_init_tx_list(struct ste_softc *);
static void	ste_intr(void *);
static int	ste_ioctl(struct ifnet *, u_long, caddr_t);
static int	ste_mii_readreg(struct ste_softc *, struct ste_mii_frame *);
static void	ste_mii_send(struct ste_softc *, uint32_t, int);
static void	ste_mii_sync(struct ste_softc *);
static int	ste_mii_writereg(struct ste_softc *, struct ste_mii_frame *);
static int	ste_miibus_readreg(device_t, int, int);
static void	ste_miibus_statchg(device_t);
static int	ste_miibus_writereg(device_t, int, int, int);
static int	ste_newbuf(struct ste_softc *, struct ste_chain_onefrag *);
static int	ste_read_eeprom(struct ste_softc *, caddr_t, int, int, int);
static void	ste_reset(struct ste_softc *);
static int	ste_rxeof(struct ste_softc *, int);
static void	ste_setmulti(struct ste_softc *);
static void	ste_start(struct ifnet *);
static void	ste_start_locked(struct ifnet *);
static void	ste_stats_update(struct ste_softc *);
static void	ste_stop(struct ste_softc *);
static void	ste_tick(void *);
static void	ste_txeoc(struct ste_softc *);
static void	ste_txeof(struct ste_softc *);
static void	ste_wait(struct ste_softc *);
static void	ste_watchdog(struct ste_softc *);

#ifdef STE_USEIOSPACE
#define STE_RES			SYS_RES_IOPORT
#define STE_RID			STE_PCI_LOIO
#else
#define STE_RES			SYS_RES_MEMORY
#define STE_RID			STE_PCI_LOMEM
#endif

static device_method_t ste_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		ste_probe),
	DEVMETHOD(device_attach,	ste_attach),
	DEVMETHOD(device_detach,	ste_detach),
	DEVMETHOD(device_shutdown,	ste_shutdown),

	/* bus interface */
	DEVMETHOD(bus_print_child,	bus_generic_print_child),
	DEVMETHOD(bus_driver_added,	bus_generic_driver_added),

	/* MII interface */
	DEVMETHOD(miibus_readreg,	ste_miibus_readreg),
	DEVMETHOD(miibus_writereg,	ste_miibus_writereg),
	DEVMETHOD(miibus_statchg,	ste_miibus_statchg),

	{ 0, 0 }
};

static driver_t ste_driver = {
	"ste",
	ste_methods,
	sizeof(struct ste_softc)
};

static devclass_t ste_devclass;

DRIVER_MODULE(ste, pci, ste_driver, ste_devclass, 0, 0);
DRIVER_MODULE(miibus, ste, miibus_driver, miibus_devclass, 0, 0);

#define STE_SETBIT4(sc, reg, x)				\
	CSR_WRITE_4(sc, reg, CSR_READ_4(sc, reg) | (x))

#define STE_CLRBIT4(sc, reg, x)				\
	CSR_WRITE_4(sc, reg, CSR_READ_4(sc, reg) & ~(x))

#define STE_SETBIT2(sc, reg, x)				\
	CSR_WRITE_2(sc, reg, CSR_READ_2(sc, reg) | (x))

#define STE_CLRBIT2(sc, reg, x)				\
	CSR_WRITE_2(sc, reg, CSR_READ_2(sc, reg) & ~(x))

#define STE_SETBIT1(sc, reg, x)				\
	CSR_WRITE_1(sc, reg, CSR_READ_1(sc, reg) | (x))

#define STE_CLRBIT1(sc, reg, x)				\
	CSR_WRITE_1(sc, reg, CSR_READ_1(sc, reg) & ~(x))


#define MII_SET(x)		STE_SETBIT1(sc, STE_PHYCTL, x)
#define MII_CLR(x)		STE_CLRBIT1(sc, STE_PHYCTL, x)

/*
 * Sync the PHYs by setting data bit and strobing the clock 32 times.
 */
static void
ste_mii_sync(struct ste_softc *sc)
{
	int i;

	MII_SET(STE_PHYCTL_MDIR|STE_PHYCTL_MDATA);

	for (i = 0; i < 32; i++) {
		MII_SET(STE_PHYCTL_MCLK);
		DELAY(1);
		MII_CLR(STE_PHYCTL_MCLK);
		DELAY(1);
	}
}

/*
 * Clock a series of bits through the MII.
 */
static void
ste_mii_send(struct ste_softc *sc, uint32_t bits, int cnt)
{
	int i;

	MII_CLR(STE_PHYCTL_MCLK);

	for (i = (0x1 << (cnt - 1)); i; i >>= 1) {
		if (bits & i) {
			MII_SET(STE_PHYCTL_MDATA);
                } else {
			MII_CLR(STE_PHYCTL_MDATA);
                }
		DELAY(1);
		MII_CLR(STE_PHYCTL_MCLK);
		DELAY(1);
		MII_SET(STE_PHYCTL_MCLK);
	}
}

/*
 * Read an PHY register through the MII.
 */
static int
ste_mii_readreg(struct ste_softc *sc, struct ste_mii_frame *frame)
{
	int i, ack;

	/*
	 * Set up frame for RX.
	 */
	frame->mii_stdelim = STE_MII_STARTDELIM;
	frame->mii_opcode = STE_MII_READOP;
	frame->mii_turnaround = 0;
	frame->mii_data = 0;

	CSR_WRITE_2(sc, STE_PHYCTL, 0);
	/*
 	 * Turn on data xmit.
	 */
	MII_SET(STE_PHYCTL_MDIR);

	ste_mii_sync(sc);

	/*
	 * Send command/address info.
	 */
	ste_mii_send(sc, frame->mii_stdelim, 2);
	ste_mii_send(sc, frame->mii_opcode, 2);
	ste_mii_send(sc, frame->mii_phyaddr, 5);
	ste_mii_send(sc, frame->mii_regaddr, 5);

	/* Turn off xmit. */
	MII_CLR(STE_PHYCTL_MDIR);

	/* Idle bit */
	MII_CLR((STE_PHYCTL_MCLK|STE_PHYCTL_MDATA));
	DELAY(1);
	MII_SET(STE_PHYCTL_MCLK);
	DELAY(1);

	/* Check for ack */
	MII_CLR(STE_PHYCTL_MCLK);
	DELAY(1);
	ack = CSR_READ_2(sc, STE_PHYCTL) & STE_PHYCTL_MDATA;
	MII_SET(STE_PHYCTL_MCLK);
	DELAY(1);

	/*
	 * Now try reading data bits. If the ack failed, we still
	 * need to clock through 16 cycles to keep the PHY(s) in sync.
	 */
	if (ack) {
		for (i = 0; i < 16; i++) {
			MII_CLR(STE_PHYCTL_MCLK);
			DELAY(1);
			MII_SET(STE_PHYCTL_MCLK);
			DELAY(1);
		}
		goto fail;
	}

	for (i = 0x8000; i; i >>= 1) {
		MII_CLR(STE_PHYCTL_MCLK);
		DELAY(1);
		if (!ack) {
			if (CSR_READ_2(sc, STE_PHYCTL) & STE_PHYCTL_MDATA)
				frame->mii_data |= i;
			DELAY(1);
		}
		MII_SET(STE_PHYCTL_MCLK);
		DELAY(1);
	}

fail:

	MII_CLR(STE_PHYCTL_MCLK);
	DELAY(1);
	MII_SET(STE_PHYCTL_MCLK);
	DELAY(1);

	if (ack)
		return (1);
	return (0);
}

/*
 * Write to a PHY register through the MII.
 */
static int
ste_mii_writereg(struct ste_softc *sc, struct ste_mii_frame *frame)
{

	/*
	 * Set up frame for TX.
	 */

	frame->mii_stdelim = STE_MII_STARTDELIM;
	frame->mii_opcode = STE_MII_WRITEOP;
	frame->mii_turnaround = STE_MII_TURNAROUND;

	/*
 	 * Turn on data output.
	 */
	MII_SET(STE_PHYCTL_MDIR);

	ste_mii_sync(sc);

	ste_mii_send(sc, frame->mii_stdelim, 2);
	ste_mii_send(sc, frame->mii_opcode, 2);
	ste_mii_send(sc, frame->mii_phyaddr, 5);
	ste_mii_send(sc, frame->mii_regaddr, 5);
	ste_mii_send(sc, frame->mii_turnaround, 2);
	ste_mii_send(sc, frame->mii_data, 16);

	/* Idle bit. */
	MII_SET(STE_PHYCTL_MCLK);
	DELAY(1);
	MII_CLR(STE_PHYCTL_MCLK);
	DELAY(1);

	/*
	 * Turn off xmit.
	 */
	MII_CLR(STE_PHYCTL_MDIR);

	return (0);
}

static int
ste_miibus_readreg(device_t dev, int phy, int reg)
{
	struct ste_softc *sc;
	struct ste_mii_frame frame;

	sc = device_get_softc(dev);

	if ((sc->ste_flags & STE_FLAG_ONE_PHY) != 0 && phy != 0)
		return (0);

	bzero((char *)&frame, sizeof(frame));

	frame.mii_phyaddr = phy;
	frame.mii_regaddr = reg;
	ste_mii_readreg(sc, &frame);

	return (frame.mii_data);
}

static int
ste_miibus_writereg(device_t dev, int phy, int reg, int data)
{
	struct ste_softc *sc;
	struct ste_mii_frame frame;

	sc = device_get_softc(dev);
	bzero((char *)&frame, sizeof(frame));

	frame.mii_phyaddr = phy;
	frame.mii_regaddr = reg;
	frame.mii_data = data;

	ste_mii_writereg(sc, &frame);

	return (0);
}

static void
ste_miibus_statchg(device_t dev)
{
	struct ste_softc *sc;
	struct mii_data *mii;
	struct ifnet *ifp;
	uint16_t cfg;

	sc = device_get_softc(dev);

	mii = device_get_softc(sc->ste_miibus);
	ifp = sc->ste_ifp;
	if (mii == NULL || ifp == NULL ||
	    (ifp->if_drv_flags & IFF_DRV_RUNNING) == 0)
		return;

	sc->ste_flags &= ~STE_FLAG_LINK;
	if ((mii->mii_media_status & (IFM_ACTIVE | IFM_AVALID)) ==
	    (IFM_ACTIVE | IFM_AVALID)) {
		switch (IFM_SUBTYPE(mii->mii_media_active)) {
		case IFM_10_T:
		case IFM_100_TX:
		case IFM_100_FX:
		case IFM_100_T4:
			sc->ste_flags |= STE_FLAG_LINK;
		default:
			break;
		}
	}

	/* Program MACs with resolved speed/duplex/flow-control. */
	if ((sc->ste_flags & STE_FLAG_LINK) != 0) {
		cfg = CSR_READ_2(sc, STE_MACCTL0);
		cfg &= ~(STE_MACCTL0_FLOWCTL_ENABLE | STE_MACCTL0_FULLDUPLEX);
		if ((IFM_OPTIONS(mii->mii_media_active) & IFM_FDX) != 0) {
			/*
			 * ST201 data sheet says driver should enable receiving
			 * MAC control frames bit of receive mode register to
			 * receive flow-control frames but the register has no
			 * such bits. In addition the controller has no ability
			 * to send pause frames so it should be handled in
			 * driver. Implementing pause timer handling in driver
			 * layer is not trivial, so don't enable flow-control
			 * here.
			 */
			cfg |= STE_MACCTL0_FULLDUPLEX;
		}
		CSR_WRITE_2(sc, STE_MACCTL0, cfg);
	}
}

static int
ste_ifmedia_upd(struct ifnet *ifp)
{
	struct ste_softc *sc;

	sc = ifp->if_softc;
	STE_LOCK(sc);
	ste_ifmedia_upd_locked(ifp);
	STE_UNLOCK(sc);

	return (0);
}

static void
ste_ifmedia_upd_locked(struct ifnet *ifp)
{
	struct ste_softc *sc;
	struct mii_data *mii;

	sc = ifp->if_softc;
	STE_LOCK_ASSERT(sc);
	mii = device_get_softc(sc->ste_miibus);
	sc->ste_flags &= ~STE_FLAG_LINK;
	if (mii->mii_instance) {
		struct mii_softc	*miisc;
		LIST_FOREACH(miisc, &mii->mii_phys, mii_list)
			mii_phy_reset(miisc);
	}
	mii_mediachg(mii);
}

static void
ste_ifmedia_sts(struct ifnet *ifp, struct ifmediareq *ifmr)
{
	struct ste_softc *sc;
	struct mii_data *mii;

	sc = ifp->if_softc;
	mii = device_get_softc(sc->ste_miibus);

	STE_LOCK(sc);
	mii_pollstat(mii);
	ifmr->ifm_active = mii->mii_media_active;
	ifmr->ifm_status = mii->mii_media_status;
	STE_UNLOCK(sc);
}

static void
ste_wait(struct ste_softc *sc)
{
	int i;

	for (i = 0; i < STE_TIMEOUT; i++) {
		if (!(CSR_READ_4(sc, STE_DMACTL) & STE_DMACTL_DMA_HALTINPROG))
			break;
		DELAY(1);
	}

	if (i == STE_TIMEOUT)
		device_printf(sc->ste_dev, "command never completed!\n");
}

/*
 * The EEPROM is slow: give it time to come ready after issuing
 * it a command.
 */
static int
ste_eeprom_wait(struct ste_softc *sc)
{
	int i;

	DELAY(1000);

	for (i = 0; i < 100; i++) {
		if (CSR_READ_2(sc, STE_EEPROM_CTL) & STE_EECTL_BUSY)
			DELAY(1000);
		else
			break;
	}

	if (i == 100) {
		device_printf(sc->ste_dev, "eeprom failed to come ready\n");
		return (1);
	}

	return (0);
}

/*
 * Read a sequence of words from the EEPROM. Note that ethernet address
 * data is stored in the EEPROM in network byte order.
 */
static int
ste_read_eeprom(struct ste_softc *sc, caddr_t dest, int off, int cnt, int swap)
{
	uint16_t word, *ptr;
	int err = 0, i;

	if (ste_eeprom_wait(sc))
		return (1);

	for (i = 0; i < cnt; i++) {
		CSR_WRITE_2(sc, STE_EEPROM_CTL, STE_EEOPCODE_READ | (off + i));
		err = ste_eeprom_wait(sc);
		if (err)
			break;
		word = CSR_READ_2(sc, STE_EEPROM_DATA);
		ptr = (uint16_t *)(dest + (i * 2));
		if (swap)
			*ptr = ntohs(word);
		else
			*ptr = word;
	}

	return (err ? 1 : 0);
}

static void
ste_setmulti(struct ste_softc *sc)
{
	struct ifnet *ifp;
	struct ifmultiaddr *ifma;
	uint32_t hashes[2] = { 0, 0 };
	int h;

	ifp = sc->ste_ifp;
	if (ifp->if_flags & IFF_ALLMULTI || ifp->if_flags & IFF_PROMISC) {
		STE_SETBIT1(sc, STE_RX_MODE, STE_RXMODE_ALLMULTI);
		STE_CLRBIT1(sc, STE_RX_MODE, STE_RXMODE_MULTIHASH);
		return;
	}

	/* first, zot all the existing hash bits */
	CSR_WRITE_2(sc, STE_MAR0, 0);
	CSR_WRITE_2(sc, STE_MAR1, 0);
	CSR_WRITE_2(sc, STE_MAR2, 0);
	CSR_WRITE_2(sc, STE_MAR3, 0);

	/* now program new ones */
	if_maddr_rlock(ifp);
	TAILQ_FOREACH(ifma, &ifp->if_multiaddrs, ifma_link) {
		if (ifma->ifma_addr->sa_family != AF_LINK)
			continue;
		h = ether_crc32_be(LLADDR((struct sockaddr_dl *)
		    ifma->ifma_addr), ETHER_ADDR_LEN) & 0x3F;
		if (h < 32)
			hashes[0] |= (1 << h);
		else
			hashes[1] |= (1 << (h - 32));
	}
	if_maddr_runlock(ifp);

	CSR_WRITE_2(sc, STE_MAR0, hashes[0] & 0xFFFF);
	CSR_WRITE_2(sc, STE_MAR1, (hashes[0] >> 16) & 0xFFFF);
	CSR_WRITE_2(sc, STE_MAR2, hashes[1] & 0xFFFF);
	CSR_WRITE_2(sc, STE_MAR3, (hashes[1] >> 16) & 0xFFFF);
	STE_CLRBIT1(sc, STE_RX_MODE, STE_RXMODE_ALLMULTI);
	STE_SETBIT1(sc, STE_RX_MODE, STE_RXMODE_MULTIHASH);
}

#ifdef DEVICE_POLLING
static poll_handler_t ste_poll, ste_poll_locked;

static int
ste_poll(struct ifnet *ifp, enum poll_cmd cmd, int count)
{
	struct ste_softc *sc = ifp->if_softc;
	int rx_npkts = 0;

	STE_LOCK(sc);
	if (ifp->if_drv_flags & IFF_DRV_RUNNING)
		rx_npkts = ste_poll_locked(ifp, cmd, count);
	STE_UNLOCK(sc);
	return (rx_npkts);
}

static int
ste_poll_locked(struct ifnet *ifp, enum poll_cmd cmd, int count)
{
	struct ste_softc *sc = ifp->if_softc;
	int rx_npkts;

	STE_LOCK_ASSERT(sc);

	rx_npkts = ste_rxeof(sc, count);
	ste_txeof(sc);
	if (!IFQ_DRV_IS_EMPTY(&ifp->if_snd))
		ste_start_locked(ifp);

	if (cmd == POLL_AND_CHECK_STATUS) {
		uint16_t status;

		status = CSR_READ_2(sc, STE_ISR_ACK);

		if (status & STE_ISR_TX_DONE)
			ste_txeoc(sc);

		if (status & STE_ISR_STATS_OFLOW)
			ste_stats_update(sc);

		if (status & STE_ISR_HOSTERR)
			ste_init_locked(sc);
	}
	return (rx_npkts);
}
#endif /* DEVICE_POLLING */

static void
ste_intr(void *xsc)
{
	struct ste_softc *sc;
	struct ifnet *ifp;
	uint16_t status;

	sc = xsc;
	STE_LOCK(sc);
	ifp = sc->ste_ifp;

#ifdef DEVICE_POLLING
	if (ifp->if_capenable & IFCAP_POLLING) {
		STE_UNLOCK(sc);
		return;
	}
#endif

	/* See if this is really our interrupt. */
	if (!(CSR_READ_2(sc, STE_ISR) & STE_ISR_INTLATCH)) {
		STE_UNLOCK(sc);
		return;
	}

	for (;;) {
		status = CSR_READ_2(sc, STE_ISR_ACK);

		if (!(status & STE_INTRS))
			break;

		if (status & STE_ISR_RX_DMADONE)
			ste_rxeof(sc, -1);

		if (status & STE_ISR_TX_DMADONE)
			ste_txeof(sc);

		if (status & STE_ISR_TX_DONE)
			ste_txeoc(sc);

		if (status & STE_ISR_STATS_OFLOW)
			ste_stats_update(sc);

		if (status & STE_ISR_HOSTERR)
			ste_init_locked(sc);
	}

	/* Re-enable interrupts */
	CSR_WRITE_2(sc, STE_IMR, STE_INTRS);

	if (!IFQ_DRV_IS_EMPTY(&ifp->if_snd))
		ste_start_locked(ifp);

	STE_UNLOCK(sc);
}

/*
 * A frame has been uploaded: pass the resulting mbuf chain up to
 * the higher level protocols.
 */
static int
ste_rxeof(struct ste_softc *sc, int count)
{
        struct mbuf *m;
        struct ifnet *ifp;
	struct ste_chain_onefrag *cur_rx;
	uint32_t rxstat;
	int total_len, rx_npkts;

	ifp = sc->ste_ifp;

	bus_dmamap_sync(sc->ste_cdata.ste_rx_list_tag,
	    sc->ste_cdata.ste_rx_list_map,
	    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);

	cur_rx = sc->ste_cdata.ste_rx_head;
	for (rx_npkts = 0; rx_npkts < STE_RX_LIST_CNT; rx_npkts++,
	    cur_rx = cur_rx->ste_next) {
		rxstat = le32toh(cur_rx->ste_ptr->ste_status);
		if ((rxstat & STE_RXSTAT_DMADONE) == 0)
			break;
#ifdef DEVICE_POLLING
		if (ifp->if_capenable & IFCAP_POLLING) {
			if (count == 0)
				break;
			count--;
		}
#endif
		if ((ifp->if_drv_flags & IFF_DRV_RUNNING) == 0)
			break;
		/*
		 * If an error occurs, update stats, clear the
		 * status word and leave the mbuf cluster in place:
		 * it should simply get re-used next time this descriptor
	 	 * comes up in the ring.
		 */
		if (rxstat & STE_RXSTAT_FRAME_ERR) {
			ifp->if_ierrors++;
			cur_rx->ste_ptr->ste_status = 0;
			continue;
		}

		/* No errors; receive the packet. */
		m = cur_rx->ste_mbuf;
		total_len = STE_RX_BYTES(rxstat);

		/*
		 * Try to conjure up a new mbuf cluster. If that
		 * fails, it means we have an out of memory condition and
		 * should leave the buffer in place and continue. This will
		 * result in a lost packet, but there's little else we
		 * can do in this situation.
		 */
		if (ste_newbuf(sc, cur_rx) != 0) {
			ifp->if_ierrors++;
			cur_rx->ste_ptr->ste_status = 0;
			continue;
		}

		m->m_pkthdr.rcvif = ifp;
		m->m_pkthdr.len = m->m_len = total_len;

		ifp->if_ipackets++;
		STE_UNLOCK(sc);
		(*ifp->if_input)(ifp, m);
		STE_LOCK(sc);
	}

	if (rx_npkts > 0) {
		sc->ste_cdata.ste_rx_head = cur_rx;
		bus_dmamap_sync(sc->ste_cdata.ste_rx_list_tag,
		    sc->ste_cdata.ste_rx_list_map,
		    BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);
	}

	return (rx_npkts);
}

static void
ste_txeoc(struct ste_softc *sc)
{
	struct ifnet *ifp;
	uint8_t txstat;

	ifp = sc->ste_ifp;

	while ((txstat = CSR_READ_1(sc, STE_TX_STATUS)) &
	    STE_TXSTATUS_TXDONE) {
		if (txstat & STE_TXSTATUS_UNDERRUN ||
		    txstat & STE_TXSTATUS_EXCESSCOLLS ||
		    txstat & STE_TXSTATUS_RECLAIMERR) {
			ifp->if_oerrors++;
			device_printf(sc->ste_dev,
			    "transmission error: %x\n", txstat);

			ste_init_locked(sc);

			if (txstat & STE_TXSTATUS_UNDERRUN &&
			    sc->ste_tx_thresh < STE_PACKET_SIZE) {
				sc->ste_tx_thresh += STE_MIN_FRAMELEN;
				device_printf(sc->ste_dev,
				    "tx underrun, increasing tx"
				    " start threshold to %d bytes\n",
				    sc->ste_tx_thresh);
			}
			CSR_WRITE_2(sc, STE_TX_STARTTHRESH, sc->ste_tx_thresh);
			CSR_WRITE_2(sc, STE_TX_RECLAIM_THRESH,
			    (STE_PACKET_SIZE >> 4));
		}
		ste_init_locked(sc);
		CSR_WRITE_2(sc, STE_TX_STATUS, txstat);
	}
}

static void
ste_tick(void *arg)
{
	struct ste_softc *sc;
	struct mii_data *mii;

	sc = (struct ste_softc *)arg;

	STE_LOCK_ASSERT(sc);

	mii = device_get_softc(sc->ste_miibus);
	mii_tick(mii);
	/*
	 * ukphy(4) does not seem to generate CB that reports
	 * resolved link state so if we know we lost a link,
	 * explicitly check the link state.
	 */
	if ((sc->ste_flags & STE_FLAG_LINK) == 0)
		ste_miibus_statchg(sc->ste_dev);
	ste_stats_update(sc);
	ste_watchdog(sc);
	callout_reset(&sc->ste_callout, hz, ste_tick, sc);
}

static void
ste_txeof(struct ste_softc *sc)
{
	struct ifnet *ifp;
	struct ste_chain *cur_tx;
	uint32_t txstat;
	int idx;

	STE_LOCK_ASSERT(sc);

	ifp = sc->ste_ifp;
	idx = sc->ste_cdata.ste_tx_cons;
	if (idx == sc->ste_cdata.ste_tx_prod)
		return;

	bus_dmamap_sync(sc->ste_cdata.ste_tx_list_tag,
	    sc->ste_cdata.ste_tx_list_map,
	    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);

	while (idx != sc->ste_cdata.ste_tx_prod) {
		cur_tx = &sc->ste_cdata.ste_tx_chain[idx];
		txstat = le32toh(cur_tx->ste_ptr->ste_ctl);
		if ((txstat & STE_TXCTL_DMADONE) == 0)
			break;
		bus_dmamap_sync(sc->ste_cdata.ste_tx_tag, cur_tx->ste_map,
		    BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(sc->ste_cdata.ste_tx_tag, cur_tx->ste_map);
		KASSERT(cur_tx->ste_mbuf != NULL,
		    ("%s: freeing NULL mbuf!\n", __func__));
		m_freem(cur_tx->ste_mbuf);
		cur_tx->ste_mbuf = NULL;
		ifp->if_drv_flags &= ~IFF_DRV_OACTIVE;
		ifp->if_opackets++;
		sc->ste_cdata.ste_tx_cnt--;
		STE_INC(idx, STE_TX_LIST_CNT);
	}

	sc->ste_cdata.ste_tx_cons = idx;
	if (sc->ste_cdata.ste_tx_cnt == 0)
		sc->ste_timer = 0;
}

static void
ste_stats_update(struct ste_softc *sc)
{
	struct ifnet *ifp;

	STE_LOCK_ASSERT(sc);

	ifp = sc->ste_ifp;
	ifp->if_collisions += CSR_READ_1(sc, STE_LATE_COLLS)
	    + CSR_READ_1(sc, STE_MULTI_COLLS)
	    + CSR_READ_1(sc, STE_SINGLE_COLLS);
}

/*
 * Probe for a Sundance ST201 chip. Check the PCI vendor and device
 * IDs against our list and return a device name if we find a match.
 */
static int
ste_probe(device_t dev)
{
	struct ste_type *t;

	t = ste_devs;

	while (t->ste_name != NULL) {
		if ((pci_get_vendor(dev) == t->ste_vid) &&
		    (pci_get_device(dev) == t->ste_did)) {
			device_set_desc(dev, t->ste_name);
			return (BUS_PROBE_DEFAULT);
		}
		t++;
	}

	return (ENXIO);
}

/*
 * Attach the interface. Allocate softc structures, do ifmedia
 * setup and ethernet/BPF attach.
 */
static int
ste_attach(device_t dev)
{
	struct ste_softc *sc;
	struct ifnet *ifp;
	u_char eaddr[6];
	int error = 0, rid;

	sc = device_get_softc(dev);
	sc->ste_dev = dev;

	/*
	 * Only use one PHY since this chip reports multiple
	 * Note on the DFE-550 the PHY is at 1 on the DFE-580
	 * it is at 0 & 1.  It is rev 0x12.
	 */
	if (pci_get_vendor(dev) == DL_VENDORID &&
	    pci_get_device(dev) == DL_DEVICEID_DL10050 &&
	    pci_get_revid(dev) == 0x12 )
		sc->ste_flags |= STE_FLAG_ONE_PHY;

	mtx_init(&sc->ste_mtx, device_get_nameunit(dev), MTX_NETWORK_LOCK,
	    MTX_DEF);
	/*
	 * Map control/status registers.
	 */
	pci_enable_busmaster(dev);

	rid = STE_RID;
	sc->ste_res = bus_alloc_resource_any(dev, STE_RES, &rid, RF_ACTIVE);

	if (sc->ste_res == NULL) {
		device_printf(dev, "couldn't map ports/memory\n");
		error = ENXIO;
		goto fail;
	}

	sc->ste_btag = rman_get_bustag(sc->ste_res);
	sc->ste_bhandle = rman_get_bushandle(sc->ste_res);

	/* Allocate interrupt */
	rid = 0;
	sc->ste_irq = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid,
	    RF_SHAREABLE | RF_ACTIVE);

	if (sc->ste_irq == NULL) {
		device_printf(dev, "couldn't map interrupt\n");
		error = ENXIO;
		goto fail;
	}

	callout_init_mtx(&sc->ste_callout, &sc->ste_mtx, 0);

	/* Reset the adapter. */
	ste_reset(sc);

	/*
	 * Get station address from the EEPROM.
	 */
	if (ste_read_eeprom(sc, eaddr,
	    STE_EEADDR_NODE0, 3, 0)) {
		device_printf(dev, "failed to read station address\n");
		error = ENXIO;;
		goto fail;
	}

	if ((error = ste_dma_alloc(sc)) != 0)
		goto fail;

	ifp = sc->ste_ifp = if_alloc(IFT_ETHER);
	if (ifp == NULL) {
		device_printf(dev, "can not if_alloc()\n");
		error = ENOSPC;
		goto fail;
	}

	/* Do MII setup. */
	if (mii_phy_probe(dev, &sc->ste_miibus,
	    ste_ifmedia_upd, ste_ifmedia_sts)) {
		device_printf(dev, "MII without any phy!\n");
		error = ENXIO;
		goto fail;
	}

	ifp->if_softc = sc;
	if_initname(ifp, device_get_name(dev), device_get_unit(dev));
	ifp->if_mtu = ETHERMTU;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_ioctl = ste_ioctl;
	ifp->if_start = ste_start;
	ifp->if_init = ste_init;
	IFQ_SET_MAXLEN(&ifp->if_snd, STE_TX_LIST_CNT - 1);
	ifp->if_snd.ifq_drv_maxlen = STE_TX_LIST_CNT - 1;
	IFQ_SET_READY(&ifp->if_snd);

	sc->ste_tx_thresh = STE_TXSTART_THRESH;

	/*
	 * Call MI attach routine.
	 */
	ether_ifattach(ifp, eaddr);

	/*
	 * Tell the upper layer(s) we support long frames.
	 */
	ifp->if_data.ifi_hdrlen = sizeof(struct ether_vlan_header);
	ifp->if_capabilities |= IFCAP_VLAN_MTU;
	ifp->if_capenable = ifp->if_capabilities;
#ifdef DEVICE_POLLING
	ifp->if_capabilities |= IFCAP_POLLING;
#endif

	/* Hook interrupt last to avoid having to lock softc */
	error = bus_setup_intr(dev, sc->ste_irq, INTR_TYPE_NET | INTR_MPSAFE,
	    NULL, ste_intr, sc, &sc->ste_intrhand);

	if (error) {
		device_printf(dev, "couldn't set up irq\n");
		ether_ifdetach(ifp);
		goto fail;
	}

fail:
	if (error)
		ste_detach(dev);

	return (error);
}

/*
 * Shutdown hardware and free up resources. This can be called any
 * time after the mutex has been initialized. It is called in both
 * the error case in attach and the normal detach case so it needs
 * to be careful about only freeing resources that have actually been
 * allocated.
 */
static int
ste_detach(device_t dev)
{
	struct ste_softc *sc;
	struct ifnet *ifp;

	sc = device_get_softc(dev);
	KASSERT(mtx_initialized(&sc->ste_mtx), ("ste mutex not initialized"));
	ifp = sc->ste_ifp;

#ifdef DEVICE_POLLING
	if (ifp->if_capenable & IFCAP_POLLING)
		ether_poll_deregister(ifp);
#endif

	/* These should only be active if attach succeeded */
	if (device_is_attached(dev)) {
		ether_ifdetach(ifp);
		STE_LOCK(sc);
		ste_stop(sc);
		STE_UNLOCK(sc);
		callout_drain(&sc->ste_callout);
	}
	if (sc->ste_miibus)
		device_delete_child(dev, sc->ste_miibus);
	bus_generic_detach(dev);

	if (sc->ste_intrhand)
		bus_teardown_intr(dev, sc->ste_irq, sc->ste_intrhand);
	if (sc->ste_irq)
		bus_release_resource(dev, SYS_RES_IRQ, 0, sc->ste_irq);
	if (sc->ste_res)
		bus_release_resource(dev, STE_RES, STE_RID, sc->ste_res);

	if (ifp)
		if_free(ifp);

	ste_dma_free(sc);
	mtx_destroy(&sc->ste_mtx);

	return (0);
}

struct ste_dmamap_arg {
	bus_addr_t	ste_busaddr;
};

static void
ste_dmamap_cb(void *arg, bus_dma_segment_t *segs, int nsegs, int error)
{
	struct ste_dmamap_arg *ctx;

	if (error != 0)
		return;

	KASSERT(nsegs == 1, ("%s: %d segments returned!", __func__, nsegs));

	ctx = (struct ste_dmamap_arg *)arg;
	ctx->ste_busaddr = segs[0].ds_addr;
}

static int
ste_dma_alloc(struct ste_softc *sc)
{
	struct ste_chain *txc;
	struct ste_chain_onefrag *rxc;
	struct ste_dmamap_arg ctx;
	int error, i;

	/* Create parent DMA tag. */
	error = bus_dma_tag_create(
	    bus_get_dma_tag(sc->ste_dev), /* parent */
	    1, 0,			/* alignment, boundary */
	    BUS_SPACE_MAXADDR_32BIT,	/* lowaddr */
	    BUS_SPACE_MAXADDR,		/* highaddr */
	    NULL, NULL,			/* filter, filterarg */
	    BUS_SPACE_MAXSIZE_32BIT,	/* maxsize */
	    0,				/* nsegments */
	    BUS_SPACE_MAXSIZE_32BIT,	/* maxsegsize */
	    0,				/* flags */
	    NULL, NULL,			/* lockfunc, lockarg */
	    &sc->ste_cdata.ste_parent_tag);
	if (error != 0) {
		device_printf(sc->ste_dev,
		    "could not create parent DMA tag.\n");
		goto fail;
	}

	/* Create DMA tag for Tx descriptor list. */
	error = bus_dma_tag_create(
	    sc->ste_cdata.ste_parent_tag, /* parent */
	    STE_DESC_ALIGN, 0,		/* alignment, boundary */
	    BUS_SPACE_MAXADDR,		/* lowaddr */
	    BUS_SPACE_MAXADDR,		/* highaddr */
	    NULL, NULL,			/* filter, filterarg */
	    STE_TX_LIST_SZ,		/* maxsize */
	    1,				/* nsegments */
	    STE_TX_LIST_SZ,		/* maxsegsize */
	    0,				/* flags */
	    NULL, NULL,			/* lockfunc, lockarg */
	    &sc->ste_cdata.ste_tx_list_tag);
	if (error != 0) {
		device_printf(sc->ste_dev,
		    "could not create Tx list DMA tag.\n");
		goto fail;
	}

	/* Create DMA tag for Rx descriptor list. */
	error = bus_dma_tag_create(
	    sc->ste_cdata.ste_parent_tag, /* parent */
	    STE_DESC_ALIGN, 0,		/* alignment, boundary */
	    BUS_SPACE_MAXADDR,		/* lowaddr */
	    BUS_SPACE_MAXADDR,		/* highaddr */
	    NULL, NULL,			/* filter, filterarg */
	    STE_RX_LIST_SZ,		/* maxsize */
	    1,				/* nsegments */
	    STE_RX_LIST_SZ,		/* maxsegsize */
	    0,				/* flags */
	    NULL, NULL,			/* lockfunc, lockarg */
	    &sc->ste_cdata.ste_rx_list_tag);
	if (error != 0) {
		device_printf(sc->ste_dev,
		    "could not create Rx list DMA tag.\n");
		goto fail;
	}

	/* Create DMA tag for Tx buffers. */
	error = bus_dma_tag_create(
	    sc->ste_cdata.ste_parent_tag, /* parent */
	    1, 0,			/* alignment, boundary */
	    BUS_SPACE_MAXADDR,		/* lowaddr */
	    BUS_SPACE_MAXADDR,		/* highaddr */
	    NULL, NULL,			/* filter, filterarg */
	    MCLBYTES * STE_MAXFRAGS,	/* maxsize */
	    STE_MAXFRAGS,		/* nsegments */
	    MCLBYTES,			/* maxsegsize */
	    0,				/* flags */
	    NULL, NULL,			/* lockfunc, lockarg */
	    &sc->ste_cdata.ste_tx_tag);
	if (error != 0) {
		device_printf(sc->ste_dev, "could not create Tx DMA tag.\n");
		goto fail;
	}

	/* Create DMA tag for Rx buffers. */
	error = bus_dma_tag_create(
	    sc->ste_cdata.ste_parent_tag, /* parent */
	    1, 0,			/* alignment, boundary */
	    BUS_SPACE_MAXADDR,		/* lowaddr */
	    BUS_SPACE_MAXADDR,		/* highaddr */
	    NULL, NULL,			/* filter, filterarg */
	    MCLBYTES,			/* maxsize */
	    1,				/* nsegments */
	    MCLBYTES,			/* maxsegsize */
	    0,				/* flags */
	    NULL, NULL,			/* lockfunc, lockarg */
	    &sc->ste_cdata.ste_rx_tag);
	if (error != 0) {
		device_printf(sc->ste_dev, "could not create Rx DMA tag.\n");
		goto fail;
	}

	/* Allocate DMA'able memory and load the DMA map for Tx list. */
	error = bus_dmamem_alloc(sc->ste_cdata.ste_tx_list_tag,
	    (void **)&sc->ste_ldata.ste_tx_list,
	    BUS_DMA_WAITOK | BUS_DMA_ZERO | BUS_DMA_COHERENT,
	    &sc->ste_cdata.ste_tx_list_map);
	if (error != 0) {
		device_printf(sc->ste_dev,
		    "could not allocate DMA'able memory for Tx list.\n");
		goto fail;
	}
	ctx.ste_busaddr = 0;
	error = bus_dmamap_load(sc->ste_cdata.ste_tx_list_tag,
	    sc->ste_cdata.ste_tx_list_map, sc->ste_ldata.ste_tx_list,
	    STE_TX_LIST_SZ, ste_dmamap_cb, &ctx, 0);
	if (error != 0 || ctx.ste_busaddr == 0) {
		device_printf(sc->ste_dev,
		    "could not load DMA'able memory for Tx list.\n");
		goto fail;
	}
	sc->ste_ldata.ste_tx_list_paddr = ctx.ste_busaddr;

	/* Allocate DMA'able memory and load the DMA map for Rx list. */
	error = bus_dmamem_alloc(sc->ste_cdata.ste_rx_list_tag,
	    (void **)&sc->ste_ldata.ste_rx_list,
	    BUS_DMA_WAITOK | BUS_DMA_ZERO | BUS_DMA_COHERENT,
	    &sc->ste_cdata.ste_rx_list_map);
	if (error != 0) {
		device_printf(sc->ste_dev,
		    "could not allocate DMA'able memory for Rx list.\n");
		goto fail;
	}
	ctx.ste_busaddr = 0;
	error = bus_dmamap_load(sc->ste_cdata.ste_rx_list_tag,
	    sc->ste_cdata.ste_rx_list_map, sc->ste_ldata.ste_rx_list,
	    STE_RX_LIST_SZ, ste_dmamap_cb, &ctx, 0);
	if (error != 0 || ctx.ste_busaddr == 0) {
		device_printf(sc->ste_dev,
		    "could not load DMA'able memory for Rx list.\n");
		goto fail;
	}
	sc->ste_ldata.ste_rx_list_paddr = ctx.ste_busaddr;

	/* Create DMA maps for Tx buffers. */
	for (i = 0; i < STE_TX_LIST_CNT; i++) {
		txc = &sc->ste_cdata.ste_tx_chain[i];
		txc->ste_ptr = NULL;
		txc->ste_mbuf = NULL;
		txc->ste_next = NULL;
		txc->ste_phys = 0;
		txc->ste_map = NULL;
		error = bus_dmamap_create(sc->ste_cdata.ste_tx_tag, 0,
		    &txc->ste_map);
		if (error != 0) {
			device_printf(sc->ste_dev,
			    "could not create Tx dmamap.\n");
			goto fail;
		}
	}
	/* Create DMA maps for Rx buffers. */
	if ((error = bus_dmamap_create(sc->ste_cdata.ste_rx_tag, 0,
	    &sc->ste_cdata.ste_rx_sparemap)) != 0) {
		device_printf(sc->ste_dev,
		    "could not create spare Rx dmamap.\n");
		goto fail;
	}
	for (i = 0; i < STE_RX_LIST_CNT; i++) {
		rxc = &sc->ste_cdata.ste_rx_chain[i];
		rxc->ste_ptr = NULL;
		rxc->ste_mbuf = NULL;
		rxc->ste_next = NULL;
		rxc->ste_map = NULL;
		error = bus_dmamap_create(sc->ste_cdata.ste_rx_tag, 0,
		    &rxc->ste_map);
		if (error != 0) {
			device_printf(sc->ste_dev,
			    "could not create Rx dmamap.\n");
			goto fail;
		}
	}

fail:
	return (error);
}

static void
ste_dma_free(struct ste_softc *sc)
{
	struct ste_chain *txc;
	struct ste_chain_onefrag *rxc;
	int i;

	/* Tx buffers. */
	if (sc->ste_cdata.ste_tx_tag != NULL) {
		for (i = 0; i < STE_TX_LIST_CNT; i++) {
			txc = &sc->ste_cdata.ste_tx_chain[i];
			if (txc->ste_map != NULL) {
				bus_dmamap_destroy(sc->ste_cdata.ste_tx_tag,
				    txc->ste_map);
				txc->ste_map = NULL;
			}
		}
		bus_dma_tag_destroy(sc->ste_cdata.ste_tx_tag);
		sc->ste_cdata.ste_tx_tag = NULL;
	}
	/* Rx buffers. */
	if (sc->ste_cdata.ste_rx_tag != NULL) {
		for (i = 0; i < STE_RX_LIST_CNT; i++) {
			rxc = &sc->ste_cdata.ste_rx_chain[i];
			if (rxc->ste_map != NULL) {
				bus_dmamap_destroy(sc->ste_cdata.ste_rx_tag,
				    rxc->ste_map);
				rxc->ste_map = NULL;
			}
		}
		if (sc->ste_cdata.ste_rx_sparemap != NULL) {
			bus_dmamap_destroy(sc->ste_cdata.ste_rx_tag,
			    sc->ste_cdata.ste_rx_sparemap);
			sc->ste_cdata.ste_rx_sparemap = NULL;
		}
		bus_dma_tag_destroy(sc->ste_cdata.ste_rx_tag);
		sc->ste_cdata.ste_rx_tag = NULL;
	}
	/* Tx descriptor list. */
	if (sc->ste_cdata.ste_tx_list_tag != NULL) {
		if (sc->ste_cdata.ste_tx_list_map != NULL)
			bus_dmamap_unload(sc->ste_cdata.ste_tx_list_tag,
			    sc->ste_cdata.ste_tx_list_map);
		if (sc->ste_cdata.ste_tx_list_map != NULL &&
		    sc->ste_ldata.ste_tx_list != NULL)
			bus_dmamem_free(sc->ste_cdata.ste_tx_list_tag,
			    sc->ste_ldata.ste_tx_list,
			    sc->ste_cdata.ste_tx_list_map);
		sc->ste_ldata.ste_tx_list = NULL;
		sc->ste_cdata.ste_tx_list_map = NULL;
		bus_dma_tag_destroy(sc->ste_cdata.ste_tx_list_tag);
		sc->ste_cdata.ste_tx_list_tag = NULL;
	}
	/* Rx descriptor list. */
	if (sc->ste_cdata.ste_rx_list_tag != NULL) {
		if (sc->ste_cdata.ste_rx_list_map != NULL)
			bus_dmamap_unload(sc->ste_cdata.ste_rx_list_tag,
			    sc->ste_cdata.ste_rx_list_map);
		if (sc->ste_cdata.ste_rx_list_map != NULL &&
		    sc->ste_ldata.ste_rx_list != NULL)
			bus_dmamem_free(sc->ste_cdata.ste_rx_list_tag,
			    sc->ste_ldata.ste_rx_list,
			    sc->ste_cdata.ste_rx_list_map);
		sc->ste_ldata.ste_rx_list = NULL;
		sc->ste_cdata.ste_rx_list_map = NULL;
		bus_dma_tag_destroy(sc->ste_cdata.ste_rx_list_tag);
		sc->ste_cdata.ste_rx_list_tag = NULL;
	}
	if (sc->ste_cdata.ste_parent_tag != NULL) {
		bus_dma_tag_destroy(sc->ste_cdata.ste_parent_tag);
		sc->ste_cdata.ste_parent_tag = NULL;
	}
}

static int
ste_newbuf(struct ste_softc *sc, struct ste_chain_onefrag *rxc)
{
	struct mbuf *m;
	bus_dma_segment_t segs[1];
	bus_dmamap_t map;
	int error, nsegs;

	m = m_getcl(M_DONTWAIT, MT_DATA, M_PKTHDR);
	if (m == NULL)
		return (ENOBUFS);
	m->m_len = m->m_pkthdr.len = MCLBYTES;
	m_adj(m, ETHER_ALIGN);

	if ((error = bus_dmamap_load_mbuf_sg(sc->ste_cdata.ste_rx_tag,
	    sc->ste_cdata.ste_rx_sparemap, m, segs, &nsegs, 0)) != 0) {
		m_freem(m);
		return (error);
	}
	KASSERT(nsegs == 1, ("%s: %d segments returned!", __func__, nsegs));

	if (rxc->ste_mbuf != NULL) {
		bus_dmamap_sync(sc->ste_cdata.ste_rx_tag, rxc->ste_map,
		    BUS_DMASYNC_POSTREAD);
		bus_dmamap_unload(sc->ste_cdata.ste_rx_tag, rxc->ste_map);
	}
	map = rxc->ste_map;
	rxc->ste_map = sc->ste_cdata.ste_rx_sparemap;
	sc->ste_cdata.ste_rx_sparemap = map;
	bus_dmamap_sync(sc->ste_cdata.ste_rx_tag, rxc->ste_map,
	    BUS_DMASYNC_PREREAD);
	rxc->ste_mbuf = m;
	rxc->ste_ptr->ste_status = 0;
	rxc->ste_ptr->ste_frag.ste_addr = htole32(segs[0].ds_addr);
	rxc->ste_ptr->ste_frag.ste_len = htole32(segs[0].ds_len |
	    STE_FRAG_LAST);
	return (0);
}

static int
ste_init_rx_list(struct ste_softc *sc)
{
	struct ste_chain_data *cd;
	struct ste_list_data *ld;
	int error, i;

	cd = &sc->ste_cdata;
	ld = &sc->ste_ldata;
	bzero(ld->ste_rx_list, STE_RX_LIST_SZ);
	for (i = 0; i < STE_RX_LIST_CNT; i++) {
		cd->ste_rx_chain[i].ste_ptr = &ld->ste_rx_list[i];
		error = ste_newbuf(sc, &cd->ste_rx_chain[i]);
		if (error != 0)
			return (error);
		if (i == (STE_RX_LIST_CNT - 1)) {
			cd->ste_rx_chain[i].ste_next = &cd->ste_rx_chain[0];
			ld->ste_rx_list[i].ste_next = ld->ste_rx_list_paddr +
			    (sizeof(struct ste_desc_onefrag) * 0);
		} else {
			cd->ste_rx_chain[i].ste_next = &cd->ste_rx_chain[i + 1];
			ld->ste_rx_list[i].ste_next = ld->ste_rx_list_paddr +
			    (sizeof(struct ste_desc_onefrag) * (i + 1));
		}
	}

	cd->ste_rx_head = &cd->ste_rx_chain[0];
	bus_dmamap_sync(sc->ste_cdata.ste_rx_list_tag,
	    sc->ste_cdata.ste_rx_list_map,
	    BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);

	return (0);
}

static void
ste_init_tx_list(struct ste_softc *sc)
{
	struct ste_chain_data *cd;
	struct ste_list_data *ld;
	int i;

	cd = &sc->ste_cdata;
	ld = &sc->ste_ldata;
	bzero(ld->ste_tx_list, STE_TX_LIST_SZ);
	for (i = 0; i < STE_TX_LIST_CNT; i++) {
		cd->ste_tx_chain[i].ste_ptr = &ld->ste_tx_list[i];
		cd->ste_tx_chain[i].ste_mbuf = NULL;
		if (i == (STE_TX_LIST_CNT - 1)) {
			cd->ste_tx_chain[i].ste_next = &cd->ste_tx_chain[0];
			cd->ste_tx_chain[i].ste_phys = htole32(STE_ADDR_LO(
			    ld->ste_tx_list_paddr +
			    (sizeof(struct ste_desc) * 0)));
		} else {
			cd->ste_tx_chain[i].ste_next = &cd->ste_tx_chain[i + 1];
			cd->ste_tx_chain[i].ste_phys = htole32(STE_ADDR_LO(
			    ld->ste_tx_list_paddr +
			    (sizeof(struct ste_desc) * (i + 1))));
		}
	}

	cd->ste_last_tx = NULL;
	cd->ste_tx_prod = 0;
	cd->ste_tx_cons = 0;
	cd->ste_tx_cnt = 0;

	bus_dmamap_sync(sc->ste_cdata.ste_tx_list_tag,
	    sc->ste_cdata.ste_tx_list_map,
	    BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);
}

static void
ste_init(void *xsc)
{
	struct ste_softc *sc;

	sc = xsc;
	STE_LOCK(sc);
	ste_init_locked(sc);
	STE_UNLOCK(sc);
}

static void
ste_init_locked(struct ste_softc *sc)
{
	struct ifnet *ifp;
	int i;

	STE_LOCK_ASSERT(sc);
	ifp = sc->ste_ifp;

	ste_stop(sc);
	/* Reset the chip to a known state. */
	ste_reset(sc);

	/* Init our MAC address */
	for (i = 0; i < ETHER_ADDR_LEN; i += 2) {
		CSR_WRITE_2(sc, STE_PAR0 + i,
		    ((IF_LLADDR(sc->ste_ifp)[i] & 0xff) |
		     IF_LLADDR(sc->ste_ifp)[i + 1] << 8));
	}

	/* Init RX list */
	if (ste_init_rx_list(sc) != 0) {
		device_printf(sc->ste_dev,
		    "initialization failed: no memory for RX buffers\n");
		ste_stop(sc);
		return;
	}

	/* Set RX polling interval */
	CSR_WRITE_1(sc, STE_RX_DMAPOLL_PERIOD, 64);

	/* Init TX descriptors */
	ste_init_tx_list(sc);

	/* Set the TX freethresh value */
	CSR_WRITE_1(sc, STE_TX_DMABURST_THRESH, STE_PACKET_SIZE >> 8);

	/* Set the TX start threshold for best performance. */
	CSR_WRITE_2(sc, STE_TX_STARTTHRESH, sc->ste_tx_thresh);

	/* Set the TX reclaim threshold. */
	CSR_WRITE_1(sc, STE_TX_RECLAIM_THRESH, (STE_PACKET_SIZE >> 4));

	/* Set up the RX filter. */
	CSR_WRITE_1(sc, STE_RX_MODE, STE_RXMODE_UNICAST);

	/* If we want promiscuous mode, set the allframes bit. */
	if (ifp->if_flags & IFF_PROMISC) {
		STE_SETBIT1(sc, STE_RX_MODE, STE_RXMODE_PROMISC);
	} else {
		STE_CLRBIT1(sc, STE_RX_MODE, STE_RXMODE_PROMISC);
	}

	/* Set capture broadcast bit to accept broadcast frames. */
	if (ifp->if_flags & IFF_BROADCAST) {
		STE_SETBIT1(sc, STE_RX_MODE, STE_RXMODE_BROADCAST);
	} else {
		STE_CLRBIT1(sc, STE_RX_MODE, STE_RXMODE_BROADCAST);
	}

	ste_setmulti(sc);

	/* Load the address of the RX list. */
	STE_SETBIT4(sc, STE_DMACTL, STE_DMACTL_RXDMA_STALL);
	ste_wait(sc);
	CSR_WRITE_4(sc, STE_RX_DMALIST_PTR,
	    STE_ADDR_LO(sc->ste_ldata.ste_rx_list_paddr));
	STE_SETBIT4(sc, STE_DMACTL, STE_DMACTL_RXDMA_UNSTALL);
	STE_SETBIT4(sc, STE_DMACTL, STE_DMACTL_RXDMA_UNSTALL);

	/* Set TX polling interval(defer until we TX first packet). */
	CSR_WRITE_1(sc, STE_TX_DMAPOLL_PERIOD, 0);

	/* Load address of the TX list */
	STE_SETBIT4(sc, STE_DMACTL, STE_DMACTL_TXDMA_STALL);
	ste_wait(sc);
	CSR_WRITE_4(sc, STE_TX_DMALIST_PTR, 0);
	STE_SETBIT4(sc, STE_DMACTL, STE_DMACTL_TXDMA_UNSTALL);
	STE_SETBIT4(sc, STE_DMACTL, STE_DMACTL_TXDMA_UNSTALL);
	ste_wait(sc);

	/* Enable receiver and transmitter */
	CSR_WRITE_2(sc, STE_MACCTL0, 0);
	CSR_WRITE_2(sc, STE_MACCTL1, 0);
	STE_SETBIT2(sc, STE_MACCTL1, STE_MACCTL1_TX_ENABLE);
	STE_SETBIT2(sc, STE_MACCTL1, STE_MACCTL1_RX_ENABLE);

	/* Enable stats counters. */
	STE_SETBIT2(sc, STE_MACCTL1, STE_MACCTL1_STATS_ENABLE);

	CSR_WRITE_2(sc, STE_ISR, 0xFFFF);
#ifdef DEVICE_POLLING
	/* Disable interrupts if we are polling. */
	if (ifp->if_capenable & IFCAP_POLLING)
		CSR_WRITE_2(sc, STE_IMR, 0);
	else
#endif
	/* Enable interrupts. */
	CSR_WRITE_2(sc, STE_IMR, STE_INTRS);

	/* Accept VLAN length packets */
	CSR_WRITE_2(sc, STE_MAX_FRAMELEN, ETHER_MAX_LEN + ETHER_VLAN_ENCAP_LEN);

	ste_ifmedia_upd_locked(ifp);

	ifp->if_drv_flags |= IFF_DRV_RUNNING;
	ifp->if_drv_flags &= ~IFF_DRV_OACTIVE;

	callout_reset(&sc->ste_callout, hz, ste_tick, sc);
}

static void
ste_stop(struct ste_softc *sc)
{
	struct ifnet *ifp;
	struct ste_chain_onefrag *cur_rx;
	struct ste_chain *cur_tx;
	uint32_t val;
	int i;

	STE_LOCK_ASSERT(sc);
	ifp = sc->ste_ifp;

	callout_stop(&sc->ste_callout);
	sc->ste_timer = 0;
	ifp->if_drv_flags &= ~(IFF_DRV_RUNNING|IFF_DRV_OACTIVE);

	CSR_WRITE_2(sc, STE_IMR, 0);
	/* Stop pending DMA. */
	val = CSR_READ_4(sc, STE_DMACTL);
	val |= STE_DMACTL_TXDMA_STALL | STE_DMACTL_RXDMA_STALL;
	CSR_WRITE_4(sc, STE_DMACTL, val);
	ste_wait(sc);
	/* Disable auto-polling. */
	CSR_WRITE_1(sc, STE_RX_DMAPOLL_PERIOD, 0);
	CSR_WRITE_1(sc, STE_TX_DMAPOLL_PERIOD, 0);
	/* Nullify DMA address to stop any further DMA. */
	CSR_WRITE_4(sc, STE_RX_DMALIST_PTR, 0);
	CSR_WRITE_4(sc, STE_TX_DMALIST_PTR, 0);
	/* Stop TX/RX MAC. */
	val = CSR_READ_2(sc, STE_MACCTL1);
	val |= STE_MACCTL1_TX_DISABLE | STE_MACCTL1_RX_DISABLE |
	    STE_MACCTL1_STATS_DISABLE;
	CSR_WRITE_2(sc, STE_MACCTL1, val);
	for (i = 0; i < STE_TIMEOUT; i++) {
		DELAY(10);
		if ((CSR_READ_2(sc, STE_MACCTL1) & (STE_MACCTL1_TX_DISABLE |
		    STE_MACCTL1_RX_DISABLE | STE_MACCTL1_STATS_DISABLE)) == 0)
			break;
	}
	if (i == STE_TIMEOUT)
		device_printf(sc->ste_dev, "Stopping MAC timed out\n");
	/* Acknowledge any pending interrupts. */
	CSR_READ_2(sc, STE_ISR_ACK);
	ste_stats_update(sc);

	for (i = 0; i < STE_RX_LIST_CNT; i++) {
		cur_rx = &sc->ste_cdata.ste_rx_chain[i];
		if (cur_rx->ste_mbuf != NULL) {
			bus_dmamap_sync(sc->ste_cdata.ste_rx_tag,
			    cur_rx->ste_map, BUS_DMASYNC_POSTREAD);
			bus_dmamap_unload(sc->ste_cdata.ste_rx_tag,
			    cur_rx->ste_map);
			m_freem(cur_rx->ste_mbuf);
			cur_rx->ste_mbuf = NULL;
		}
	}

	for (i = 0; i < STE_TX_LIST_CNT; i++) {
		cur_tx = &sc->ste_cdata.ste_tx_chain[i];
		if (cur_tx->ste_mbuf != NULL) {
			bus_dmamap_sync(sc->ste_cdata.ste_tx_tag,
			    cur_tx->ste_map, BUS_DMASYNC_POSTWRITE);
			bus_dmamap_unload(sc->ste_cdata.ste_tx_tag,
			    cur_tx->ste_map);
			m_freem(cur_tx->ste_mbuf);
			cur_tx->ste_mbuf = NULL;
		}
	}
}

static void
ste_reset(struct ste_softc *sc)
{
	int i;

	STE_SETBIT4(sc, STE_ASICCTL,
	    STE_ASICCTL_GLOBAL_RESET|STE_ASICCTL_RX_RESET|
	    STE_ASICCTL_TX_RESET|STE_ASICCTL_DMA_RESET|
	    STE_ASICCTL_FIFO_RESET|STE_ASICCTL_NETWORK_RESET|
	    STE_ASICCTL_AUTOINIT_RESET|STE_ASICCTL_HOST_RESET|
	    STE_ASICCTL_EXTRESET_RESET);

	DELAY(100000);

	for (i = 0; i < STE_TIMEOUT; i++) {
		if (!(CSR_READ_4(sc, STE_ASICCTL) & STE_ASICCTL_RESET_BUSY))
			break;
	}

	if (i == STE_TIMEOUT)
		device_printf(sc->ste_dev, "global reset never completed\n");
}

static int
ste_ioctl(struct ifnet *ifp, u_long command, caddr_t data)
{
	struct ste_softc *sc;
	struct ifreq *ifr;
	struct mii_data *mii;
	int error = 0;

	sc = ifp->if_softc;
	ifr = (struct ifreq *)data;

	switch (command) {
	case SIOCSIFFLAGS:
		STE_LOCK(sc);
		if (ifp->if_flags & IFF_UP) {
			if (ifp->if_drv_flags & IFF_DRV_RUNNING &&
			    ifp->if_flags & IFF_PROMISC &&
			    !(sc->ste_if_flags & IFF_PROMISC)) {
				STE_SETBIT1(sc, STE_RX_MODE,
				    STE_RXMODE_PROMISC);
			} else if (ifp->if_drv_flags & IFF_DRV_RUNNING &&
			    !(ifp->if_flags & IFF_PROMISC) &&
			    sc->ste_if_flags & IFF_PROMISC) {
				STE_CLRBIT1(sc, STE_RX_MODE,
				    STE_RXMODE_PROMISC);
			}
			if (ifp->if_drv_flags & IFF_DRV_RUNNING &&
			    (ifp->if_flags ^ sc->ste_if_flags) & IFF_ALLMULTI)
				ste_setmulti(sc);
			if (!(ifp->if_drv_flags & IFF_DRV_RUNNING)) {
				sc->ste_tx_thresh = STE_TXSTART_THRESH;
				ste_init_locked(sc);
			}
		} else {
			if (ifp->if_drv_flags & IFF_DRV_RUNNING)
				ste_stop(sc);
		}
		sc->ste_if_flags = ifp->if_flags;
		STE_UNLOCK(sc);
		error = 0;
		break;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		STE_LOCK(sc);
		ste_setmulti(sc);
		STE_UNLOCK(sc);
		error = 0;
		break;
	case SIOCGIFMEDIA:
	case SIOCSIFMEDIA:
		mii = device_get_softc(sc->ste_miibus);
		error = ifmedia_ioctl(ifp, ifr, &mii->mii_media, command);
		break;
	case SIOCSIFCAP:
#ifdef DEVICE_POLLING
		if (ifr->ifr_reqcap & IFCAP_POLLING &&
		    !(ifp->if_capenable & IFCAP_POLLING)) {
			error = ether_poll_register(ste_poll, ifp);
			if (error)
				return (error);
			STE_LOCK(sc);
			/* Disable interrupts */
			CSR_WRITE_2(sc, STE_IMR, 0);
			ifp->if_capenable |= IFCAP_POLLING;
			STE_UNLOCK(sc);
			return (error);

		}
		if (!(ifr->ifr_reqcap & IFCAP_POLLING) &&
		    ifp->if_capenable & IFCAP_POLLING) {
			error = ether_poll_deregister(ifp);
			/* Enable interrupts. */
			STE_LOCK(sc);
			CSR_WRITE_2(sc, STE_IMR, STE_INTRS);
			ifp->if_capenable &= ~IFCAP_POLLING;
			STE_UNLOCK(sc);
			return (error);
		}
#endif /* DEVICE_POLLING */
		break;
	default:
		error = ether_ioctl(ifp, command, data);
		break;
	}

	return (error);
}

static int
ste_encap(struct ste_softc *sc, struct mbuf **m_head, struct ste_chain *txc)
{
	struct ste_frag *frag;
	struct mbuf *m;
	struct ste_desc *desc;
	bus_dma_segment_t txsegs[STE_MAXFRAGS];
	int error, i, nsegs;

	STE_LOCK_ASSERT(sc);
	M_ASSERTPKTHDR((*m_head));

	error = bus_dmamap_load_mbuf_sg(sc->ste_cdata.ste_tx_tag,
	    txc->ste_map, *m_head, txsegs, &nsegs, 0);
	if (error == EFBIG) {
		m = m_collapse(*m_head, M_DONTWAIT, STE_MAXFRAGS);
		if (m == NULL) {
			m_freem(*m_head);
			*m_head = NULL;
			return (ENOMEM);
		}
		*m_head = m;
		error = bus_dmamap_load_mbuf_sg(sc->ste_cdata.ste_tx_tag,
		    txc->ste_map, *m_head, txsegs, &nsegs, 0);
		if (error != 0) {
			m_freem(*m_head);
			*m_head = NULL;
			return (error);
		}
	} else if (error != 0)
		return (error);
	if (nsegs == 0) {
		m_freem(*m_head);
		*m_head = NULL;
		return (EIO);
	}
	bus_dmamap_sync(sc->ste_cdata.ste_tx_tag, txc->ste_map,
	    BUS_DMASYNC_PREWRITE);

	desc = txc->ste_ptr;
	for (i = 0; i < nsegs; i++) {
		frag = &desc->ste_frags[i];
		frag->ste_addr = htole32(STE_ADDR_LO(txsegs[i].ds_addr));
		frag->ste_len = htole32(txsegs[i].ds_len);
	}
	desc->ste_frags[i - 1].ste_len |= htole32(STE_FRAG_LAST);
	/*
	 * Because we use Tx polling we can't chain multiple
	 * Tx descriptors here. Otherwise we race with controller.
	 */
	desc->ste_next = 0;
	desc->ste_ctl = htole32(STE_TXCTL_ALIGN_DIS | STE_TXCTL_DMAINTR);
	txc->ste_mbuf = *m_head;
	STE_INC(sc->ste_cdata.ste_tx_prod, STE_TX_LIST_CNT);
	sc->ste_cdata.ste_tx_cnt++;

	return (0);
}

static void
ste_start(struct ifnet *ifp)
{
	struct ste_softc *sc;

	sc = ifp->if_softc;
	STE_LOCK(sc);
	ste_start_locked(ifp);
	STE_UNLOCK(sc);
}

static void
ste_start_locked(struct ifnet *ifp)
{
	struct ste_softc *sc;
	struct ste_chain *cur_tx;
	struct mbuf *m_head = NULL;
	int enq;

	sc = ifp->if_softc;
	STE_LOCK_ASSERT(sc);

	if ((ifp->if_drv_flags & (IFF_DRV_RUNNING | IFF_DRV_OACTIVE)) !=
	    IFF_DRV_RUNNING || (sc->ste_flags & STE_FLAG_LINK) == 0)
		return;

	for (enq = 0; !IFQ_DRV_IS_EMPTY(&ifp->if_snd);) {
		if (sc->ste_cdata.ste_tx_cnt == STE_TX_LIST_CNT - 1) {
			/*
			 * Controller may have cached copy of the last used
			 * next ptr so we have to reserve one TFD to avoid
			 * TFD overruns.
			 */
			ifp->if_drv_flags |= IFF_DRV_OACTIVE;
			break;
		}
		IFQ_DRV_DEQUEUE(&ifp->if_snd, m_head);
		if (m_head == NULL)
			break;
		cur_tx = &sc->ste_cdata.ste_tx_chain[sc->ste_cdata.ste_tx_prod];
		if (ste_encap(sc, &m_head, cur_tx) != 0) {
			if (m_head == NULL)
				break;
			IFQ_DRV_PREPEND(&ifp->if_snd, m_head);
			break;
		}
		if (sc->ste_cdata.ste_last_tx == NULL) {
			bus_dmamap_sync(sc->ste_cdata.ste_tx_list_tag,
			    sc->ste_cdata.ste_tx_list_map,
			    BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);
			STE_SETBIT4(sc, STE_DMACTL, STE_DMACTL_TXDMA_STALL);
			ste_wait(sc);
			CSR_WRITE_4(sc, STE_TX_DMALIST_PTR,
	    		    STE_ADDR_LO(sc->ste_ldata.ste_tx_list_paddr));
			CSR_WRITE_1(sc, STE_TX_DMAPOLL_PERIOD, 64);
			STE_SETBIT4(sc, STE_DMACTL, STE_DMACTL_TXDMA_UNSTALL);
			ste_wait(sc);
		} else {
			sc->ste_cdata.ste_last_tx->ste_ptr->ste_next =
			    sc->ste_cdata.ste_last_tx->ste_phys;
			bus_dmamap_sync(sc->ste_cdata.ste_tx_list_tag,
			    sc->ste_cdata.ste_tx_list_map,
			    BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);
		}
		sc->ste_cdata.ste_last_tx = cur_tx;

		enq++;
		/*
		 * If there's a BPF listener, bounce a copy of this frame
		 * to him.
	 	 */
		BPF_MTAP(ifp, m_head);
	}

	if (enq > 0)
		sc->ste_timer = STE_TX_TIMEOUT;
}

static void
ste_watchdog(struct ste_softc *sc)
{
	struct ifnet *ifp;

	ifp = sc->ste_ifp;
	STE_LOCK_ASSERT(sc);

	if (sc->ste_timer == 0 || --sc->ste_timer)
		return;

	ifp->if_oerrors++;
	if_printf(ifp, "watchdog timeout\n");

	ste_txeoc(sc);
	ste_txeof(sc);
	ste_rxeof(sc, -1);
	ste_init_locked(sc);

	if (!IFQ_DRV_IS_EMPTY(&ifp->if_snd))
		ste_start_locked(ifp);
}

static int
ste_shutdown(device_t dev)
{
	struct ste_softc *sc;

	sc = device_get_softc(dev);

	STE_LOCK(sc);
	ste_stop(sc);
	STE_UNLOCK(sc);

	return (0);
}
