/***********************************************************
		Copyright IBM Corporation 1987

                      All Rights Reserved

Permission to use, copy, modify, and distribute this software and its 
documentation for any purpose and without fee is hereby granted, 
provided that the above copyright notice appear in all copies and that
both that copyright notice and this permission notice appear in 
supporting documentation, and that the name of IBM not be
used in advertising or publicity pertaining to distribution of the
software without specific, written prior permission.  

IBM DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING
ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO EVENT SHALL
IBM BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR
ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
SOFTWARE.

******************************************************************/

/*
 * ARGO Project, Computer Sciences Dept., University of Wisconsin - Madison
 */
/* 
 * ARGO TP
 *
 * $Header: tp_trace.c,v 5.3 88/11/18 17:29:14 nhall Exp $
 * $Source: /usr/argo/sys/netiso/RCS/tp_trace.c,v $
 *
 * The whole protocol trace module.
 * We keep a circular buffer of trace structures, which are big
 * unions of different structures we might want to see.
 * Unfortunately this gets too big pretty easily. Pcbs were removed
 * from the tracing when the kernel got too big to boot.
 */

#ifndef lint
static char *rcsid = "$Header: tp_trace.c,v 5.3 88/11/18 17:29:14 nhall Exp $";
#endif lint

#define TP_TRACEFILE

#include "param.h"
#include "systm.h"
#include "mbuf.h"
#include "socket.h"
#include "types.h"
#include "time.h"

#include "tp_param.h"
#include "tp_timer.h"
#include "tp_stat.h"
#include "tp_param.h"
#include "tp_ip.h"
#include "tp_pcb.h"
#include "tp_tpdu.h"
#include "argo_debug.h"
#include "tp_trace.h"

#ifdef TPPT
static tp_seq = 0;
u_char tp_traceflags[128];

/*
 * The argument tpcb is the obvious.
 * event here is just the type of trace event - TPPTmisc, etc.
 * The rest of the arguments have different uses depending
 * on the type of trace event.
 */
/*ARGSUSED*/
/*VARARGS*/

void
tpTrace(tpcb, event, arg, src, len, arg4, arg5)
	struct tp_pcb	*tpcb;
	u_int 			event, arg;
	u_int	 		src;
	u_int	 		len; 
	u_int	 		arg4;
	u_int	 		arg5;
{
	register struct tp_Trace *tp;

	tp = &tp_Trace[tp_Tracen++];
	tp_Tracen %= TPTRACEN;

	tp->tpt_event = event;
	tp->tpt_tseq = tp_seq++;
	tp->tpt_arg = arg;
	if(tpcb)
		tp->tpt_arg2 = tpcb->tp_lref;
	bcopy( (caddr_t)&time, (caddr_t)&tp->tpt_time, sizeof(struct timeval) );

	switch(event) {

	case TPPTertpdu:
		bcopy((caddr_t)src, (caddr_t)&tp->tpt_ertpdu,
			(unsigned)MIN((int)len, sizeof(struct tp_Trace)));
		break;

	case TPPTusrreq:
	case TPPTmisc:

		/* arg is a string */
		bcopy((caddr_t)arg, (caddr_t)tp->tpt_str, 
			(unsigned)MIN(1+strlen((caddr_t) arg), TPTRACE_STRLEN));
		tp->tpt_m2 = src; 
		tp->tpt_m3 = len;
		tp->tpt_m4 = arg4;
		tp->tpt_m1 = arg5;
		break;

	case TPPTgotXack: 
	case TPPTXack: 
	case TPPTsendack: 
	case TPPTgotack: 
	case TPPTack: 
	case TPPTindicate: 
	default:
	case TPPTdriver: 
		tp->tpt_m2 = arg; 
		tp->tpt_m3 = src;
		tp->tpt_m4 = len;
		tp->tpt_m5 = arg4;
		tp->tpt_m1 = arg5; 
		break;
	case TPPTparam:
		bcopy((caddr_t)src, (caddr_t)&tp->tpt_param, sizeof(struct tp_param));
		break;
	case TPPTref:
		bcopy((caddr_t)src, (caddr_t)&tp->tpt_ref, sizeof(struct tp_ref));
		break;

	case TPPTtpduin:
	case TPPTtpduout:
		tp->tpt_arg2 = arg4;
		bcopy((caddr_t)src, (caddr_t)&tp->tpt_tpdu,
		      (unsigned)MIN((int)len, sizeof(struct tp_Trace)));
		break;
	}
}
#endif TPPT
