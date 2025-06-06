/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2015 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_QUEUE_H
#define SPDK_QUEUE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/cdefs.h>
#include <sys/queue.h>

/*
 * The SPDK NVMe driver was originally ported from FreeBSD, which makes
 *  use of features in FreeBSD's queue.h that do not exist on Linux.
 *  Include a header with these additional features on Linux only.
 */
#ifdef __linux__
#include "spdk/queue_extras.h"
#endif

/*
 * scan-build can't follow double pointers in queues and often assumes
 * that removed elements are still on the list. We redefine TAILQ_REMOVE
 * with extra asserts to silence it.
 */
#ifdef __clang_analyzer__
#undef TAILQ_REMOVE
#define TAILQ_REMOVE(head, elm, field) do {				\
	__typeof__(elm) _elm;						\
	if (((elm)->field.tqe_next) != NULL)				\
		(elm)->field.tqe_next->field.tqe_prev =			\
		    (elm)->field.tqe_prev;				\
	else								\
		(head)->tqh_last = (elm)->field.tqe_prev;		\
	*(elm)->field.tqe_prev = (elm)->field.tqe_next;			\
	/* make sure the removed elm is not on the list anymore */	\
	TAILQ_FOREACH(_elm, head, field) {				\
		assert(_elm != elm);					\
	}								\
} while (0)
#endif

/*
 * Check if an entry is on any TAILQ list.
 *
 * Should only be used on zero initalized entries or after
 * calling TAILQ_REMOVE_CLEAR() or TAILQ_ENTRY_CLEAR().
 */
#define TAILQ_ENTRY_ENQUEUED(elm, field)				\
    ((elm)->field.tqe_prev != NULL)

/*
 * Check if an entry is not on any TAILQ list.
 *
 * Should only be used on zero initalized entries or after
 * calling TAILQ_REMOVE_CLEAR() or TAILQ_ENTRY_CLEAR().
 */
#define TAILQ_ENTRY_NOT_ENQUEUED(elm, field)				\
    (!TAILQ_ENTRY_ENQUEUED(elm, field))

/*
 * Mark an entry as absent from any TAILQ list.
 *
 * Should be called once after TAILQ_REMOVE(), or on entries
 * that were not initalized to zero.
 */
#define TAILQ_ENTRY_CLEAR(elm, field) do {				\
	/* Ensure the entry was on a list before clearing */		\
	assert(TAILQ_ENTRY_ENQUEUED(elm, field));			\
	(elm)->field.tqe_prev = NULL;					\
} while (0)

/*
 * Remove entry from TAILQ list and mark it as absent.
 */
#define TAILQ_REMOVE_CLEAR(head, elm, field) do {			\
	TAILQ_REMOVE(head, elm, field);					\
	TAILQ_ENTRY_CLEAR(elm, field);					\
} while (0)

#ifdef __cplusplus
}
#endif

#endif
