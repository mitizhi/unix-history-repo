/*
 * Copyright (c) 1994, Garrett A. Wollman.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
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
 *
 *	$Id: devconf.h,v 1.1 1994/10/16 03:50:01 wollman Exp $
 */
/*
 * devconf.h - machine-dependent device configuration table
 *
 * Garrett A. Wollman, October 1994.
 */

#ifndef _MACHINE_DEVCONF_H_
#define _MACHINE_DEVCONF_H_ 1

#define PARENTNAMELEN	32

enum machdep_devtype { MDDT_CPU, MDDT_ISA, MDDT_EISA, MDDT_PCI, MDDT_SCSI,
	       MDDT_DISK, NDEVTYPES };

#define DEVTYPENAMES { \
			       "cpu", \
			       "isa", \
			       "eisa", \
			       "pci", \
			       "scsi", \
			       "disk", \
			       0 \
	       }

struct machdep_devconf {
	char mddc_parent[PARENTNAMELEN];
	enum machdep_devtype mddc_devtype;
	int mddc_flags;
};

#define dc_parent dc_md.mddc_parent
#define dc_devtype dc_md.mddc_devtype
#define dc_flags dc_md.mddc_flags	

#include <i386/isa/isa_device.h>
#include <i386/pci/pcireg.h>
#include <scsi/scsi_all.h>
#include <scsi/scsiconf.h>

#define ISA_EXTERNALLEN (sizeof(struct isa_device))
#define EISA_EXTERNALLEN (sizeof(struct isa_device) + sizeof(int))
#define PCI_EXTERNALLEN (sizeof(struct pci_device))
#define SCSI_EXTERNALLEN (sizeof(struct scsi_link))
#define DISK_EXTERNALLEN (sizeof(int))

#ifdef KERNEL			/* XXX move these */
extern int isa_externalize(struct isa_device *, void *, size_t);
extern int isa_internalize(struct isa_device *, void *, size_t);

extern int eisa_externalize(struct isa_device *, int, void *, size_t);

extern int pci_externalize(struct pci_device *, void *, size_t);

extern int scsi_externalize(struct scsi_link *, void *, size_t);
extern int scsi_internalize(struct scsi_link *, void *, size_t);
#endif

#endif /* _MACHINE_DEVCONF_H_ */
