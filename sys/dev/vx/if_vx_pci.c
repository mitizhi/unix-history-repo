/*-
 * Copyright (C) 1996 Naoki Hamada <nao@tom-yam.or.jp>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/if_arp.h>

#include <machine/bus_pio.h>
#include <machine/bus.h>
#include <machine/resource.h>
#include <sys/bus.h>
#include <sys/rman.h>

#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>

#include <dev/vx/if_vxreg.h>
#include <dev/vx/if_vxvar.h>

static void vx_pci_shutdown(device_t);
static int vx_pci_probe(device_t);
static int vx_pci_attach(device_t);

static device_method_t vx_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe, vx_pci_probe),
	DEVMETHOD(device_attach, vx_pci_attach),
	DEVMETHOD(device_shutdown, vx_pci_shutdown),

	{0, 0}
};

static driver_t vx_driver = {
	"vx",
	vx_methods,
	sizeof(struct vx_softc)
};

static devclass_t vx_devclass;

DRIVER_MODULE(vx, pci, vx_driver, vx_devclass, 0, 0);
MODULE_DEPEND(vx, pci, 1, 1, 1);
MODULE_DEPEND(vx, ether, 1, 1, 1);

static void
vx_pci_shutdown(device_t dev)
{
	struct vx_softc *sc;

	sc = device_get_softc(dev);
	vxstop(sc);
}

static int
vx_pci_probe(device_t dev)
{
	u_int32_t device_id;

	device_id = pci_read_config(dev, PCIR_DEVVENDOR, 4);

	if (device_id == 0x590010b7ul) {
		device_set_desc(dev, "3COM 3C590 Etherlink III PCI");
		return (0);
	}
	if (device_id == 0x595010b7ul || device_id == 0x595110b7ul ||
	    device_id == 0x595210b7ul) {
		device_set_desc(dev, "3COM 3C595 Etherlink III PCI");
		return (0);
	}
	/*
	 * The (Fast) Etherlink XL adapters are now supported by
	 * the xl driver, which uses bus master DMA and is much
	 * faster. (And which also supports the 3c905B.
	 */
#ifdef VORTEX_ETHERLINK_XL
	if (device_id == 0x900010b7ul || device_id == 0x900110b7ul) {
		device_set_desc(dev, "3COM 3C900 Etherlink XL PCI");
		return (0);
	}
	if (device_id == 0x905010b7ul || device_id == 0x905110b7ul) {
		device_set_desc(dev, "3COM 3C905 Etherlink XL PCI");
		return (0);
	}
#endif
	return (ENXIO);
}

static int
vx_pci_attach(device_t dev)
{
	struct vx_softc *sc;
	int rid;

	sc = device_get_softc(dev);

	rid = PCIR_BAR(0);
	sc->vx_res =
	    bus_alloc_resource_any(dev, SYS_RES_IOPORT, &rid, RF_ACTIVE);

	if (sc->vx_res == NULL)
		goto bad;

	sc->bst = rman_get_bustag(sc->vx_res);
	sc->bsh = rman_get_bushandle(sc->vx_res);

	rid = 0;
	sc->vx_irq = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid,
	    RF_SHAREABLE | RF_ACTIVE);

	if (sc->vx_irq == NULL)
		goto bad;

	if (bus_setup_intr(dev, sc->vx_irq, INTR_TYPE_NET,
	    vxintr, sc, &sc->vx_intrhand))
		goto bad;

	if (vxattach(dev) == 0)
		goto bad;

	/* defect check for 3C590 */
	if ((pci_read_config(dev, PCIR_DEVVENDOR, 4) >> 16) == 0x5900) {
		GO_WINDOW(0);
		if (vxbusyeeprom(sc))
			goto bad;
		CSR_WRITE_2(sc, VX_W0_EEPROM_COMMAND,
		    EEPROM_CMD_RD | EEPROM_SOFTINFO2);
		if (vxbusyeeprom(sc))
			goto bad;
		if (!(CSR_READ_2(sc, VX_W0_EEPROM_DATA) & NO_RX_OVN_ANOMALY))
			printf("Warning! Defective early revision adapter!\n");
	}
	return (0);

bad:
	if (sc->vx_intrhand != NULL)
		bus_teardown_intr(dev, sc->vx_irq, sc->vx_intrhand);
	if (sc->vx_res != NULL)
		bus_release_resource(dev, SYS_RES_IOPORT, 0, sc->vx_res);
	if (sc->vx_irq != NULL)
		bus_release_resource(dev, SYS_RES_IRQ, 0, sc->vx_irq);
	return (ENXIO);
}
