/*
 * Copyright (C) 2002
 * 	Hidetoshi Shimokawa. All rights reserved.
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
 *
 *	This product includes software developed by Hidetoshi Shimokawa.
 *
 * 4. Neither the name of the author nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * 
 * $FreeBSD$
 */

#include <sys/param.h>
#if defined(_KERNEL) || defined(TEST)
#include <sys/queue.h>
#endif
#ifdef _KERNEL
#include <sys/systm.h>
#include <sys/kernel.h>
#else
#include <netinet/in.h>
#include <fcntl.h>
#include <stdio.h>
#include <err.h>
#include <stdlib.h>
#include <string.h>
#endif
#include <dev/firewire/firewire.h>
#include <dev/firewire/iec13213.h>

void
crom_init_context(struct crom_context *cc, u_int32_t *p)
{
	struct csrhdr *hdr;

	hdr = (struct csrhdr *)p;
	if (hdr->info_len == 1) {
		/* minimum ROM */
		cc->depth = -1;
	}
	p += 1 + hdr->info_len;
	cc->depth = 0;
	cc->stack[0].dir = (struct csrdirectory *)p;
	cc->stack[0].index = 0;
}

struct csrreg *
crom_get(struct crom_context *cc)
{
	struct crom_ptr *ptr;

	ptr = &cc->stack[cc->depth];
	return (&ptr->dir->entry[ptr->index]);
}

void
crom_next(struct crom_context *cc)
{
	struct crom_ptr *ptr;
	struct csrreg *reg;

	if (cc->depth < 0)
		return;
	reg = crom_get(cc);
	if ((reg->key & CSRTYPE_MASK) == CSRTYPE_D) {
		cc->depth ++;
		if (cc->depth > CROM_MAX_DEPTH) {
			printf("crom_next: too deep\n");
			cc->depth --;
			goto again;
		}
		ptr = &cc->stack[cc->depth];
		ptr->dir = (struct csrdirectory *) (reg + reg->val);
		ptr->index = 0;
		goto check;
	}
again:
	ptr = &cc->stack[cc->depth];
	ptr->index ++;
check:
	if (ptr->index < ptr->dir->crc_len)
		return;
	if (cc->depth > 0) {
		cc->depth--;
		goto again;
	}
	/* no more data */
	cc->depth = -1;
}


struct csrreg *
crom_search_key(struct crom_context *cc, u_int8_t key)
{
	struct csrreg *reg;

	while(cc->depth >= 0) {
		reg = crom_get(cc);
		if (reg->key == key)
			return reg;
		crom_next(cc);
	}
	return NULL;
}

void
crom_parse_text(struct crom_context *cc, char *buf, int len)
{
	struct csrreg *reg;
	struct csrtext *textleaf;
	u_int32_t *bp;
	int i, qlen;
	static char *nullstr = "(null)";

	reg = crom_get(cc);
	if (reg->key != CROM_TEXTLEAF) {
		strncpy(buf, nullstr, len);
		return;
	}
	textleaf = (struct csrtext *)(reg + reg->val);

	/* XXX should check spec and type */

	bp = (u_int32_t *)&buf[0];
	qlen = textleaf->crc_len - 2;
	if (len < qlen * 4)
		qlen = len/4;
	for (i = 0; i < qlen; i ++)
		*bp++ = ntohl(textleaf->text[i]);
	/* make sure to terminate the string */
	if (len <= qlen * 4)
		buf[len - 1] = 0;
	else
		buf[qlen * 4] = 0;
}

u_int16_t
crom_crc(u_int32_t *ptr, int len)
{
	int i, shift;
	u_int32_t data, sum, crc = 0;

	for (i = 0; i < len; i++) {
		data = ptr[i];
		for (shift = 28; shift >= 0; shift -= 4) {
			sum = ((crc >> 12) ^ (data >> shift)) & 0xf;
			crc = (crc << 4) ^ (sum << 12) ^ (sum << 5) ^ sum;
		}
		crc &= 0xffff;
	}
	return((u_int16_t) crc);
}

#ifndef _KERNEL
char *
crom_desc(struct crom_context *cc, char *buf, int len)
{
	struct csrreg *reg;
	struct csrdirectory *dir;
	char *desc;
	u_int16_t crc;

	reg = crom_get(cc);
	switch (reg->key & CSRTYPE_MASK) {
	case CSRTYPE_I:
		snprintf(buf, len, "%d", reg->val);
		break;
	case CSRTYPE_C:
		snprintf(buf, len, "offset=0x%04x(%d)", reg->val, reg->val);
		break;
	case CSRTYPE_L:
		/* XXX fall through */
	case CSRTYPE_D:
		dir = (struct csrdirectory *) (reg + reg->val);
		crc = crom_crc((u_int32_t *)&dir->entry[0], dir->crc_len);
		len -= snprintf(buf, len, "len=%d crc=0x%04x",
			dir->crc_len, dir->crc);
		if (crc == dir->crc)
			strncat(buf, "(OK) ", len);
		else
			strncat(buf, "(NG) ", len);
		len -= 5;
	}
	switch (reg->key) {
	case 0x03:
		desc = "module_vendor_ID";
		break;
	case 0x04:
		desc = "hardware_version";
		break;
	case 0x0c:
		desc = "node_capabilities";
		break;
	case 0x12:
		desc = "unit_spec_ID";
		break;
	case 0x13:
		desc = "unit_sw_version";
		break;
	case 0x14:
		desc = "logical_unit_number";
		break;
	case 0x17:
		desc = "model_ID";
		break;
	case 0x38:
		desc = "command_set_spec_ID";
		break;
	case 0x39:
		desc = "command_set";
		break;
	case 0x3a:
		desc = "unit_characteristics";
		break;
	case 0x3b:
		desc = "command_set_revision";
		break;
	case 0x3c:
		desc = "firmware_revision";
		break;
	case 0x3d:
		desc = "reconnect_timeout";
		break;
	case 0x54:
		desc = "management_agent";
		break;
	case 0x81:
		desc = "text_leaf";
		crom_parse_text(cc, buf + strlen(buf), len);
		break;
	case 0xd1:
		desc = "unit_directory";
		break;
	case 0xd4:
		desc = "logical_unit_directory";
		break;
	default:
		desc = "unknown";
	}
	return desc;
}
#endif

#if defined(_KERNEL) || defined(TEST)

int
crom_add_quad(struct crom_chunk *chunk, u_int32_t entry)
{
	int index;

	index = chunk->data.crc_len;
	if (index >= CROM_MAX_CHUNK_LEN - 1) {
		printf("too large chunk %d\n", index);
		return(-1);
	}
	chunk->data.buf[index] = entry;
	chunk->data.crc_len++;
	return(index);
}

int
crom_add_entry(struct crom_chunk *chunk, int key, int val)
{
	struct csrreg *reg;
	u_int32_t i;
	
	reg = (struct csrreg *)&i;
	reg->key = key;
	reg->val = val;
	return(crom_add_quad(chunk, (u_int32_t) i));
}

int
crom_add_chunk(struct crom_src *src, struct crom_chunk *parent,
				struct crom_chunk *child, int key)
{
	int index;

	if (parent == NULL) {
		STAILQ_INSERT_TAIL(&src->chunk_list, child, link);
		return(0);
	}

	index = crom_add_entry(parent, key, 0);
	if (index < 0) {
		return(-1);
	}
	child->ref_chunk = parent;
	child->ref_index = index;
	STAILQ_INSERT_TAIL(&src->chunk_list, child, link);
	return(index);
}

int
crom_add_simple_text(struct crom_src *src, struct crom_chunk *parent,
				struct crom_chunk *chunk, char *buf)
{
	struct csrtext *tl;
	u_int32_t *p;
	int len, i;

	len = strlen(buf);
#define MAX_TEXT ((CROM_MAX_CHUNK_LEN + 1) * 4 - sizeof(struct csrtext))
	if (len > MAX_TEXT) {
		printf("text(%d) trancated to %d.\n", len, MAX_TEXT);
		len = MAX_TEXT;
	}

	tl = (struct csrtext *) &chunk->data;
	tl->crc_len = roundup2(sizeof(struct csrtext) + len, 4) / 4;
	tl->spec_id = 0;
	tl->spec_type = 0;
	tl->lang_id = 0;
	p = (u_int32_t *) buf;
	for (i = 0; i < roundup2(len, 4) / 4; i ++)
		tl->text[i] = ntohl(*p++);
	return (crom_add_chunk(src, parent, chunk, CROM_TEXTLEAF));
}

static int
crom_copy(u_int32_t *src, u_int32_t *dst, int *offset, int len, int maxlen)
{
	if (*offset + len > maxlen) {
		printf("Config. ROM is too large for the buffer\n");
		return(-1);
	}
	bcopy(src, (char *)(dst + *offset), len * sizeof(u_int32_t));
	*offset += len;
	return(0);
}

int
crom_load(struct crom_src *src, u_int32_t *buf, int maxlen)
{
	struct crom_chunk *chunk, *parent;
	struct csrhdr *hdr;
#if 0
	u_int32_t *ptr;
#endif
	int count, offset;
	int len;

	offset = 0;
	/* Determine offset */
	STAILQ_FOREACH(chunk, &src->chunk_list, link) {
		chunk->offset = offset;
		/* Assume the offset of the parent is already known */
		parent = chunk->ref_chunk;
		if (parent != NULL) {
			struct csrreg *reg;
			reg = (struct csrreg *)
				&parent->data.buf[chunk->ref_index];
			reg->val = offset -
				(parent->offset + 1 + chunk->ref_index);
		}
		offset += 1 + chunk->data.crc_len;
	}

	/* Calculate CRC and dump to the buffer */
	len = 1 + src->hdr.info_len;
	count = 0;
	if (crom_copy((u_int32_t *)&src->hdr, buf, &count, len, maxlen) < 0)
		return(-1);
	STAILQ_FOREACH(chunk, &src->chunk_list, link) {
		chunk->data.crc =
			crom_crc(&chunk->data.buf[0], chunk->data.crc_len);

		len = 1 + chunk->data.crc_len;
		if (crom_copy((u_int32_t *)&chunk->data, buf,
					&count, len, maxlen) < 0)
			return(-1);
	}
	hdr = (struct csrhdr *)buf;
	hdr->crc_len = count - 1;
	hdr->crc = crom_crc(buf + 1, hdr->crc_len);

#if 0
	/* byte swap */
	ptr = buf;
	for (i = 0; i < count; i ++) {
		*ptr = htonl(*ptr);
		ptr++;
	}
#endif

	return(count);
}
#endif

#ifdef TEST
int
main () {
	struct crom_src src;
	struct crom_chunk root,unit1,unit2,unit3;
	struct crom_chunk text1,text2,text3,text4,text5,text6,text7;
	u_int32_t buf[256], *p;
	int i;

	bzero(&src, sizeof(src));
	bzero(&root, sizeof(root));
	bzero(&unit1, sizeof(unit1));
	bzero(&unit2, sizeof(unit2));
	bzero(&unit3, sizeof(unit3));
	bzero(&text1, sizeof(text1));
	bzero(&text2, sizeof(text2));
	bzero(&text3, sizeof(text3));
	bzero(&text3, sizeof(text4));
	bzero(&text3, sizeof(text5));
	bzero(&text3, sizeof(text6));
	bzero(&text3, sizeof(text7));
	bzero(buf, sizeof(buf));

	/* BUS info sample */
	src.hdr.info_len = 4;
	src.businfo.bus_name = CSR_BUS_NAME_IEEE1394;
	src.businfo.eui64.hi = 0x11223344;
	src.businfo.eui64.lo = 0x55667788;
	src.businfo.link_spd = FWSPD_S400;
	src.businfo.generation = 0;
	src.businfo.max_rom = MAXROM_4;
	src.businfo.max_rec = 10;
	src.businfo.cyc_clk_acc = 100;
	src.businfo.pmc = 0;
	src.businfo.bmc = 1;
	src.businfo.isc = 1;
	src.businfo.cmc = 1;
	src.businfo.irmc = 1;
	STAILQ_INIT(&src.chunk_list);

	/* Root directory */
	crom_add_chunk(&src, NULL, &root, 0);
	crom_add_entry(&root, CSRKEY_NCAP, 0x123456);
	/* private company_id */
	crom_add_entry(&root, CSRKEY_VENDOR, 0xacde48);

	crom_add_simple_text(&src, &root, &text1, "FreeBSD");
	crom_add_entry(&root, CSRKEY_HW, __FreeBSD_version);
	crom_add_simple_text(&src, &root, &text2, "FreeBSD-5");

	/* SBP unit directory */
	crom_add_chunk(&src, &root, &unit1, CROM_UDIR);
	crom_add_entry(&unit1, CSRKEY_SPEC, CSRVAL_ANSIT10);
	crom_add_entry(&unit1, CSRKEY_VER, CSRVAL_T10SBP2);
	crom_add_entry(&unit1, CSRKEY_COM_SPEC, CSRVAL_ANSIT10);
	crom_add_entry(&unit1, CSRKEY_COM_SET, CSRVAL_SCSI);
	/* management_agent */
	crom_add_entry(&unit1, CROM_MGM, 0x1000);
	crom_add_entry(&unit1, CSRKEY_UNIT_CH, (10<<8) | 8);
	/* Device type and LUN */
	crom_add_entry(&unit1, CROM_LUN, 0);
	crom_add_entry(&unit1, CSRKEY_MODEL, 1);
	crom_add_simple_text(&src, &unit1, &text3, "scsi_target");

	/* RFC2734 IPv4 over IEEE1394 */
	crom_add_chunk(&src, &root, &unit2, CROM_UDIR);
	crom_add_entry(&unit2, CSRKEY_SPEC, CSRVAL_IETF);
	crom_add_simple_text(&src, &unit2, &text4, "IANA");
	crom_add_entry(&unit2, CSRKEY_VER, 1);
	crom_add_simple_text(&src, &unit2, &text5, "IPv4");

	/* RFC3146 IPv6 over IEEE1394 */
	crom_add_chunk(&src, &root, &unit3, CROM_UDIR);
	crom_add_entry(&unit3, CSRKEY_SPEC, CSRVAL_IETF);
	crom_add_simple_text(&src, &unit3, &text6, "IANA");
	crom_add_entry(&unit3, CSRKEY_VER, 2);
	crom_add_simple_text(&src, &unit3, &text7, "IPv6");

	crom_load(&src, buf, 256);
	p = buf;
#define DUMP_FORMAT     "%08x %08x %08x %08x %08x %08x %08x %08x\n"
	for (i = 0; i < 256/8; i ++) {
		printf(DUMP_FORMAT,
			p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
		p += 8;
	}
	return(0);
}
#endif
