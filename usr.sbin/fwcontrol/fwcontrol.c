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
#include <sys/malloc.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/errno.h>
#include <dev/firewire/firewire.h>
#include <dev/firewire/iec13213.h>

#include <netinet/in.h>
#include <fcntl.h>
#include <stdio.h>
#include <err.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void
usage(void)
{
	printf("fwcontrol [-g gap_count] [-b pri_req] [-c node]"
		" [-r] [-t] [-s] [-d node] [-l file]\n");
	printf("\t-g: broadcast gap_count by phy_config packet\n");
	printf("\t-b: set PRIORITY_BUDGET register on all supported nodes\n");
	printf("\t-c: read configuration ROM\n");
	printf("\t-r: bus reset\n");
	printf("\t-t: read topology map\n");
	printf("\t-s: read speed map\n");
	printf("\t-d: hex dump of configuration ROM\n");
	printf("\t-l: load and parse hex dump file of configuration ROM\n");
	exit(0);
}

void
get_num_of_dev(int fd, struct fw_devlstreq *data)
{
	data->n = 64;
	if( ioctl(fd, FW_GDEVLST, data) < 0) {
       			err(1, "ioctl");
	}
}

void
list_dev(int fd)
{
	struct fw_devlstreq data;
	int i;

	get_num_of_dev(fd, &data);
	printf("%d devices\n", data.n);
	for (i = 0; i < data.n; i++) {
		printf("%d node %d eui:%08x%08x status:%d\n",
			i,
			data.dst[i],
			data.eui[i].hi,
			data.eui[i].lo,
			data.status[i]
		);
	}
}

u_int32_t
read_write_quad(int fd, struct fw_eui64 eui, u_int32_t addr_lo, int read, u_int32_t data)
{
        struct fw_asyreq *asyreq;
	u_int32_t *qld, res;

        asyreq = (struct fw_asyreq *)malloc(sizeof(struct fw_asyreq_t) + 16);
	asyreq->req.len = 16;
	asyreq->req.type = FWASREQEUI;
	asyreq->req.dst.eui = eui;
#if 0
	asyreq->pkt.mode.rreqq.dst = htons(FWLOCALBUS | node);
#endif
	asyreq->pkt.mode.rreqq.tlrt = 0;
	if (read)
		asyreq->pkt.mode.rreqq.tcode = FWTCODE_RREQQ;
	else
		asyreq->pkt.mode.rreqq.tcode = FWTCODE_WREQQ;

	asyreq->pkt.mode.rreqq.dest_hi = htons(0xffff);
	asyreq->pkt.mode.rreqq.dest_lo = htonl(addr_lo);

	qld = (u_int32_t *)&asyreq->pkt;
	if (!read)
		asyreq->pkt.mode.wreqq.data = data;

	if (ioctl(fd, FW_ASYREQ, asyreq) < 0) {
       		err(1, "ioctl");
	}
	res = qld[3];
	free(asyreq);
	if (read)
		return ntohl(res);
	else
		return 0;
}
void
send_phy_config(int fd, int root_node, int gap_count)
{
        struct fw_asyreq *asyreq;

	asyreq = (struct fw_asyreq *)malloc(sizeof(struct fw_asyreq_t) + 12);
	asyreq->req.len = 12;
	asyreq->req.type = FWASREQNODE;
	asyreq->pkt.mode.ld[0] = 0;
	asyreq->pkt.mode.ld[1] = 0;
	asyreq->pkt.mode.common.tcode = FWTCODE_PHY;
	if (root_node >= 0)
		asyreq->pkt.mode.ld[1] |= htonl((root_node & 0x3f) << 24 | 1 << 23);
	if (gap_count >= 0)
		asyreq->pkt.mode.ld[1] |= htonl(1 << 22 | (gap_count & 0x3f) << 16);
	asyreq->pkt.mode.ld[2] = ~asyreq->pkt.mode.ld[1];

	printf("send phy_config root_node=%d gap_count=%d\n",
						root_node, gap_count);

	if (ioctl(fd, FW_ASYREQ, asyreq) < 0) {
       		err(1, "ioctl");
	}
}

void
set_pri_req(int fd, int pri_req)
{
	struct fw_devlstreq data;
	u_int32_t max, reg, old;
	int i;

	get_num_of_dev(fd, &data);
#define BUGET_REG 0xf0000218
	for (i = 0; i < data.n; i++) {
		if (!data.status[i])
			continue;
		reg = read_write_quad(fd, data.eui[i], BUGET_REG, 1, 0);
		printf("%d %08x:%08x, %08x",
			data.dst[i], data.eui[i].hi, data.eui[i].lo, reg);
		if (reg > 0 && pri_req >= 0) {
			old = (reg & 0x3f);
			max = (reg & 0x3f00) >> 8;
			if (pri_req > max)
				pri_req =  max;
			printf(" 0x%x -> 0x%x\n", old, pri_req);
			read_write_quad(fd, data.eui[i], BUGET_REG, 0, pri_req);
		} else {
			printf("\n");
		}
	}
}

void parse_bus_info_block(u_int32_t *p, int info_len)
{
	int i;

	for (i = 0; i < info_len; i++) {
		printf("bus_info%d: 0x%08x\n", i, *p++);
	}
}

int
get_crom(int fd, int node, void *crom_buf, int len)
{
	struct fw_crom_buf buf;
	int i, error;
	struct fw_devlstreq data;

	get_num_of_dev(fd, &data);

	for (i = 0; i < data.n; i++) {
		if (data.dst[i] == node && data.eui[i].lo != 0)
			break;
	}
	if (i != data.n) {
		buf.eui = data.eui[i];
	} else {
		err(1, "no such node: %d\n", node);
	}

	buf.len = len;
	buf.ptr = crom_buf;
	if ((error = ioctl(fd, FW_GCROM, &buf)) < 0) {
       		err(1, "ioctl");
	}
	return error;
}

show_crom(u_int32_t *crom_buf)
{
	int i;
	struct crom_context cc;
	char *desc, info[256];
	static char *key_types = "ICLD";
	struct csrreg *reg;
	struct csrdirectory *dir;
	struct csrhdr *hdr;

	printf("first quad: 0x%08x\n", *crom_buf);
	hdr = (struct csrhdr *)crom_buf;
	if (hdr->info_len == 1) {
		/* minimum ROM */
		struct csrreg *reg;
		reg = (struct csrreg *)hdr;
		printf("verndor ID: 0x%06x\n",  reg->val);
		return;
	}
	printf("len: %d\n", hdr->crc_len);
	parse_bus_info_block(crom_buf+1, hdr->info_len);

	crom_init_context(&cc, crom_buf);
	dir = cc.stack[0].dir;
	printf("root_directory: len=0x%04x(%d) crc=0x%04x\n",
			dir->crc_len, dir->crc_len, dir->crc);
	while (cc.depth >= 0) {
		desc = crom_desc(&cc, info, sizeof(info));
		reg = crom_get(&cc);
		for (i = 0; i < cc.depth; i++)
			printf("\t");
		printf("%02x(%c:%02x) %06x %s: %s\n",
			reg->key,
			key_types[(reg->key & CSRTYPE_MASK)>>6],
			reg->key & CSRKEY_MASK, reg->val,
			desc, info);
		crom_next(&cc);
	}
}

#define DUMP_FORMAT	"%08x %08x %08x %08x %08x %08x %08x %08x\n"

dump_crom(u_int32_t *p)
{
	int len=1024, i;

	for (i = 0; i < len/(4*8); i ++) {
		printf(DUMP_FORMAT,
			p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
		p += 8;
	}
}

load_crom(char *filename, u_int32_t *p)
{
	FILE *file;
	int len=1024, i;

	if ((file = fopen(filename, "r")) == NULL)
		err(1, "load_crom");
	for (i = 0; i < len/(4*8); i ++) {
		fscanf(file, DUMP_FORMAT,
			p, p+1, p+2, p+3, p+4, p+5, p+6, p+7);
		p += 8;
	}
}

static void
show_topology_map(int fd)
{
	struct fw_topology_map *tmap;
	union fw_self_id sid;
	int i;
	static char *port_status[] = {" ", "-", "P", "C"};
	static char *pwr_class[] = {" 0W", "15W", "30W", "45W",
					"-1W", "-2W", "-5W", "-9W"};
	static char *speed[] = {"S100", "S200", "S400", "S800"};
	tmap = malloc(sizeof(struct fw_topology_map));
	if (tmap == NULL)
		return;
	if (ioctl(fd, FW_GTPMAP, tmap) < 0) {
       		err(1, "ioctl");
	}
	printf("crc_len: %d generation:%d node_count:%d sid_count:%d\n",
		tmap->crc_len, tmap->generation,
		tmap->node_count, tmap->self_id_count);
	printf("id link gap_cnt speed delay cIRM power port0 port1 port2"
		" ini more\n");
	for (i = 0; i < tmap->crc_len - 2; i++) {
		sid = tmap->self_id[i];
		if (sid.p0.sequel) {
			printf("%02d sequel packet\n", sid.p0.phy_id);
			continue;
		}
		printf("%02d   %2d      %d  %4s     %d    %d   %3s"
				"     %s     %s     %s   %d    %d\n",
			sid.p0.phy_id,
			sid.p0.link_active,
			sid.p0.gap_count,
			speed[sid.p0.phy_speed],
			sid.p0.phy_delay,
			sid.p0.contender,
			pwr_class[sid.p0.power_class],
			port_status[sid.p0.port0],
			port_status[sid.p0.port1],
			port_status[sid.p0.port2],
			sid.p0.initiated_reset,
			sid.p0.more_packets
		);
	}
	free(tmap);
}

static void
show_speed_map(int fd)
{
	struct fw_speed_map *smap;
	int i,j;

	smap = malloc(sizeof(struct fw_speed_map));
	if (smap == NULL)
		return;
	if (ioctl(fd, FW_GSPMAP, &smap) < 0) {
       		err(1, "ioctl");
	}
	printf("crc_len: %d generation:%d\n", smap->crc_len, smap->generation);
	for (i = 0; i < 64; i ++) {
		for (j = 0; j < 64; j ++)
			printf("%d", smap->speed[i][j]);
		printf("\n");
	}
	free(smap);
}

int
main(int argc, char **argv)
{
	char devname[] = "/dev/fw1";
	u_int32_t crom_buf[1024/4];
	int fd, tmp, ch, len=1024;

	if ((fd = open(devname, O_RDWR)) < 0)
		err(1, "open");

	if (argc < 2) {
		list_dev(fd);
		usage();
	}

	while ((ch = getopt(argc, argv, "g:b:rtsc:d:l:")) != -1)
		switch(ch) {
		case 'g':
			/* gap count */
			tmp = strtol(optarg, NULL, 0);
			send_phy_config(fd, -1, tmp);
			break;
		case 'b':
			tmp = strtol(optarg, NULL, 0);
			set_pri_req(fd, tmp);
			break;
		case 'r':
			if(ioctl(fd, FW_IBUSRST, &tmp) < 0)
                       		err(1, "ioctl");
			break;
		case 't':
			show_topology_map(fd);
			break;
		case 's':
			show_speed_map(fd);
			break;
		case 'c':
			tmp = strtol(optarg, NULL, 0);
			get_crom(fd, tmp, crom_buf, len);
			show_crom(crom_buf);
			break;
		case 'd':
			tmp = strtol(optarg, NULL, 0);
			get_crom(fd, tmp, crom_buf, len);
			dump_crom(crom_buf);
			break;
		case 'l':
			load_crom(optarg, crom_buf);
			show_crom(crom_buf);
			break;
		default:
			usage();
		}
	return 0;
}
