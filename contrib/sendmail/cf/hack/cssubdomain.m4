divert(-1)
#
# Copyright (c) 1998, 1999 Sendmail, Inc. and its suppliers.
#	All rights reserved.
# Copyright (c) 1983 Eric P. Allman.  All rights reserved.
# Copyright (c) 1988, 1993
#	The Regents of the University of California.  All rights reserved.
#
# By using this file, you agree to the terms and conditions set
# forth in the LICENSE file which can be found at the top level of
# the sendmail distribution.
#
#
divert(0)
VERSIONID(`$Id: cssubdomain.m4,v 1.1.1.3 2000/08/12 21:55:37 gshapiro Exp $')

divert(2)
# find possible (old & new) versions of our name via short circuit hack
# (this code should exist ONLY during the transition from .Berkeley.EDU
#  names to .CS.Berkeley.EDU names -- probably not more than a few months)
R$* < @ $=w .CS.Berkeley.EDU > $*	$: $1 < @ $j > $3
R$* < @ $=w .Berkeley.EDU> $*		$: $1 < @ $j > $3
divert(0)
