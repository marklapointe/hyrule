# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Mark LaPointe <mark@cloudbsd.org>

KMOD=	hyrule
SRCS=	hyrule.c
MAN=	hyrule.4

.include <bsd.kmod.mk>
.include <bsd.man.mk>

install: maninstall
