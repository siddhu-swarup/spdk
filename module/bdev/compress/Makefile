#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 7
SO_MINOR := 0

CFLAGS += -I$(SPDK_ROOT_DIR)/lib/bdev/

C_SRCS = vbdev_compress.c vbdev_compress_rpc.c
LIBNAME = bdev_compress
CFLAGS += $(ENV_CFLAGS)

SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
