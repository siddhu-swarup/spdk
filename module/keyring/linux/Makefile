# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2024 Intel Corporation.  All rights reserved.

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 2
SO_MINOR := 0

C_SRCS = keyring.c keyring_rpc.c
LIBNAME = keyring_linux

LOCAL_SYS_LIBS = -lkeyutils

SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
