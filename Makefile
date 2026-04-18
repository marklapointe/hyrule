# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Mark LaPointe <mark@cloudbsd.org>

KMOD=	hyrule
SRCS=	hyrule.c hyrule_map.c hyrule_input.c

.include <bsd.kmod.mk>
