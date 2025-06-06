/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2021-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk_internal/cunit.h"
#include "spdk/blob.h"
#include "spdk/string.h"

#include "common/lib/ut_multithread.c"
#include "../bs_dev_common.c"
#include "thread/thread.c"
#include "ext_dev.c"
#include "blob/blobstore.c"
#include "blob/request.c"
#include "blob/zeroes.c"
#include "blob/blob_bs_dev.c"
#include "esnap_dev.c"
#define BLOCKLEN DEV_BUFFER_BLOCKLEN

struct spdk_blob_store *g_bs;
spdk_blob_id g_blobid;
struct spdk_blob *g_blob, *g_blob2;
int g_bserrno, g_bserrno2;
struct spdk_xattr_names *g_names;
int g_done;
char *g_xattr_names[] = {"first", "second", "third"};
char *g_xattr_values[] = {"one", "two", "three"};
uint64_t g_ctx = 1729;
bool g_use_extent_table = false;
uint64_t g_copied_clusters_count = 0;

struct spdk_bs_super_block_ver1 {
	uint8_t		signature[8];
	uint32_t        version;
	uint32_t        length;
	uint32_t	clean; /* If there was a clean shutdown, this is 1. */
	spdk_blob_id	super_blob;

	uint32_t	cluster_size; /* In bytes */

	uint32_t	used_page_mask_start; /* Offset from beginning of disk, in pages */
	uint32_t	used_page_mask_len; /* Count, in pages */

	uint32_t	used_cluster_mask_start; /* Offset from beginning of disk, in pages */
	uint32_t	used_cluster_mask_len; /* Count, in pages */

	uint32_t	md_start; /* Offset from beginning of disk, in pages */
	uint32_t	md_len; /* Count, in pages */

	uint8_t		reserved[4036];
	uint32_t	crc;
} __attribute__((packed));
SPDK_STATIC_ASSERT(sizeof(struct spdk_bs_super_block_ver1) == 0x1000, "Invalid super block size");

static struct spdk_blob *ut_blob_create_and_open(struct spdk_blob_store *bs,
		struct spdk_blob_opts *blob_opts);
static void ut_blob_close_and_delete(struct spdk_blob_store *bs, struct spdk_blob *blob);
static void suite_blob_setup(void);
static void suite_blob_cleanup(void);

DEFINE_STUB(spdk_memory_domain_memzero, int, (struct spdk_memory_domain *src_domain,
		void *src_domain_ctx, struct iovec *iov, uint32_t iovcnt, void (*cpl_cb)(void *, int),
		void *cpl_cb_arg), 0);

static bool
is_esnap_clone(struct spdk_blob *_blob, const void *id, size_t id_len)
{
	const void *val = NULL;
	size_t len = 0;
	bool c0, c1, c2, c3;

	CU_ASSERT(blob_get_xattr_value(_blob, BLOB_EXTERNAL_SNAPSHOT_ID, &val, &len,
				       true) == 0);
	CU_ASSERT((c0 = (len == id_len)));
	CU_ASSERT((c1 = (val != NULL && memcmp(val, id, len) == 0)));
	CU_ASSERT((c2 = !!(_blob->invalid_flags & SPDK_BLOB_EXTERNAL_SNAPSHOT)));
	CU_ASSERT((c3 = (_blob->parent_id == SPDK_BLOBID_EXTERNAL_SNAPSHOT)));

	return c0 && c1 && c2 && c3;
}

static bool
is_not_esnap_clone(struct spdk_blob *_blob)
{
	const void *val = NULL;
	size_t len = 0;
	bool c1, c2, c3, c4;

	CU_ASSERT((c1 = (blob_get_xattr_value(_blob, BLOB_EXTERNAL_SNAPSHOT_ID, &val, &len,
					      true) == -ENOENT)));
	CU_ASSERT((c2 = (val == NULL)));
	CU_ASSERT((c3 = ((_blob->invalid_flags & SPDK_BLOB_EXTERNAL_SNAPSHOT) == 0)));
	CU_ASSERT((c4 = (_blob->parent_id != SPDK_BLOBID_EXTERNAL_SNAPSHOT)));

	return c1 && c2 && c3 && c4;
}

#define UT_ASSERT_IS_ESNAP_CLONE(_blob, _id, _len) CU_ASSERT(is_esnap_clone(_blob, _id, _len))
#define UT_ASSERT_IS_NOT_ESNAP_CLONE(_blob) CU_ASSERT(is_not_esnap_clone(_blob))

static void
_get_xattr_value(void *arg, const char *name,
		 const void **value, size_t *value_len)
{
	uint64_t i;

	SPDK_CU_ASSERT_FATAL(value_len != NULL);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(arg == &g_ctx);

	for (i = 0; i < sizeof(g_xattr_names); i++) {
		if (!strcmp(name, g_xattr_names[i])) {
			*value_len = strlen(g_xattr_values[i]);
			*value = g_xattr_values[i];
			break;
		}
	}
}

static void
_get_xattr_value_null(void *arg, const char *name,
		      const void **value, size_t *value_len)
{
	SPDK_CU_ASSERT_FATAL(value_len != NULL);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(arg == NULL);

	*value_len = 0;
	*value = NULL;
}

static int
_get_snapshots_count(struct spdk_blob_store *bs)
{
	struct spdk_blob_list *snapshot = NULL;
	int count = 0;

	TAILQ_FOREACH(snapshot, &bs->snapshots, link) {
		count += 1;
	}

	return count;
}

static void
ut_spdk_blob_opts_init(struct spdk_blob_opts *opts)
{
	spdk_blob_opts_init(opts, sizeof(*opts));
	opts->use_extent_table = g_use_extent_table;
}

static void
bs_op_complete(void *cb_arg, int bserrno)
{
	g_bserrno = bserrno;
}

static void
bs_op_with_handle_complete(void *cb_arg, struct spdk_blob_store *bs,
			   int bserrno)
{
	g_bs = bs;
	g_bserrno = bserrno;
}

static void
blob_op_complete(void *cb_arg, int bserrno)
{
	if (cb_arg != NULL) {
		int *errp = cb_arg;

		*errp = bserrno;
	}
	g_bserrno = bserrno;
}

static void
blob_op_with_id_complete(void *cb_arg, spdk_blob_id blobid, int bserrno)
{
	g_blobid = blobid;
	g_bserrno = bserrno;
}

static void
blob_op_with_handle_complete(void *cb_arg, struct spdk_blob *blb, int bserrno)
{
	g_blob = blb;
	g_bserrno = bserrno;
}

static void
blob_op_with_handle_complete2(void *cb_arg, struct spdk_blob *blob, int bserrno)
{
	if (g_blob == NULL) {
		g_blob = blob;
		g_bserrno = bserrno;
	} else {
		g_blob2 = blob;
		g_bserrno2 = bserrno;
	}
}

static void
blob_shallow_copy_status_cb(uint64_t copied_clusters, void *cb_arg)
{
	g_copied_clusters_count = copied_clusters;
}

static void
ut_bs_reload(struct spdk_blob_store **bs, struct spdk_bs_opts *opts)
{
	struct spdk_bs_dev *dev;

	/* Unload the blob store */
	spdk_bs_unload(*bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	dev = init_dev();
	/* Load an existing blob store */
	spdk_bs_load(dev, opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	*bs = g_bs;

	g_bserrno = -1;
}

static void
ut_bs_dirty_load(struct spdk_blob_store **bs, struct spdk_bs_opts *opts)
{
	struct spdk_bs_dev *dev;

	/* Dirty shutdown */
	bs_free(*bs);

	dev = init_dev();
	/* Load an existing blob store */
	spdk_bs_load(dev, opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	*bs = g_bs;

	g_bserrno = -1;
}

static void
blob_init(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;

	dev = init_dev();

	/* should fail for an unsupported blocklen */
	dev->blocklen = 500;
	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EINVAL);

	dev = init_dev();
	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_super(void)
{
	struct spdk_blob_store *bs = g_bs;
	spdk_blob_id blobid;
	struct spdk_blob_opts blob_opts;

	/* Get the super blob without having set one */
	spdk_bs_get_super(bs, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -ENOENT);
	CU_ASSERT(g_blobid == SPDK_BLOBID_INVALID);

	/* Create a blob */
	ut_spdk_blob_opts_init(&blob_opts);
	spdk_bs_create_blob_ext(bs, &blob_opts, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid !=  SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	/* Set the blob as the super blob */
	spdk_bs_set_super(bs, blobid, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Get the super blob */
	spdk_bs_get_super(bs, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(blobid == g_blobid);
}

static void
blob_open(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob;
	struct spdk_blob_opts blob_opts;
	spdk_blob_id blobid, blobid2;

	ut_spdk_blob_opts_init(&blob_opts);
	spdk_bs_create_blob_ext(bs, &blob_opts, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	blobid2 = spdk_blob_get_id(blob);
	CU_ASSERT(blobid == blobid2);

	/* Try to open file again.  It should return success. */
	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(blob == g_blob);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/*
	 * Close the file a second time, releasing the second reference.  This
	 *  should succeed.
	 */
	blob = g_blob;
	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/*
	 * Try to open file again.  It should succeed.  This tests the case
	 *  where the file is opened, closed, then re-opened again.
	 */
	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;
	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Try to open file twice in succession.  This should return the same
	 * blob object.
	 */
	g_blob = NULL;
	g_blob2 = NULL;
	g_bserrno = -1;
	g_bserrno2 = -1;
	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete2, NULL);
	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete2, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_bserrno2 == 0);
	CU_ASSERT(g_blob != NULL);
	CU_ASSERT(g_blob2 != NULL);
	CU_ASSERT(g_blob == g_blob2);

	g_bserrno = -1;
	spdk_blob_close(g_blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	ut_blob_close_and_delete(bs, g_blob);
}

static void
blob_create(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob;
	struct spdk_blob_opts opts;
	spdk_blob_id blobid;

	/* Create blob with 10 clusters */

	ut_spdk_blob_opts_init(&opts);
	opts.num_clusters = 10;

	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 10);
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 10);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Create blob with 0 clusters */

	ut_spdk_blob_opts_init(&opts);
	opts.num_clusters = 0;

	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 0);
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Create blob with default options (opts == NULL) */

	spdk_bs_create_blob_ext(bs, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 0);
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Try to create blob with size larger than blobstore */

	ut_spdk_blob_opts_init(&opts);
	opts.num_clusters = bs->total_clusters + 1;

	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -ENOSPC);
}

static void
blob_create_zero_extent(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob;
	spdk_blob_id blobid;

	/* Create blob with default options (opts == NULL) */
	spdk_bs_create_blob_ext(bs, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 0);
	CU_ASSERT(blob->extent_table_found == true);
	CU_ASSERT(blob->active.extent_pages_array_size == 0);
	CU_ASSERT(blob->active.extent_pages == NULL);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Create blob with NULL internal options  */
	bs_create_blob(bs, NULL, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;
	CU_ASSERT(TAILQ_FIRST(&blob->xattrs_internal) == NULL);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 0);
	CU_ASSERT(blob->extent_table_found == true);
	CU_ASSERT(blob->active.extent_pages_array_size == 0);
	CU_ASSERT(blob->active.extent_pages == NULL);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
}

/*
 * Create and delete one blob in a loop over and over again.  This helps ensure
 * that the internal bit masks tracking used clusters and md_pages are being
 * tracked correctly.
 */
static void
blob_create_loop(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob_opts opts;
	uint32_t i, loop_count;

	loop_count = 4 * spdk_max(spdk_bit_array_capacity(bs->used_md_pages),
				  spdk_bit_pool_capacity(bs->used_clusters));

	for (i = 0; i < loop_count; i++) {
		ut_spdk_blob_opts_init(&opts);
		opts.num_clusters = 1;
		g_bserrno = -1;
		g_blobid = SPDK_BLOBID_INVALID;
		spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
		spdk_bs_delete_blob(bs, g_blobid, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
	}
}

static void
blob_create_fail(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob_opts opts;
	spdk_blob_id blobid;
	uint32_t used_blobids_count = spdk_bit_array_count_set(bs->used_blobids);
	uint32_t used_md_pages_count = spdk_bit_array_count_set(bs->used_md_pages);

	/* NULL callback */
	ut_spdk_blob_opts_init(&opts);
	opts.xattrs.names = g_xattr_names;
	opts.xattrs.get_value = NULL;
	opts.xattrs.count = 1;
	opts.xattrs.ctx = &g_ctx;

	blobid = spdk_bit_array_find_first_clear(bs->used_md_pages, 0);
	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EINVAL);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	CU_ASSERT(spdk_bit_array_count_set(bs->used_blobids) == used_blobids_count);
	CU_ASSERT(spdk_bit_array_count_set(bs->used_md_pages) == used_md_pages_count);

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -ENOENT);
	SPDK_CU_ASSERT_FATAL(g_blob == NULL);

	ut_bs_reload(&bs, NULL);
	CU_ASSERT(spdk_bit_array_count_set(bs->used_blobids) == used_blobids_count);
	CU_ASSERT(spdk_bit_array_count_set(bs->used_md_pages) == used_md_pages_count);

	spdk_bs_iter_first(bs, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_blob == NULL);
	CU_ASSERT(g_bserrno == -ENOENT);
}

static void
blob_create_internal(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob;
	struct spdk_blob_opts opts;
	struct spdk_blob_xattr_opts internal_xattrs;
	const void *value;
	size_t value_len;
	spdk_blob_id blobid;
	int rc;

	/* Create blob with custom xattrs */

	ut_spdk_blob_opts_init(&opts);
	blob_xattrs_init(&internal_xattrs);
	internal_xattrs.count = 3;
	internal_xattrs.names = g_xattr_names;
	internal_xattrs.get_value = _get_xattr_value;
	internal_xattrs.ctx = &g_ctx;

	bs_create_blob(bs, &opts, &internal_xattrs, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	rc = blob_get_xattr_value(blob, g_xattr_names[0], &value, &value_len, true);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(value_len == strlen(g_xattr_values[0]));
	CU_ASSERT_NSTRING_EQUAL_FATAL(value, g_xattr_values[0], value_len);

	rc = blob_get_xattr_value(blob, g_xattr_names[1], &value, &value_len, true);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(value_len == strlen(g_xattr_values[1]));
	CU_ASSERT_NSTRING_EQUAL((char *)value, g_xattr_values[1], value_len);

	rc = blob_get_xattr_value(blob, g_xattr_names[2], &value, &value_len, true);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(value_len == strlen(g_xattr_values[2]));
	CU_ASSERT_NSTRING_EQUAL((char *)value, g_xattr_values[2], value_len);

	rc = spdk_blob_get_xattr_value(blob, g_xattr_names[0], &value, &value_len);
	CU_ASSERT(rc != 0);

	rc = spdk_blob_get_xattr_value(blob, g_xattr_names[1], &value, &value_len);
	CU_ASSERT(rc != 0);

	rc = spdk_blob_get_xattr_value(blob, g_xattr_names[2], &value, &value_len);
	CU_ASSERT(rc != 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Create blob with NULL internal options  */

	bs_create_blob(bs, NULL, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	CU_ASSERT(TAILQ_FIRST(&g_blob->xattrs_internal) == NULL);
	CU_ASSERT(spdk_blob_get_num_clusters(g_blob) == 0);

	blob = g_blob;

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
}

static void
blob_thin_provision(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	struct spdk_blob_opts opts;
	struct spdk_bs_opts bs_opts;
	spdk_blob_id blobid;

	dev = init_dev();
	spdk_bs_opts_init(&bs_opts, sizeof(bs_opts));
	snprintf(bs_opts.bstype.bstype, sizeof(bs_opts.bstype.bstype), "TESTTYPE");

	/* Initialize a new blob store */
	spdk_bs_init(dev, &bs_opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	bs = g_bs;

	/* Create blob with thin provisioning enabled */

	ut_spdk_blob_opts_init(&opts);
	opts.thin_provision = true;
	opts.num_clusters = 10;

	blob = ut_blob_create_and_open(bs, &opts);
	blobid = spdk_blob_get_id(blob);
	CU_ASSERT(blob->invalid_flags & SPDK_BLOB_THIN_PROV);
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 0);
	/* In thin provisioning with num_clusters is set, if not using the
	 * extent table, there is no allocation. If extent table is used,
	 * there is related allocation happened. */
	if (blob->extent_table_found == true) {
		CU_ASSERT(blob->active.extent_pages_array_size > 0);
		CU_ASSERT(blob->active.extent_pages != NULL);
	} else {
		CU_ASSERT(blob->active.extent_pages_array_size == 0);
		CU_ASSERT(blob->active.extent_pages == NULL);
	}

	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	/* Do not shut down cleanly.  This makes sure that when we load again
	 *  and try to recover a valid used_cluster map, that blobstore will
	 *  ignore clusters with index 0 since these are unallocated clusters.
	 */
	ut_bs_dirty_load(&bs, &bs_opts);

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;
	CU_ASSERT(blob->invalid_flags & SPDK_BLOB_THIN_PROV);
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 0);

	ut_blob_close_and_delete(bs, blob);

	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_snapshot(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob;
	struct spdk_blob *snapshot, *snapshot2;
	struct spdk_blob_bs_dev *blob_bs_dev;
	struct spdk_blob_opts opts;
	struct spdk_blob_xattr_opts xattrs;
	spdk_blob_id blobid;
	spdk_blob_id snapshotid;
	spdk_blob_id snapshotid2;
	const void *value;
	size_t value_len;
	int rc;
	spdk_blob_id ids[2];
	size_t count;

	/* Create blob with 10 clusters */
	ut_spdk_blob_opts_init(&opts);
	opts.num_clusters = 10;

	blob = ut_blob_create_and_open(bs, &opts);
	blobid = spdk_blob_get_id(blob);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 10);

	/* Create snapshot from blob */
	CU_ASSERT_EQUAL(_get_snapshots_count(bs), 0);
	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	CU_ASSERT_EQUAL(_get_snapshots_count(bs), 1);
	snapshotid = g_blobid;

	spdk_bs_open_blob(bs, snapshotid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot = g_blob;
	CU_ASSERT(snapshot->data_ro == true);
	CU_ASSERT(snapshot->md_ro == true);
	CU_ASSERT(spdk_blob_get_num_clusters(snapshot) == 10);
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(snapshot) == 10);

	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 10);
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 0);
	CU_ASSERT(blob->invalid_flags & SPDK_BLOB_THIN_PROV);
	CU_ASSERT(spdk_mem_all_zero(blob->active.clusters,
				    blob->active.num_clusters * sizeof(blob->active.clusters[0])));

	/* Try to create snapshot from clone with xattrs */
	xattrs.names = g_xattr_names;
	xattrs.get_value = _get_xattr_value;
	xattrs.count = 3;
	xattrs.ctx = &g_ctx;
	spdk_bs_create_snapshot(bs, blobid, &xattrs, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	CU_ASSERT_EQUAL(_get_snapshots_count(bs), 2);
	snapshotid2 = g_blobid;

	spdk_bs_open_blob(bs, snapshotid2, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot2 = g_blob;
	CU_ASSERT(snapshot2->data_ro == true);
	CU_ASSERT(snapshot2->md_ro == true);
	CU_ASSERT(spdk_blob_get_num_clusters(snapshot2) == 10);
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(snapshot2) == 0);

	/* Confirm that blob is backed by snapshot2 and snapshot2 is backed by snapshot */
	CU_ASSERT(snapshot->back_bs_dev == NULL);
	SPDK_CU_ASSERT_FATAL(blob->back_bs_dev != NULL);
	SPDK_CU_ASSERT_FATAL(snapshot2->back_bs_dev != NULL);

	blob_bs_dev = (struct spdk_blob_bs_dev *)blob->back_bs_dev;
	CU_ASSERT(blob_bs_dev->blob == snapshot2);

	blob_bs_dev = (struct spdk_blob_bs_dev *)snapshot2->back_bs_dev;
	CU_ASSERT(blob_bs_dev->blob == snapshot);

	rc = spdk_blob_get_xattr_value(snapshot2, g_xattr_names[0], &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(value_len == strlen(g_xattr_values[0]));
	CU_ASSERT_NSTRING_EQUAL_FATAL(value, g_xattr_values[0], value_len);

	rc = spdk_blob_get_xattr_value(snapshot2, g_xattr_names[1], &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(value_len == strlen(g_xattr_values[1]));
	CU_ASSERT_NSTRING_EQUAL((char *)value, g_xattr_values[1], value_len);

	rc = spdk_blob_get_xattr_value(snapshot2, g_xattr_names[2], &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(value_len == strlen(g_xattr_values[2]));
	CU_ASSERT_NSTRING_EQUAL((char *)value, g_xattr_values[2], value_len);

	/* Confirm that blob is clone of snapshot2, and snapshot2 is clone of snapshot */
	count = 2;
	CU_ASSERT(spdk_blob_get_clones(bs, snapshotid2, ids, &count) == 0);
	CU_ASSERT(count == 1);
	CU_ASSERT(ids[0] == blobid);

	count = 2;
	CU_ASSERT(spdk_blob_get_clones(bs, snapshotid, ids, &count) == 0);
	CU_ASSERT(count == 1);
	CU_ASSERT(ids[0] == snapshotid2);

	/* Try to create snapshot from snapshot */
	spdk_bs_create_snapshot(bs, snapshotid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EINVAL);
	CU_ASSERT(g_blobid == SPDK_BLOBID_INVALID);
	CU_ASSERT_EQUAL(_get_snapshots_count(bs), 2);

	/* Delete blob and confirm that it is no longer on snapshot2 clone list */
	ut_blob_close_and_delete(bs, blob);
	count = 2;
	CU_ASSERT(spdk_blob_get_clones(bs, snapshotid2, ids, &count) == 0);
	CU_ASSERT(count == 0);

	/* Delete snapshot2 and confirm that it is no longer on snapshot clone list */
	ut_blob_close_and_delete(bs, snapshot2);
	CU_ASSERT_EQUAL(_get_snapshots_count(bs), 1);
	count = 2;
	CU_ASSERT(spdk_blob_get_clones(bs, snapshotid2, ids, &count) == 0);
	CU_ASSERT(count == 0);

	ut_blob_close_and_delete(bs, snapshot);
	CU_ASSERT_EQUAL(_get_snapshots_count(bs), 0);
}

static void
blob_snapshot_freeze_io(void)
{
	struct spdk_io_channel *channel;
	struct spdk_bs_channel *bs_channel;
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob;
	struct spdk_blob_opts opts;
	spdk_blob_id blobid;
	uint32_t num_of_pages = 10;
	uint8_t payload_read[num_of_pages * BLOCKLEN];
	uint8_t payload_write[num_of_pages * BLOCKLEN];
	uint8_t payload_zero[num_of_pages * BLOCKLEN];

	memset(payload_write, 0xE5, sizeof(payload_write));
	memset(payload_read, 0x00, sizeof(payload_read));
	memset(payload_zero, 0x00, sizeof(payload_zero));

	/* Test freeze I/O during snapshot */
	channel = spdk_bs_alloc_io_channel(bs);
	bs_channel = spdk_io_channel_get_ctx(channel);

	/* Create blob with 10 clusters */
	ut_spdk_blob_opts_init(&opts);
	opts.num_clusters = 10;
	opts.thin_provision = false;

	blob = ut_blob_create_and_open(bs, &opts);
	blobid = spdk_blob_get_id(blob);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 10);

	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);

	/* This is implementation specific.
	 * Flag 'frozen_io' is set in _spdk_bs_snapshot_freeze_cpl callback.
	 * Four async I/O operations happen before that. */
	poll_thread_times(0, 5);

	CU_ASSERT(TAILQ_EMPTY(&bs_channel->queued_io));

	/* Blob I/O should be frozen here */
	CU_ASSERT(blob->frozen_refcnt == 1);

	/* Write to the blob */
	spdk_blob_io_write(blob, channel, payload_write, 0, num_of_pages, blob_op_complete, NULL);

	/* Verify that I/O is queued */
	CU_ASSERT(!TAILQ_EMPTY(&bs_channel->queued_io));
	/* Verify that payload is not written to disk, at this point the blobs already switched */
	CU_ASSERT(blob->active.clusters[0] == 0);

	/* Finish all operations including spdk_bs_create_snapshot */
	poll_threads();

	/* Verify snapshot */
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);

	/* Verify that blob has unset frozen_io */
	CU_ASSERT(blob->frozen_refcnt == 0);

	/* Verify that postponed I/O completed successfully by comparing payload */
	spdk_blob_io_read(blob, channel, payload_read, 0, num_of_pages, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_write, payload_read, num_of_pages * BLOCKLEN) == 0);

	spdk_bs_free_io_channel(channel);
	poll_threads();

	ut_blob_close_and_delete(bs, blob);
}

static void
blob_clone(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob_opts opts;
	struct spdk_blob *blob, *snapshot, *clone;
	spdk_blob_id blobid, cloneid, snapshotid;
	struct spdk_blob_xattr_opts xattrs;
	const void *value;
	size_t value_len;
	int rc;

	/* Create blob with 10 clusters */

	ut_spdk_blob_opts_init(&opts);
	opts.num_clusters = 10;

	blob = ut_blob_create_and_open(bs, &opts);
	blobid = spdk_blob_get_id(blob);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 10);

	/* Create snapshot */
	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snapshotid = g_blobid;

	spdk_bs_open_blob(bs, snapshotid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot = g_blob;
	CU_ASSERT(snapshot->data_ro == true);
	CU_ASSERT(snapshot->md_ro == true);
	CU_ASSERT(spdk_blob_get_num_clusters(snapshot) == 10);

	spdk_blob_close(snapshot, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Create clone from snapshot with xattrs */
	xattrs.names = g_xattr_names;
	xattrs.get_value = _get_xattr_value;
	xattrs.count = 3;
	xattrs.ctx = &g_ctx;

	spdk_bs_create_clone(bs, snapshotid, &xattrs, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	cloneid = g_blobid;

	spdk_bs_open_blob(bs, cloneid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	clone = g_blob;
	CU_ASSERT(clone->data_ro == false);
	CU_ASSERT(clone->md_ro == false);
	CU_ASSERT(spdk_blob_get_num_clusters(clone) == 10);
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(clone) == 0);

	rc = spdk_blob_get_xattr_value(clone, g_xattr_names[0], &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(value_len == strlen(g_xattr_values[0]));
	CU_ASSERT_NSTRING_EQUAL_FATAL(value, g_xattr_values[0], value_len);

	rc = spdk_blob_get_xattr_value(clone, g_xattr_names[1], &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(value_len == strlen(g_xattr_values[1]));
	CU_ASSERT_NSTRING_EQUAL((char *)value, g_xattr_values[1], value_len);

	rc = spdk_blob_get_xattr_value(clone, g_xattr_names[2], &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(value_len == strlen(g_xattr_values[2]));
	CU_ASSERT_NSTRING_EQUAL((char *)value, g_xattr_values[2], value_len);


	spdk_blob_close(clone, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Try to create clone from not read only blob */
	spdk_bs_create_clone(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EINVAL);
	CU_ASSERT(g_blobid == SPDK_BLOBID_INVALID);

	/* Mark blob as read only */
	spdk_blob_set_read_only(blob);
	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Create clone from read only blob */
	spdk_bs_create_clone(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	cloneid = g_blobid;

	spdk_bs_open_blob(bs, cloneid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	clone = g_blob;
	CU_ASSERT(clone->data_ro == false);
	CU_ASSERT(clone->md_ro == false);
	CU_ASSERT(spdk_blob_get_num_clusters(clone) == 10);

	ut_blob_close_and_delete(bs, clone);
	ut_blob_close_and_delete(bs, blob);
}

static void
_blob_inflate(bool decouple_parent)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob_opts opts;
	struct spdk_blob *blob, *snapshot;
	spdk_blob_id blobid, snapshotid;
	struct spdk_io_channel *channel;
	uint64_t free_clusters;

	channel = spdk_bs_alloc_io_channel(bs);
	SPDK_CU_ASSERT_FATAL(channel != NULL);

	/* Create blob with 10 clusters */

	ut_spdk_blob_opts_init(&opts);
	opts.num_clusters = 10;
	opts.thin_provision = true;

	blob = ut_blob_create_and_open(bs, &opts);
	blobid = spdk_blob_get_id(blob);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 10);
	CU_ASSERT(spdk_blob_is_thin_provisioned(blob) == true);
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 0);

	/* 1) Blob with no parent */
	if (decouple_parent) {
		/* Decouple parent of blob with no parent (should fail) */
		spdk_bs_blob_decouple_parent(bs, channel, blobid, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno != 0);
	} else {
		/* Inflate of thin blob with no parent should made it thick */
		spdk_bs_inflate_blob(bs, channel, blobid, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(spdk_blob_is_thin_provisioned(blob) == false);
		CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 10);
	}

	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snapshotid = g_blobid;

	CU_ASSERT(spdk_blob_is_thin_provisioned(blob) == true);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 10);

	spdk_bs_open_blob(bs, snapshotid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot = g_blob;
	CU_ASSERT(snapshot->data_ro == true);
	CU_ASSERT(snapshot->md_ro == true);
	CU_ASSERT(spdk_blob_get_num_clusters(snapshot) == 10);

	spdk_blob_close(snapshot, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	free_clusters = spdk_bs_free_cluster_count(bs);

	/* 2) Blob with parent */
	if (!decouple_parent) {
		/* Do full blob inflation */
		spdk_bs_inflate_blob(bs, channel, blobid, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		/* all 10 clusters should be allocated */
		CU_ASSERT(spdk_bs_free_cluster_count(bs) == free_clusters - 10);
		CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 10);
	} else {
		/* Decouple parent of blob */
		spdk_bs_blob_decouple_parent(bs, channel, blobid, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		/* when only parent is removed, none of the clusters should be allocated */
		CU_ASSERT(spdk_bs_free_cluster_count(bs) == free_clusters);
		CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 0);
	}

	/* Now, it should be possible to delete snapshot */
	spdk_bs_delete_blob(bs, snapshotid, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 10);
	CU_ASSERT(spdk_blob_is_thin_provisioned(blob) == decouple_parent);

	spdk_bs_free_io_channel(channel);
	poll_threads();

	ut_blob_close_and_delete(bs, blob);
}

static void
blob_inflate(void)
{
	_blob_inflate(false);
	_blob_inflate(true);
}

static void
blob_delete(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob_opts blob_opts;
	spdk_blob_id blobid;

	/* Create a blob and then delete it. */
	ut_spdk_blob_opts_init(&blob_opts);
	spdk_bs_create_blob_ext(bs, &blob_opts, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid > 0);
	blobid = g_blobid;

	spdk_bs_delete_blob(bs, blobid, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Try to open the blob */
	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -ENOENT);
}

static void
blob_resize_test(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob;
	uint64_t free_clusters;

	free_clusters = spdk_bs_free_cluster_count(bs);

	blob = ut_blob_create_and_open(bs, NULL);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 0);

	/* Confirm that resize fails if blob is marked read-only. */
	blob->md_ro = true;
	spdk_blob_resize(blob, 5, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EPERM);
	blob->md_ro = false;

	/* The blob started at 0 clusters. Resize it to be 5. */
	spdk_blob_resize(blob, 5, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT((free_clusters - 5) == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 5);

	/* Shrink the blob to 3 clusters. This will not actually release
	 * the old clusters until the blob is synced.
	 */
	spdk_blob_resize(blob, 3, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	/* Verify there are still 5 clusters in use */
	CU_ASSERT((free_clusters - 5) == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 3);

	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	/* Now there are only 3 clusters in use */
	CU_ASSERT((free_clusters - 3) == spdk_bs_free_cluster_count(bs));

	/* Resize the blob to be 10 clusters. Growth takes effect immediately. */
	spdk_blob_resize(blob, 10, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT((free_clusters - 10) == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 10);

	/* Try to resize the blob to size larger than blobstore. */
	spdk_blob_resize(blob, bs->total_clusters + 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -ENOSPC);

	ut_blob_close_and_delete(bs, blob);
}

static void
blob_resize_thin_test(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob;
	struct spdk_blob_opts opts;
	struct spdk_io_channel *blob_ch;
	uint64_t free_clusters;
	uint64_t io_units_per_cluster;
	uint64_t offset;
	uint8_t buf1[DEV_BUFFER_BLOCKLEN];

	free_clusters = spdk_bs_free_cluster_count(bs);

	blob_ch = spdk_bs_alloc_io_channel(bs);
	SPDK_CU_ASSERT_FATAL(blob_ch != NULL);

	/* Create blob with thin provisioning enabled */
	ut_spdk_blob_opts_init(&opts);
	opts.thin_provision = true;
	opts.num_clusters = 0;

	blob = ut_blob_create_and_open(bs, &opts);
	CU_ASSERT((free_clusters) == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 0);
	io_units_per_cluster = bs_io_units_per_cluster(blob);

	/* The blob started at 0 clusters. Resize it to be 6. */
	spdk_blob_resize(blob, 6, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT((free_clusters) == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 0);

	/* Write on cluster 0,2,4 and 5 of blob */
	for (offset = 0; offset < io_units_per_cluster; offset++) {
		spdk_blob_io_write(blob, blob_ch, buf1, offset, 1, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
	}
	for (offset = 2 * io_units_per_cluster; offset < 3 * io_units_per_cluster; offset++) {
		spdk_blob_io_write(blob, blob_ch, buf1, offset, 1, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
	}
	for (offset = 4 * io_units_per_cluster; offset < 5 * io_units_per_cluster; offset++) {
		spdk_blob_io_write(blob, blob_ch, buf1, offset, 1, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
	}
	for (offset = 5 * io_units_per_cluster; offset < 6 * io_units_per_cluster; offset++) {
		spdk_blob_io_write(blob, blob_ch, buf1, offset, 1, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
	}

	/* Check allocated clusters after write */
	CU_ASSERT((free_clusters - 4) == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 4);

	/* Shrink the blob to 2 clusters. This will not actually release
	 * the old clusters until the blob is synced.
	 */
	spdk_blob_resize(blob, 2, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 2);
	CU_ASSERT((free_clusters - 4) == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 1);

	/* Sync blob: 4 clusters were truncated but only 3 of them was allocated */
	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT((free_clusters - 1) == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 2);
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 1);

	spdk_bs_free_io_channel(blob_ch);
	ut_blob_close_and_delete(bs, blob);
}

static void
blob_read_only(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	struct spdk_bs_opts opts;
	spdk_blob_id blobid;
	int rc;

	dev = init_dev();
	spdk_bs_opts_init(&opts, sizeof(opts));
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");

	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	blob = ut_blob_create_and_open(bs, NULL);
	blobid = spdk_blob_get_id(blob);

	rc = spdk_blob_set_read_only(blob);
	CU_ASSERT(rc == 0);

	CU_ASSERT(blob->data_ro == false);
	CU_ASSERT(blob->md_ro == false);

	spdk_blob_sync_md(blob, bs_op_complete, NULL);
	poll_threads();

	CU_ASSERT(blob->data_ro == true);
	CU_ASSERT(blob->md_ro == true);
	CU_ASSERT(blob->data_ro_flags & SPDK_BLOB_READ_ONLY);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	CU_ASSERT(blob->data_ro == true);
	CU_ASSERT(blob->md_ro == true);
	CU_ASSERT(blob->data_ro_flags & SPDK_BLOB_READ_ONLY);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	ut_bs_reload(&bs, &opts);

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	CU_ASSERT(blob->data_ro == true);
	CU_ASSERT(blob->md_ro == true);
	CU_ASSERT(blob->data_ro_flags & SPDK_BLOB_READ_ONLY);

	ut_blob_close_and_delete(bs, blob);

	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
}

static void
channel_ops(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_io_channel *channel;

	channel = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(channel != NULL);

	spdk_bs_free_io_channel(channel);
	poll_threads();
}

static void
blob_write(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob = g_blob;
	struct spdk_io_channel *channel;
	uint64_t io_units_per_cluster;
	uint8_t payload[10 * BLOCKLEN];

	io_units_per_cluster = spdk_bs_get_cluster_size(bs) / spdk_bs_get_io_unit_size(bs);

	channel = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(channel != NULL);

	/* Write to a blob with 0 size */
	spdk_blob_io_write(blob, channel, payload, 0, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EINVAL);

	/* Resize the blob */
	spdk_blob_resize(blob, 5, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Confirm that write fails if blob is marked read-only. */
	blob->data_ro = true;
	spdk_blob_io_write(blob, channel, payload, 0, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EPERM);
	blob->data_ro = false;

	/* Write to the blob */
	spdk_blob_io_write(blob, channel, payload, 0, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Write starting beyond the end */
	spdk_blob_io_write(blob, channel, payload, 5 * io_units_per_cluster, 1, blob_op_complete,
			   NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EINVAL);

	/* Write starting at a valid location but going off the end */
	spdk_blob_io_write(blob, channel, payload, 4 * io_units_per_cluster, io_units_per_cluster + 1,
			   blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EINVAL);

	spdk_bs_free_io_channel(channel);
	poll_threads();
}

static void
blob_read(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob = g_blob;
	struct spdk_io_channel *channel;
	uint64_t io_units_per_cluster;
	uint8_t payload[10 * BLOCKLEN];

	io_units_per_cluster = spdk_bs_get_cluster_size(bs) / spdk_bs_get_io_unit_size(bs);

	channel = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(channel != NULL);

	/* Read from a blob with 0 size */
	spdk_blob_io_read(blob, channel, payload, 0, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EINVAL);

	/* Resize the blob */
	spdk_blob_resize(blob, 5, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Confirm that read passes if blob is marked read-only. */
	blob->data_ro = true;
	spdk_blob_io_read(blob, channel, payload, 0, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	blob->data_ro = false;

	/* Read from the blob */
	spdk_blob_io_read(blob, channel, payload, 0, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Read starting beyond the end */
	spdk_blob_io_read(blob, channel, payload, 5 * io_units_per_cluster, 1, blob_op_complete,
			  NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EINVAL);

	/* Read starting at a valid location but going off the end */
	spdk_blob_io_read(blob, channel, payload, 4 * io_units_per_cluster, io_units_per_cluster + 1,
			  blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EINVAL);

	spdk_bs_free_io_channel(channel);
	poll_threads();
}

static void
blob_rw_verify(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob = g_blob;
	struct spdk_io_channel *channel;
	uint8_t payload_read[10 * BLOCKLEN];
	uint8_t payload_write[10 * BLOCKLEN];

	channel = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(channel != NULL);

	spdk_blob_resize(blob, 32, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	memset(payload_write, 0xE5, sizeof(payload_write));
	spdk_blob_io_write(blob, channel, payload_write, 4, 10, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	memset(payload_read, 0x00, sizeof(payload_read));
	spdk_blob_io_read(blob, channel, payload_read, 4, 10, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_write, payload_read, 4 * BLOCKLEN) == 0);

	spdk_bs_free_io_channel(channel);
	poll_threads();
}

static void
blob_rw_verify_iov(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob;
	struct spdk_io_channel *channel;
	uint8_t payload_read[10 * BLOCKLEN];
	uint8_t payload_write[10 * BLOCKLEN];
	struct iovec iov_read[3];
	struct iovec iov_write[3];
	void *buf;
	uint32_t first_data_cluster = FIRST_DATA_CLUSTER(bs);

	channel = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(channel != NULL);

	blob = ut_blob_create_and_open(bs, NULL);

	spdk_blob_resize(blob, 2, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/*
	 * Manually adjust the offset of the blob's second cluster.  This allows
	 *  us to make sure that the readv/write code correctly accounts for I/O
	 *  that cross cluster boundaries.  Start by asserting that the allocated
	 *  clusters are where we expect before modifying the second cluster.
	 */
	CU_ASSERT(blob->active.clusters[0] == first_data_cluster * 256);
	CU_ASSERT(blob->active.clusters[1] == (first_data_cluster + 1) * 256);
	blob->active.clusters[1] = (first_data_cluster + 2) * 256;

	memset(payload_write, 0xE5, sizeof(payload_write));
	iov_write[0].iov_base = payload_write;
	iov_write[0].iov_len = 1 * BLOCKLEN;
	iov_write[1].iov_base = payload_write + 1 * BLOCKLEN;
	iov_write[1].iov_len = 5 * BLOCKLEN;
	iov_write[2].iov_base = payload_write + 6 * BLOCKLEN;
	iov_write[2].iov_len = 4 * BLOCKLEN;
	/*
	 * Choose a page offset just before the cluster boundary.  The first 6 pages of payload
	 *  will get written to the first cluster, the last 4 to the second cluster.
	 */
	spdk_blob_io_writev(blob, channel, iov_write, 3, 250, 10, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	memset(payload_read, 0xAA, sizeof(payload_read));
	iov_read[0].iov_base = payload_read;
	iov_read[0].iov_len = 3 * BLOCKLEN;
	iov_read[1].iov_base = payload_read + 3 * BLOCKLEN;
	iov_read[1].iov_len = 4 * BLOCKLEN;
	iov_read[2].iov_base = payload_read + 7 * BLOCKLEN;
	iov_read[2].iov_len = 3 * BLOCKLEN;
	spdk_blob_io_readv(blob, channel, iov_read, 3, 250, 10, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_write, payload_read, 10 * BLOCKLEN) == 0);

	buf = calloc(1, 256 * BLOCKLEN);
	SPDK_CU_ASSERT_FATAL(buf != NULL);
	/* Check that cluster 2 on "disk" was not modified. */
	CU_ASSERT(memcmp(buf, &g_dev_buffer[(first_data_cluster + 1) * 256 * BLOCKLEN],
			 256 * BLOCKLEN) == 0);
	free(buf);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_free_io_channel(channel);
	poll_threads();
}

static uint32_t
bs_channel_get_req_count(struct spdk_io_channel *_channel)
{
	struct spdk_bs_channel *channel = spdk_io_channel_get_ctx(_channel);
	struct spdk_bs_request_set *set;
	uint32_t count = 0;

	TAILQ_FOREACH(set, &channel->reqs, link) {
		count++;
	}

	return count;
}

static void
blob_rw_verify_iov_nomem(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob = g_blob;
	struct spdk_io_channel *channel;
	uint8_t payload_write[10 * BLOCKLEN];
	struct iovec iov_write[3];
	uint32_t req_count;

	channel = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(channel != NULL);

	spdk_blob_resize(blob, 2, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/*
	 * Choose a page offset just before the cluster boundary.  The first 6 pages of payload
	 *  will get written to the first cluster, the last 4 to the second cluster.
	 */
	iov_write[0].iov_base = payload_write;
	iov_write[0].iov_len = 1 * BLOCKLEN;
	iov_write[1].iov_base = payload_write + 1 * BLOCKLEN;
	iov_write[1].iov_len = 5 * BLOCKLEN;
	iov_write[2].iov_base = payload_write + 6 * BLOCKLEN;
	iov_write[2].iov_len = 4 * BLOCKLEN;
	MOCK_SET(calloc, NULL);
	req_count = bs_channel_get_req_count(channel);
	spdk_blob_io_writev(blob, channel, iov_write, 3, 250, 10, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -ENOMEM);
	CU_ASSERT(req_count == bs_channel_get_req_count(channel));
	MOCK_CLEAR(calloc);

	spdk_bs_free_io_channel(channel);
	poll_threads();
}

static void
blob_rw_iov_read_only(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob = g_blob;
	struct spdk_io_channel *channel;
	uint8_t payload_read[BLOCKLEN];
	uint8_t payload_write[BLOCKLEN];
	struct iovec iov_read;
	struct iovec iov_write;

	channel = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(channel != NULL);

	spdk_blob_resize(blob, 2, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Verify that writev failed if read_only flag is set. */
	blob->data_ro = true;
	iov_write.iov_base = payload_write;
	iov_write.iov_len = sizeof(payload_write);
	spdk_blob_io_writev(blob, channel, &iov_write, 1, 0, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EPERM);

	/* Verify that reads pass if data_ro flag is set. */
	iov_read.iov_base = payload_read;
	iov_read.iov_len = sizeof(payload_read);
	spdk_blob_io_readv(blob, channel, &iov_read, 1, 0, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_free_io_channel(channel);
	poll_threads();
}

static void
_blob_io_read_no_split(struct spdk_blob *blob, struct spdk_io_channel *channel,
		       uint8_t *payload, uint64_t offset, uint64_t length,
		       spdk_blob_op_complete cb_fn, void *cb_arg)
{
	uint64_t i;
	uint8_t *buf;
	uint64_t io_unit_size = spdk_bs_get_io_unit_size(blob->bs);

	/* To be sure that operation is NOT split, read one io_unit at the time */
	buf = payload;
	for (i = 0; i < length; i++) {
		spdk_blob_io_read(blob, channel, buf, i + offset, 1, blob_op_complete, NULL);
		poll_threads();
		if (g_bserrno != 0) {
			/* Pass the error code up */
			break;
		}
		buf += io_unit_size;
	}

	cb_fn(cb_arg, g_bserrno);
}

static void
_blob_io_write_no_split(struct spdk_blob *blob, struct spdk_io_channel *channel,
			uint8_t *payload, uint64_t offset, uint64_t length,
			spdk_blob_op_complete cb_fn, void *cb_arg)
{
	uint64_t i;
	uint8_t *buf;
	uint64_t io_unit_size = spdk_bs_get_io_unit_size(blob->bs);

	/* To be sure that operation is NOT split, write one io_unit at the time */
	buf = payload;
	for (i = 0; i < length; i++) {
		spdk_blob_io_write(blob, channel, buf, i + offset, 1, blob_op_complete, NULL);
		poll_threads();
		if (g_bserrno != 0) {
			/* Pass the error code up */
			break;
		}
		buf += io_unit_size;
	}

	cb_fn(cb_arg, g_bserrno);
}

static void
blob_operation_split_rw(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob;
	struct spdk_io_channel *channel;
	struct spdk_blob_opts opts;
	uint64_t cluster_size;

	uint64_t payload_size;
	uint8_t *payload_read;
	uint8_t *payload_write;
	uint8_t *payload_pattern;

	uint64_t io_unit_size;
	uint64_t io_units_per_cluster;
	uint64_t io_units_per_payload;

	uint64_t i;

	cluster_size = spdk_bs_get_cluster_size(bs);
	io_unit_size = spdk_bs_get_io_unit_size(bs);
	io_units_per_cluster = cluster_size / io_unit_size;
	io_units_per_payload = io_units_per_cluster * 5;
	payload_size = cluster_size * 5;

	payload_read = malloc(payload_size);
	SPDK_CU_ASSERT_FATAL(payload_read != NULL);

	payload_write = malloc(payload_size);
	SPDK_CU_ASSERT_FATAL(payload_write != NULL);

	payload_pattern = malloc(payload_size);
	SPDK_CU_ASSERT_FATAL(payload_pattern != NULL);

	/* Prepare random pattern to write */
	memset(payload_pattern, 0xFF, payload_size);
	for (i = 0; i < io_units_per_payload; i++) {
		*((uint64_t *)(payload_pattern + io_unit_size * i)) = (i + 1);
	}

	channel = spdk_bs_alloc_io_channel(bs);
	SPDK_CU_ASSERT_FATAL(channel != NULL);

	/* Create blob */
	ut_spdk_blob_opts_init(&opts);
	opts.thin_provision = false;
	opts.num_clusters = 5;

	blob = ut_blob_create_and_open(bs, &opts);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 5);

	/* Initial read should return zeroed payload */
	memset(payload_read, 0xFF, payload_size);
	spdk_blob_io_read(blob, channel, payload_read, 0, io_units_per_payload, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(spdk_mem_all_zero(payload_read, payload_size));

	/* Fill whole blob except last page */
	spdk_blob_io_write(blob, channel, payload_pattern, 0, io_units_per_payload - 1,
			   blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Write last page with a pattern */
	spdk_blob_io_write(blob, channel, payload_pattern, io_units_per_payload - 1, 1,
			   blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Read whole blob and check consistency */
	memset(payload_read, 0xFF, payload_size);
	spdk_blob_io_read(blob, channel, payload_read, 0, io_units_per_payload, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_pattern, payload_read, payload_size - io_unit_size) == 0);
	CU_ASSERT(memcmp(payload_pattern, payload_read + payload_size - io_unit_size, io_unit_size) == 0);

	/* Fill whole blob except first page */
	spdk_blob_io_write(blob, channel, payload_pattern, 1, io_units_per_payload - 1,
			   blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Write first page with a pattern */
	spdk_blob_io_write(blob, channel, payload_pattern, 0, 1,
			   blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Read whole blob and check consistency */
	memset(payload_read, 0xFF, payload_size);
	spdk_blob_io_read(blob, channel, payload_read, 0, io_units_per_payload, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_pattern, payload_read + io_unit_size, payload_size - io_unit_size) == 0);
	CU_ASSERT(memcmp(payload_pattern, payload_read, io_unit_size) == 0);


	/* Fill whole blob with a pattern (5 clusters) */

	/* 1. Read test. */
	_blob_io_write_no_split(blob, channel, payload_pattern, 0, io_units_per_payload,
				blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	memset(payload_read, 0xFF, payload_size);
	spdk_blob_io_read(blob, channel, payload_read, 0, io_units_per_payload, blob_op_complete, NULL);
	poll_threads();
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_pattern, payload_read, payload_size) == 0);

	/* 2. Write test. */
	spdk_blob_io_write(blob, channel, payload_pattern, 0, io_units_per_payload,
			   blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	memset(payload_read, 0xFF, payload_size);
	_blob_io_read_no_split(blob, channel, payload_read, 0, io_units_per_payload, blob_op_complete,
			       NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_pattern, payload_read, payload_size) == 0);

	spdk_bs_free_io_channel(channel);
	poll_threads();

	g_blob = NULL;
	g_blobid = 0;

	free(payload_read);
	free(payload_write);
	free(payload_pattern);

	ut_blob_close_and_delete(bs, blob);
}

static void
blob_operation_split_rw_iov(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob;
	struct spdk_io_channel *channel;
	struct spdk_blob_opts opts;
	uint64_t cluster_size;

	uint64_t payload_size;
	uint8_t *payload_read;
	uint8_t *payload_write;
	uint8_t *payload_pattern;

	uint64_t io_unit_size;
	uint64_t io_units_per_cluster;
	uint64_t io_units_per_payload;

	struct iovec iov_read[2];
	struct iovec iov_write[2];

	uint64_t i, j;

	cluster_size = spdk_bs_get_cluster_size(bs);
	io_unit_size = spdk_bs_get_io_unit_size(bs);
	io_units_per_cluster = cluster_size / io_unit_size;
	io_units_per_payload = io_units_per_cluster * 5;
	payload_size = cluster_size * 5;

	payload_read = malloc(payload_size);
	SPDK_CU_ASSERT_FATAL(payload_read != NULL);

	payload_write = malloc(payload_size);
	SPDK_CU_ASSERT_FATAL(payload_write != NULL);

	payload_pattern = malloc(payload_size);
	SPDK_CU_ASSERT_FATAL(payload_pattern != NULL);

	/* Prepare random pattern to write */
	for (i = 0; i < io_units_per_payload; i++) {
		for (j = 0; j < io_unit_size / sizeof(uint64_t); j++) {
			uint64_t *tmp;

			tmp = (uint64_t *)payload_pattern;
			tmp += ((io_unit_size * i) / sizeof(uint64_t)) + j;
			*tmp = i + 1;
		}
	}

	channel = spdk_bs_alloc_io_channel(bs);
	SPDK_CU_ASSERT_FATAL(channel != NULL);

	/* Create blob */
	ut_spdk_blob_opts_init(&opts);
	opts.thin_provision = false;
	opts.num_clusters = 5;

	blob = ut_blob_create_and_open(bs, &opts);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 5);

	/* Initial read should return zeroes payload */
	memset(payload_read, 0xFF, payload_size);
	iov_read[0].iov_base = payload_read;
	iov_read[0].iov_len = cluster_size * 3;
	iov_read[1].iov_base = payload_read + cluster_size * 3;
	iov_read[1].iov_len = cluster_size * 2;
	spdk_blob_io_readv(blob, channel, iov_read, 2, 0, io_units_per_payload, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(spdk_mem_all_zero(payload_read, payload_size));

	/* First of iovs fills whole blob except last io_unit and second of iovs writes last io_unit
	 *  with a pattern. */
	iov_write[0].iov_base = payload_pattern;
	iov_write[0].iov_len = payload_size - io_unit_size;
	iov_write[1].iov_base = payload_pattern;
	iov_write[1].iov_len = io_unit_size;
	spdk_blob_io_writev(blob, channel, iov_write, 2, 0, io_units_per_payload, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Read whole blob and check consistency */
	memset(payload_read, 0xFF, payload_size);
	iov_read[0].iov_base = payload_read;
	iov_read[0].iov_len = cluster_size * 2;
	iov_read[1].iov_base = payload_read + cluster_size * 2;
	iov_read[1].iov_len = cluster_size * 3;
	spdk_blob_io_readv(blob, channel, iov_read, 2, 0, io_units_per_payload, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_pattern, payload_read, payload_size - io_unit_size) == 0);
	CU_ASSERT(memcmp(payload_pattern, payload_read + payload_size - io_unit_size, io_unit_size) == 0);

	/* First of iovs fills only first io_unit and second of iovs writes whole blob except
	 *  first io_unit with a pattern. */
	iov_write[0].iov_base = payload_pattern;
	iov_write[0].iov_len = io_unit_size;
	iov_write[1].iov_base = payload_pattern;
	iov_write[1].iov_len = payload_size - io_unit_size;
	spdk_blob_io_writev(blob, channel, iov_write, 2, 0, io_units_per_payload, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Read whole blob and check consistency */
	memset(payload_read, 0xFF, payload_size);
	iov_read[0].iov_base = payload_read;
	iov_read[0].iov_len = cluster_size * 4;
	iov_read[1].iov_base = payload_read + cluster_size * 4;
	iov_read[1].iov_len = cluster_size;
	spdk_blob_io_readv(blob, channel, iov_read, 2, 0, io_units_per_payload, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_pattern, payload_read + io_unit_size, payload_size - io_unit_size) == 0);
	CU_ASSERT(memcmp(payload_pattern, payload_read, io_unit_size) == 0);


	/* Fill whole blob with a pattern (5 clusters) */

	/* 1. Read test. */
	_blob_io_write_no_split(blob, channel, payload_pattern, 0, io_units_per_payload,
				blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	memset(payload_read, 0xFF, payload_size);
	iov_read[0].iov_base = payload_read;
	iov_read[0].iov_len = cluster_size;
	iov_read[1].iov_base = payload_read + cluster_size;
	iov_read[1].iov_len = cluster_size * 4;
	spdk_blob_io_readv(blob, channel, iov_read, 2, 0, io_units_per_payload, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_pattern, payload_read, payload_size) == 0);

	/* 2. Write test. */
	iov_write[0].iov_base = payload_read;
	iov_write[0].iov_len = cluster_size * 2;
	iov_write[1].iov_base = payload_read + cluster_size * 2;
	iov_write[1].iov_len = cluster_size * 3;
	spdk_blob_io_writev(blob, channel, iov_write, 2, 0, io_units_per_payload, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	memset(payload_read, 0xFF, payload_size);
	_blob_io_read_no_split(blob, channel, payload_read, 0, io_units_per_payload, blob_op_complete,
			       NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_pattern, payload_read, payload_size) == 0);

	spdk_bs_free_io_channel(channel);
	poll_threads();

	g_blob = NULL;
	g_blobid = 0;

	free(payload_read);
	free(payload_write);
	free(payload_pattern);

	ut_blob_close_and_delete(bs, blob);
}

static void
blob_unmap(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob;
	struct spdk_io_channel *channel;
	struct spdk_blob_opts opts;
	uint8_t payload[BLOCKLEN];
	uint32_t first_data_cluster = FIRST_DATA_CLUSTER(bs);
	int i;

	channel = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(channel != NULL);

	ut_spdk_blob_opts_init(&opts);
	opts.num_clusters = 10;

	blob = ut_blob_create_and_open(bs, &opts);

	spdk_blob_resize(blob, 10, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 10);

	memset(payload, 0, sizeof(payload));
	payload[0] = 0xFF;

	/*
	 * Set first byte of every cluster to 0xFF.
	 */
	for (i = 0; i < 10; i++) {
		g_dev_buffer[(first_data_cluster + i) * SPDK_BLOB_OPTS_CLUSTER_SZ] = 0xFF;
	}

	/* Confirm writes */
	for (i = 0; i < 10; i++) {
		payload[0] = 0;
		spdk_blob_io_read(blob, channel, &payload, i * SPDK_BLOB_OPTS_CLUSTER_SZ / BLOCKLEN, 1,
				  blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(payload[0] == 0xFF);
	}

	/* Mark some clusters as unallocated */
	blob->active.clusters[1] = 0;
	blob->active.clusters[2] = 0;
	blob->active.clusters[3] = 0;
	blob->active.clusters[6] = 0;
	blob->active.clusters[8] = 0;
	blob->active.num_allocated_clusters -= 5;

	/* Unmap clusters by resizing to 0 */
	spdk_blob_resize(blob, 0, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 0);

	/* Confirm that only 'allocated' clusters were unmapped */
	for (i = 0; i < 10; i++) {
		switch (i) {
		case 1:
		case 2:
		case 3:
		case 6:
		case 8:
			CU_ASSERT(g_dev_buffer[(first_data_cluster + i) * SPDK_BLOB_OPTS_CLUSTER_SZ] == 0xFF);
			break;
		default:
			CU_ASSERT(g_dev_buffer[(first_data_cluster + i) * SPDK_BLOB_OPTS_CLUSTER_SZ] == 0);
			break;
		}
	}

	spdk_bs_free_io_channel(channel);
	poll_threads();

	ut_blob_close_and_delete(bs, blob);
}

static void
blob_iter(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob;
	spdk_blob_id blobid;
	struct spdk_blob_opts blob_opts;

	spdk_bs_iter_first(bs, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_blob == NULL);
	CU_ASSERT(g_bserrno == -ENOENT);

	ut_spdk_blob_opts_init(&blob_opts);
	spdk_bs_create_blob_ext(bs, &blob_opts, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_iter_first(bs, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_blob != NULL);
	CU_ASSERT(g_bserrno == 0);
	blob = g_blob;
	CU_ASSERT(spdk_blob_get_id(blob) == blobid);

	spdk_bs_iter_next(bs, blob, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_blob == NULL);
	CU_ASSERT(g_bserrno == -ENOENT);
}

static void
blob_xattr(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob = g_blob;
	spdk_blob_id blobid = spdk_blob_get_id(blob);
	uint64_t length;
	int rc;
	const char *name1, *name2;
	const void *value;
	size_t value_len;
	struct spdk_xattr_names *names;

	/* Test that set_xattr fails if md_ro flag is set. */
	blob->md_ro = true;
	rc = spdk_blob_set_xattr(blob, "name", "log.txt", strlen("log.txt") + 1);
	CU_ASSERT(rc == -EPERM);

	blob->md_ro = false;
	rc = spdk_blob_set_xattr(blob, "name", "log.txt", strlen("log.txt") + 1);
	CU_ASSERT(rc == 0);

	length = 2345;
	rc = spdk_blob_set_xattr(blob, "length", &length, sizeof(length));
	CU_ASSERT(rc == 0);

	/* Overwrite "length" xattr. */
	length = 3456;
	rc = spdk_blob_set_xattr(blob, "length", &length, sizeof(length));
	CU_ASSERT(rc == 0);

	/* get_xattr should still work even if md_ro flag is set. */
	value = NULL;
	blob->md_ro = true;
	rc = spdk_blob_get_xattr_value(blob, "length", &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(*(uint64_t *)value == length);
	CU_ASSERT(value_len == 8);
	blob->md_ro = false;

	rc = spdk_blob_get_xattr_value(blob, "foobar", &value, &value_len);
	CU_ASSERT(rc == -ENOENT);

	names = NULL;
	rc = spdk_blob_get_xattr_names(blob, &names);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(names != NULL);
	CU_ASSERT(spdk_xattr_names_get_count(names) == 2);
	name1 = spdk_xattr_names_get_name(names, 0);
	SPDK_CU_ASSERT_FATAL(name1 != NULL);
	CU_ASSERT(!strcmp(name1, "name") || !strcmp(name1, "length"));
	name2 = spdk_xattr_names_get_name(names, 1);
	SPDK_CU_ASSERT_FATAL(name2 != NULL);
	CU_ASSERT(!strcmp(name2, "name") || !strcmp(name2, "length"));
	CU_ASSERT(strcmp(name1, name2));
	spdk_xattr_names_free(names);

	/* Confirm that remove_xattr fails if md_ro is set to true. */
	blob->md_ro = true;
	rc = spdk_blob_remove_xattr(blob, "name");
	CU_ASSERT(rc == -EPERM);

	blob->md_ro = false;
	rc = spdk_blob_remove_xattr(blob, "name");
	CU_ASSERT(rc == 0);

	rc = spdk_blob_remove_xattr(blob, "foobar");
	CU_ASSERT(rc == -ENOENT);

	/* Set internal xattr */
	length = 7898;
	rc = blob_set_xattr(blob, "internal", &length, sizeof(length), true);
	CU_ASSERT(rc == 0);
	rc = blob_get_xattr_value(blob, "internal", &value, &value_len, true);
	CU_ASSERT(rc == 0);
	CU_ASSERT(*(uint64_t *)value == length);
	/* try to get public xattr with same name */
	rc = spdk_blob_get_xattr_value(blob, "internal", &value, &value_len);
	CU_ASSERT(rc != 0);
	rc = blob_get_xattr_value(blob, "internal", &value, &value_len, false);
	CU_ASSERT(rc != 0);
	/* Check if SPDK_BLOB_INTERNAL_XATTR is set */
	CU_ASSERT((blob->invalid_flags & SPDK_BLOB_INTERNAL_XATTR) ==
		  SPDK_BLOB_INTERNAL_XATTR);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();

	/* Check if xattrs are persisted */
	ut_bs_reload(&bs, NULL);

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	rc = blob_get_xattr_value(blob, "internal", &value, &value_len, true);
	CU_ASSERT(rc == 0);
	CU_ASSERT(*(uint64_t *)value == length);

	/* try to get internal xattr through public call */
	rc = spdk_blob_get_xattr_value(blob, "internal", &value, &value_len);
	CU_ASSERT(rc != 0);

	rc = blob_remove_xattr(blob, "internal", true);
	CU_ASSERT(rc == 0);

	CU_ASSERT((blob->invalid_flags & SPDK_BLOB_INTERNAL_XATTR) == 0);
}

static void
blob_parse_md(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob;
	int rc;
	uint32_t used_pages;
	size_t xattr_length;
	char *xattr;

	used_pages = spdk_bit_array_count_set(bs->used_md_pages);
	blob = ut_blob_create_and_open(bs, NULL);

	/* Create large extent to force more than 1 page of metadata. */
	xattr_length = SPDK_BS_MAX_DESC_SIZE - sizeof(struct spdk_blob_md_descriptor_xattr) -
		       strlen("large_xattr");
	xattr = calloc(xattr_length, sizeof(char));
	SPDK_CU_ASSERT_FATAL(xattr != NULL);
	rc = spdk_blob_set_xattr(blob, "large_xattr", xattr, xattr_length);
	free(xattr);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	poll_threads();

	/* Delete the blob and verify that number of pages returned to before its creation. */
	SPDK_CU_ASSERT_FATAL(used_pages != spdk_bit_array_count_set(bs->used_md_pages));
	ut_blob_close_and_delete(bs, blob);
	SPDK_CU_ASSERT_FATAL(used_pages == spdk_bit_array_count_set(bs->used_md_pages));
}

static void
bs_load(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	spdk_blob_id blobid;
	struct spdk_blob *blob;
	struct spdk_bs_super_block *super_block;
	uint64_t length;
	int rc;
	const void *value;
	size_t value_len;
	struct spdk_bs_opts opts;
	struct spdk_blob_opts blob_opts;

	dev = init_dev();
	spdk_bs_opts_init(&opts, sizeof(opts));
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");

	/* Initialize a new blob store */
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	/* Try to open a blobid that does not exist */
	spdk_bs_open_blob(bs, 0, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -ENOENT);
	CU_ASSERT(g_blob == NULL);

	/* Create a blob */
	blob = ut_blob_create_and_open(bs, NULL);
	blobid = spdk_blob_get_id(blob);

	/* Try again to open valid blob but without the upper bit set */
	spdk_bs_open_blob(bs, blobid & 0xFFFFFFFF, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -ENOENT);
	CU_ASSERT(g_blob == NULL);

	/* Set some xattrs */
	rc = spdk_blob_set_xattr(blob, "name", "log.txt", strlen("log.txt") + 1);
	CU_ASSERT(rc == 0);

	length = 2345;
	rc = spdk_blob_set_xattr(blob, "length", &length, sizeof(length));
	CU_ASSERT(rc == 0);

	/* Resize the blob */
	spdk_blob_resize(blob, 10, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	blob = NULL;
	g_blob = NULL;
	g_blobid = SPDK_BLOBID_INVALID;

	/* Unload the blob store */
	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	g_blob = NULL;
	g_blobid = 0;

	super_block = (struct spdk_bs_super_block *)g_dev_buffer;
	CU_ASSERT(super_block->clean == 1);

	/* Load should fail for device with an unsupported blocklen */
	dev = init_dev();
	dev->blocklen = g_phys_blocklen * 2;
	spdk_bs_load(dev, NULL, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EINVAL);

	/* Load should when max_md_ops is set to zero */
	dev = init_dev();
	spdk_bs_opts_init(&opts, sizeof(opts));
	opts.max_md_ops = 0;
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EINVAL);

	/* Load should when max_channel_ops is set to zero */
	dev = init_dev();
	spdk_bs_opts_init(&opts, sizeof(opts));
	opts.max_channel_ops = 0;
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EINVAL);

	/* Load an existing blob store */
	dev = init_dev();
	spdk_bs_opts_init(&opts, sizeof(opts));
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	super_block = (struct spdk_bs_super_block *)g_dev_buffer;
	CU_ASSERT(super_block->clean == 1);
	CU_ASSERT(super_block->size == dev->blockcnt * dev->blocklen);

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	/* Verify that blobstore is marked dirty after first metadata sync */
	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	CU_ASSERT(super_block->clean == 1);

	/* Get the xattrs */
	value = NULL;
	rc = spdk_blob_get_xattr_value(blob, "length", &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(*(uint64_t *)value == length);
	CU_ASSERT(value_len == 8);

	rc = spdk_blob_get_xattr_value(blob, "foobar", &value, &value_len);
	CU_ASSERT(rc == -ENOENT);

	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 10);
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 10);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	blob = NULL;
	g_blob = NULL;

	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

	/* Load should fail: bdev size < saved size */
	dev = init_dev();
	dev->blockcnt /= 2;

	spdk_bs_opts_init(&opts, sizeof(opts));
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();

	CU_ASSERT(g_bserrno == -EILSEQ);

	/* Load should succeed: bdev size > saved size */
	dev = init_dev();
	dev->blockcnt *= 4;

	spdk_bs_opts_init(&opts, sizeof(opts));
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	CU_ASSERT(g_bserrno == 0);
	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();


	/* Test compatibility mode */

	dev = init_dev();
	super_block->size = 0;
	super_block->crc = blob_md_page_calc_crc(super_block);

	spdk_bs_opts_init(&opts, sizeof(opts));
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	/* Create a blob */
	ut_spdk_blob_opts_init(&blob_opts);
	spdk_bs_create_blob_ext(bs, &blob_opts, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);

	/* Blobstore should update number of blocks in super_block */
	CU_ASSERT(super_block->size == dev->blockcnt * dev->blocklen);
	CU_ASSERT(super_block->clean == 0);

	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(super_block->clean == 1);
	g_bs = NULL;

}

static void
bs_load_pending_removal(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob_opts opts;
	struct spdk_blob *blob, *snapshot;
	spdk_blob_id blobid, snapshotid;
	const void *value;
	size_t value_len;
	int rc;

	/* Create blob */
	ut_spdk_blob_opts_init(&opts);
	opts.num_clusters = 10;

	blob = ut_blob_create_and_open(bs, &opts);
	blobid = spdk_blob_get_id(blob);

	/* Create snapshot */
	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snapshotid = g_blobid;

	spdk_bs_open_blob(bs, snapshotid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot = g_blob;

	/* Set SNAPSHOT_PENDING_REMOVAL xattr */
	snapshot->md_ro = false;
	rc = blob_set_xattr(snapshot, SNAPSHOT_PENDING_REMOVAL, &blobid, sizeof(spdk_blob_id), true);
	CU_ASSERT(rc == 0);
	snapshot->md_ro = true;

	spdk_blob_close(snapshot, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Reload blobstore */
	ut_bs_reload(&bs, NULL);

	/* Snapshot should not be removed as blob is still pointing to it */
	spdk_bs_open_blob(bs, snapshotid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot = g_blob;

	/* SNAPSHOT_PENDING_REMOVAL xattr should be removed during load */
	rc = spdk_blob_get_xattr_value(snapshot, SNAPSHOT_PENDING_REMOVAL, &value, &value_len);
	CU_ASSERT(rc != 0);

	/* Set SNAPSHOT_PENDING_REMOVAL xattr again */
	snapshot->md_ro = false;
	rc = blob_set_xattr(snapshot, SNAPSHOT_PENDING_REMOVAL, &blobid, sizeof(spdk_blob_id), true);
	CU_ASSERT(rc == 0);
	snapshot->md_ro = true;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	/* Remove parent_id from blob by removing BLOB_SNAPSHOT xattr */
	blob_remove_xattr(blob, BLOB_SNAPSHOT, true);

	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(snapshot, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Reload blobstore */
	ut_bs_reload(&bs, NULL);

	/* Snapshot should be removed as blob is not pointing to it anymore */
	spdk_bs_open_blob(bs, snapshotid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno != 0);
}

static void
bs_load_custom_cluster_size(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_bs_super_block *super_block;
	struct spdk_bs_opts opts;
	uint32_t custom_cluster_size = 4194304; /* 4MiB */
	uint32_t cluster_sz;
	uint64_t total_clusters;

	dev = init_dev();
	spdk_bs_opts_init(&opts, sizeof(opts));
	opts.cluster_sz = custom_cluster_size;
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");

	/* Initialize a new blob store */
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;
	cluster_sz = bs->cluster_sz;
	total_clusters = bs->total_clusters;

	/* Unload the blob store */
	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	g_blob = NULL;
	g_blobid = 0;

	super_block = (struct spdk_bs_super_block *)g_dev_buffer;
	CU_ASSERT(super_block->clean == 1);

	/* Load an existing blob store */
	dev = init_dev();
	spdk_bs_opts_init(&opts, sizeof(opts));
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;
	/* Compare cluster size and number to one after initialization */
	CU_ASSERT(cluster_sz == bs->cluster_sz);
	CU_ASSERT(total_clusters == bs->total_clusters);

	super_block = (struct spdk_bs_super_block *)g_dev_buffer;
	CU_ASSERT(super_block->clean == 1);
	CU_ASSERT(super_block->size == dev->blockcnt * dev->blocklen);

	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(super_block->clean == 1);
	g_bs = NULL;
}

static void
bs_load_after_failed_grow(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_bs_super_block *super_block;
	struct spdk_bs_opts opts;
	struct spdk_bs_md_mask *mask;
	struct spdk_blob_opts blob_opts;
	struct spdk_blob *blob, *snapshot;
	spdk_blob_id blobid, snapshotid;
	uint64_t total_data_clusters;

	dev = init_dev();
	spdk_bs_opts_init(&opts, sizeof(opts));
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");
	/*
	 * The bdev_size is 64M, cluster_sz is 1M, so there are 64 clusters. The
	 * blobstore will create 64 md pages by default. We set num_md_pages to 128,
	 * thus the blobstore could grow to the double size.
	 */
	opts.num_md_pages = 128;

	/* Initialize a new blob store */
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	/* Create blob */
	ut_spdk_blob_opts_init(&blob_opts);
	blob_opts.num_clusters = 10;

	blob = ut_blob_create_and_open(bs, &blob_opts);
	blobid = spdk_blob_get_id(blob);

	/* Create snapshot */
	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snapshotid = g_blobid;

	spdk_bs_open_blob(bs, snapshotid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot = g_blob;

	spdk_blob_close(snapshot, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	total_data_clusters = bs->total_data_clusters;
	CU_ASSERT(bs->num_free_clusters + 10 == total_data_clusters);

	/* Unload the blob store */
	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	g_blob = NULL;
	g_blobid = 0;

	super_block = (struct spdk_bs_super_block *)g_dev_buffer;
	CU_ASSERT(super_block->clean == 1);

	mask = (struct spdk_bs_md_mask *)(g_dev_buffer + super_block->used_cluster_mask_start *
					  g_phys_blocklen);
	CU_ASSERT(mask->type == SPDK_MD_MASK_TYPE_USED_CLUSTERS);
	CU_ASSERT(mask->length == super_block->size / super_block->cluster_size);

	/*
	 * We change the mask->length to emulate this scenario: A spdk_bs_grow failed after it changed
	 * the used_cluster bitmap length, but it didn't change the super block yet.
	 */
	mask->length *= 2;

	/* Load an existing blob store */
	dev = init_dev();
	dev->blockcnt *= 2;
	spdk_bs_opts_init(&opts, sizeof(opts));
	opts.clear_method = BS_CLEAR_WITH_NONE;
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	/* Check the capacity is the same as before */
	CU_ASSERT(bs->total_data_clusters == total_data_clusters);
	CU_ASSERT(bs->num_free_clusters + 10 == total_data_clusters);

	/* Check the blob and the snapshot are still available */
	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_open_blob(bs, snapshotid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot = g_blob;

	spdk_blob_close(snapshot, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(super_block->clean == 1);
	g_bs = NULL;
}

static void
bs_load_error(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_bs_opts opts;
	struct spdk_power_failure_thresholds thresholds = {};

	dev = init_dev();
	spdk_bs_opts_init(&opts, sizeof(opts));
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");

	/* Initialize a new blob store */
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	/* Unload the blob store */
	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Load fails with I/O error */
	thresholds.general_threshold = 2;
	dev_set_power_failure_thresholds(thresholds);
	g_bserrno = -1;
	dev = init_dev();
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EIO);
	CU_ASSERT(g_bs == NULL);
	dev_reset_power_failure_event();

	/* Load fails with NOMEM error */
	g_bserrno = -1;
	dev = init_dev();
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	MOCK_SET(spdk_zmalloc, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -ENOMEM);
	CU_ASSERT(g_bs == NULL);
	MOCK_CLEAR(spdk_zmalloc);
}

static void
bs_type(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_bs_opts opts;

	dev = init_dev();
	spdk_bs_opts_init(&opts, sizeof(opts));
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");

	/* Initialize a new blob store */
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	/* Unload the blob store */
	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	g_blob = NULL;
	g_blobid = 0;

	/* Load non existing blobstore type */
	dev = init_dev();
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "NONEXISTING");
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno != 0);

	/* Load with empty blobstore type */
	dev = init_dev();
	memset(opts.bstype.bstype, 0, sizeof(opts.bstype.bstype));
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

	/* Initialize a new blob store with empty bstype */
	dev = init_dev();
	memset(opts.bstype.bstype, 0, sizeof(opts.bstype.bstype));
	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

	/* Load non existing blobstore type */
	dev = init_dev();
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "NONEXISTING");
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno != 0);

	/* Load with empty blobstore type */
	dev = init_dev();
	memset(opts.bstype.bstype, 0, sizeof(opts.bstype.bstype));
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
bs_super_block(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_bs_super_block *super_block;
	struct spdk_bs_opts opts;
	struct spdk_bs_super_block_ver1 super_block_v1;

	dev = init_dev();
	spdk_bs_opts_init(&opts, sizeof(opts));
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");

	/* Initialize a new blob store */
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	/* Unload the blob store */
	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	g_blob = NULL;
	g_blobid = 0;

	/* Load an existing blob store with version newer than supported */
	super_block = (struct spdk_bs_super_block *)g_dev_buffer;
	super_block->version++;

	dev = init_dev();
	memset(opts.bstype.bstype, 0, sizeof(opts.bstype.bstype));
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno != 0);

	/* Create a new blob store with super block version 1 */
	dev = init_dev();
	super_block_v1.version = 1;
	memcpy(super_block_v1.signature, "SPDKBLOB", sizeof(super_block_v1.signature));
	super_block_v1.length = 0x1000;
	super_block_v1.clean = 1;
	super_block_v1.super_blob = 0xFFFFFFFFFFFFFFFF;
	super_block_v1.cluster_size = 0x100000;
	super_block_v1.used_page_mask_start = 0x01;
	super_block_v1.used_page_mask_len = 0x01;
	super_block_v1.used_cluster_mask_start = 0x02;
	super_block_v1.used_cluster_mask_len = 0x01;
	super_block_v1.md_start = 0x03;
	super_block_v1.md_len = 0x40;
	memset(super_block_v1.reserved, 0, 4036);
	super_block_v1.crc = blob_md_page_calc_crc(&super_block_v1);
	memcpy(g_dev_buffer, &super_block_v1, sizeof(struct spdk_bs_super_block_ver1));

	memset(opts.bstype.bstype, 0, sizeof(opts.bstype.bstype));
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
bs_test_recover_cluster_count(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_bs_super_block super_block;
	struct spdk_bs_opts opts;

	dev = init_dev();
	spdk_bs_opts_init(&opts, sizeof(opts));
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");

	super_block.version = 3;
	memcpy(super_block.signature, "SPDKBLOB", sizeof(super_block.signature));
	super_block.length = 0x1000;
	super_block.clean = 0;
	super_block.super_blob = 0xFFFFFFFFFFFFFFFF;
	super_block.cluster_size = g_phys_blocklen;
	super_block.used_page_mask_start = 0x01;
	super_block.used_page_mask_len = 0x01;
	super_block.used_cluster_mask_start = 0x02;
	super_block.used_cluster_mask_len = 0x01;
	super_block.used_blobid_mask_start = 0x03;
	super_block.used_blobid_mask_len = 0x01;
	super_block.md_page_size = g_phys_blocklen;
	super_block.md_start = 0x04;
	super_block.md_len = 0x40;
	memset(super_block.bstype.bstype, 0, sizeof(super_block.bstype.bstype));
	super_block.size = dev->blockcnt * dev->blocklen;
	super_block.io_unit_size = 0x1000;
	memset(super_block.reserved, 0, SPDK_SIZEOF_MEMBER(struct spdk_bs_super_block, reserved));
	super_block.crc = blob_md_page_calc_crc(&super_block);
	memcpy(g_dev_buffer, &super_block, sizeof(struct spdk_bs_super_block));

	memset(opts.bstype.bstype, 0, sizeof(opts.bstype.bstype));
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;
	CU_ASSERT(bs->num_free_clusters == bs->total_clusters - (super_block.md_start +
			super_block.md_len));

	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
bs_grow_live_size(uint64_t new_blockcnt)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_bs_super_block super_block;
	struct spdk_bs_opts opts;
	struct spdk_bs_md_mask mask;
	uint64_t bdev_size;
	uint64_t total_data_clusters;

	/*
	 * Further down the test the dev size will be larger than the g_dev_buffer size,
	 * so we set clear_method to NONE, or the blobstore will try to clear the dev and
	 * will write beyond the end of g_dev_buffer.
	 */
	dev = init_dev();
	spdk_bs_opts_init(&opts, sizeof(opts));
	opts.clear_method = BS_CLEAR_WITH_NONE;
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	/*
	 * Set the dev size according to the new_blockcnt,
	 * then the blobstore will adjust the metadata according to the new size.
	 */
	dev->blockcnt = new_blockcnt;
	bdev_size = dev->blockcnt * dev->blocklen;
	spdk_bs_grow_live(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	total_data_clusters = spdk_bs_total_data_cluster_count(bs);

	/* Make sure the super block is updated. */
	memcpy(&super_block, g_dev_buffer, sizeof(struct spdk_bs_super_block));
	CU_ASSERT(super_block.size == bdev_size);
	CU_ASSERT(super_block.clean == 0);
	/* The used_cluster mask is not written out until first spdk_bs_unload. */
	memcpy(&mask, g_dev_buffer + super_block.used_cluster_mask_start * g_phys_blocklen,
	       sizeof(struct spdk_bs_md_mask));
	CU_ASSERT(mask.type == 0);
	CU_ASSERT(mask.length == 0);

	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

	/* Make sure all metadata is correct, super block and used_cluster mask. */
	memcpy(&super_block, g_dev_buffer, sizeof(struct spdk_bs_super_block));
	CU_ASSERT(super_block.size == bdev_size);
	CU_ASSERT(super_block.clean == 1);
	memcpy(&mask, g_dev_buffer + super_block.used_cluster_mask_start * g_phys_blocklen,
	       sizeof(struct spdk_bs_md_mask));
	CU_ASSERT(mask.type == SPDK_MD_MASK_TYPE_USED_CLUSTERS);
	CU_ASSERT(mask.length == bdev_size / (1 * 1024 * 1024));

	/* Load blobstore and check the cluster counts again. */
	dev = init_dev();
	dev->blockcnt = new_blockcnt;
	spdk_bs_load(dev, NULL, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	CU_ASSERT(super_block.clean == 1);
	bs = g_bs;
	CU_ASSERT(total_data_clusters == spdk_bs_total_data_cluster_count(bs));

	/* Perform grow without change in size, expected pass. */
	spdk_bs_grow_live(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(total_data_clusters == spdk_bs_total_data_cluster_count(bs));
	memcpy(&super_block, g_dev_buffer, sizeof(struct spdk_bs_super_block));
	CU_ASSERT(super_block.size == bdev_size);
	CU_ASSERT(super_block.clean == 1);

	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
bs_grow_live(void)
{
	/* No change expected */
	bs_grow_live_size(DEV_BUFFER_BLOCKCNT);

	/* Size slightly increased, but not enough to increase cluster count */
	bs_grow_live_size(DEV_BUFFER_BLOCKCNT + 1);

	/* Size doubled, increasing the cluster count */
	bs_grow_live_size(DEV_BUFFER_BLOCKCNT * 2);
}

static void
bs_grow_live_no_space(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_bs_super_block super_block;
	struct spdk_bs_opts opts;
	struct spdk_bs_md_mask mask;
	uint64_t bdev_size_init;
	uint64_t total_data_clusters, max_clusters;

	/*
	 * Further down the test the dev size will be larger than the g_dev_buffer size,
	 * so we set clear_method to NONE, or the blobstore will try to clear the dev and
	 * will write beyond the end of g_dev_buffer.
	 */
	dev = init_dev();
	bdev_size_init = dev->blockcnt * dev->blocklen;
	spdk_bs_opts_init(&opts, sizeof(opts));
	opts.clear_method = BS_CLEAR_WITH_NONE;
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;
	total_data_clusters = spdk_bs_total_data_cluster_count(bs);

	/*
	 * The default dev size is 64M, here we set the dev size to 32M,
	 * expecting EILSEQ due to super_block validation and no change in blobstore.
	 */
	dev->blockcnt = (32L * 1024L * 1024L) / dev->blocklen;
	spdk_bs_grow_live(bs, bs_op_complete, NULL);
	poll_threads();
	/* This error code comes from bs_super_validate() */
	CU_ASSERT(g_bserrno == -EILSEQ);
	CU_ASSERT(total_data_clusters == spdk_bs_total_data_cluster_count(bs));
	memcpy(&super_block, g_dev_buffer, sizeof(struct spdk_bs_super_block));
	CU_ASSERT(super_block.size == bdev_size_init);

	/*
	 * Blobstore in this test has only space for single md_page for used_clusters,
	 * which fits 1 bit per cluster minus the md header.
	 *
	 * Dev size is increased to exceed the reserved space for the used_cluster_mask
	 * in the metadata, expecting ENOSPC and no change in blobstore.
	 */
	max_clusters = (spdk_bs_get_page_size(bs) - sizeof(struct spdk_bs_md_mask)) * 8;
	max_clusters += 1;
	dev->blockcnt = (max_clusters * spdk_bs_get_cluster_size(bs)) / dev->blocklen;
	spdk_bs_grow_live(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -ENOSPC);
	CU_ASSERT(total_data_clusters == spdk_bs_total_data_cluster_count(bs));
	memcpy(&super_block, g_dev_buffer, sizeof(struct spdk_bs_super_block));
	CU_ASSERT(super_block.size == bdev_size_init);

	/*
	 * No change should have occurred for the duration of the test,
	 * unload blobstore and check metadata.
	 */
	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

	/* Make sure all metadata is correct, super block and used_cluster mask. */
	memcpy(&super_block, g_dev_buffer, sizeof(struct spdk_bs_super_block));
	CU_ASSERT(super_block.size == bdev_size_init);
	CU_ASSERT(super_block.clean == 1);
	memcpy(&mask, g_dev_buffer + super_block.used_cluster_mask_start * g_phys_blocklen,
	       sizeof(struct spdk_bs_md_mask));
	CU_ASSERT(mask.type == SPDK_MD_MASK_TYPE_USED_CLUSTERS);
	CU_ASSERT(mask.length == bdev_size_init / (1 * 1024 * 1024));

	/* Load blobstore and check the cluster counts again. */
	dev = init_dev();
	spdk_bs_load(dev, NULL, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;
	CU_ASSERT(total_data_clusters == spdk_bs_total_data_cluster_count(bs));

	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
bs_test_grow(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_bs_super_block super_block;
	struct spdk_bs_opts opts;
	struct spdk_bs_md_mask mask;
	uint64_t bdev_size;

	dev = init_dev();
	bdev_size = dev->blockcnt * dev->blocklen;
	spdk_bs_opts_init(&opts, sizeof(opts));
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

	/*
	 * To make sure all the metadata are updated to the disk,
	 * we check the g_dev_buffer after spdk_bs_unload.
	 */
	memcpy(&super_block, g_dev_buffer, sizeof(struct spdk_bs_super_block));
	CU_ASSERT(super_block.size == bdev_size);

	/*
	 * Make sure the used_cluster mask is correct.
	 */
	memcpy(&mask, g_dev_buffer + super_block.used_cluster_mask_start * g_phys_blocklen,
	       sizeof(struct spdk_bs_md_mask));
	CU_ASSERT(mask.type == SPDK_MD_MASK_TYPE_USED_CLUSTERS);
	CU_ASSERT(mask.length == bdev_size / (1 * 1024 * 1024));

	/*
	 * The default dev size is 64M, here we set the dev size to 128M,
	 * then the blobstore will adjust the metadata according to the new size.
	 * The dev size is larger than the g_dev_buffer size, so we set clear_method
	 * to NONE, or the blobstore will try to clear the dev and will write beyond
	 * the end of g_dev_buffer.
	 */
	dev = init_dev();
	dev->blockcnt = (128L * 1024L * 1024L) / dev->blocklen;
	bdev_size = dev->blockcnt * dev->blocklen;
	spdk_bs_opts_init(&opts, sizeof(opts));
	opts.clear_method = BS_CLEAR_WITH_NONE;
	spdk_bs_grow(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	/*
	 * After spdk_bs_grow, all metadata are updated to the disk.
	 * So we can check g_dev_buffer now.
	 */
	memcpy(&super_block, g_dev_buffer, sizeof(struct spdk_bs_super_block));
	CU_ASSERT(super_block.size == bdev_size);

	/*
	 * Make sure the used_cluster mask has been updated according to the bdev size
	 */
	memcpy(&mask, g_dev_buffer + super_block.used_cluster_mask_start * g_phys_blocklen,
	       sizeof(struct spdk_bs_md_mask));
	CU_ASSERT(mask.type == SPDK_MD_MASK_TYPE_USED_CLUSTERS);
	CU_ASSERT(mask.length == bdev_size / (1 * 1024 * 1024));

	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

/*
 * Create a blobstore and then unload it.
 */
static void
bs_unload(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob;
	struct spdk_power_failure_thresholds thresholds = {};

	/* Create a blob and open it. */
	blob = ut_blob_create_and_open(bs, NULL);

	/* Try to unload blobstore, should fail with open blob */
	g_bserrno = -1;
	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EBUSY);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);

	/* Close the blob, then successfully unload blobstore */
	g_bserrno = -1;
	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Try to unload blobstore, should fail due to I/O error */
	thresholds.general_threshold = 2;
	dev_set_power_failure_thresholds(thresholds);
	g_bserrno = -1;
	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EIO);
	dev_reset_power_failure_event();

	/* Try to unload blobstore, should fail with spdk_zmalloc returning NULL */
	g_bserrno = -1;
	spdk_bs_unload(bs, bs_op_complete, NULL);
	MOCK_SET(spdk_zmalloc, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -ENOMEM);
	MOCK_CLEAR(spdk_zmalloc);
}

/*
 * Create a blobstore with a cluster size different than the default, and ensure it is
 *  persisted.
 */
static void
bs_cluster_sz(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_bs_opts opts;
	uint32_t cluster_sz;

	/* Set cluster size to zero */
	dev = init_dev();
	spdk_bs_opts_init(&opts, sizeof(opts));
	opts.cluster_sz = 0;

	/* Initialize a new blob store */
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EINVAL);
	SPDK_CU_ASSERT_FATAL(g_bs == NULL);

	/*
	 * Set cluster size to blobstore page size,
	 * to work it is required to be at least twice the blobstore page size.
	 */
	dev = init_dev();
	spdk_bs_opts_init(&opts, sizeof(opts));
	opts.cluster_sz = g_phys_blocklen;

	/* Initialize a new blob store */
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -ENOMEM);
	SPDK_CU_ASSERT_FATAL(g_bs == NULL);

	/*
	 * Set cluster size to lower than page size,
	 * to work it is required to be at least twice the blobstore page size.
	 */
	dev = init_dev();
	spdk_bs_opts_init(&opts, sizeof(opts));
	opts.cluster_sz = g_phys_blocklen - 1;

	/* Initialize a new blob store */
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EINVAL);
	SPDK_CU_ASSERT_FATAL(g_bs == NULL);

	/* Set cluster size to twice the default */
	dev = init_dev();
	spdk_bs_opts_init(&opts, sizeof(opts));
	opts.cluster_sz *= 2;
	cluster_sz = opts.cluster_sz;

	/* Initialize a new blob store */
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	CU_ASSERT(spdk_bs_get_cluster_size(bs) == cluster_sz);

	ut_bs_reload(&bs, &opts);

	CU_ASSERT(spdk_bs_get_cluster_size(bs) == cluster_sz);

	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

/*
 * Create a blobstore, reload it and ensure total usable cluster count
 *  stays the same.
 */
static void
bs_usable_clusters(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob;
	uint32_t clusters;
	int i;


	clusters = spdk_bs_total_data_cluster_count(bs);

	ut_bs_reload(&bs, NULL);

	CU_ASSERT(spdk_bs_total_data_cluster_count(bs) == clusters);

	/* Create and resize blobs to make sure that usable cluster count won't change */
	for (i = 0; i < 4; i++) {
		g_bserrno = -1;
		g_blobid = SPDK_BLOBID_INVALID;
		blob = ut_blob_create_and_open(bs, NULL);

		spdk_blob_resize(blob, 10, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);

		g_bserrno = -1;
		spdk_blob_close(blob, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);

		CU_ASSERT(spdk_bs_total_data_cluster_count(bs) == clusters);
	}

	/* Reload the blob store to make sure that nothing changed */
	ut_bs_reload(&bs, NULL);

	CU_ASSERT(spdk_bs_total_data_cluster_count(bs) == clusters);
}

/*
 * Test resizing of the metadata blob.  This requires creating enough blobs
 *  so that one cluster is not enough to fit the metadata for those blobs.
 *  To induce this condition to happen more quickly, we reduce the cluster
 *  size to 16KB, which means only 4 4KB blob metadata pages can fit.
 */
static void
bs_resize_md(void)
{
	struct spdk_blob_store *bs;
	const int CLUSTER_PAGE_COUNT = 4;
	const int NUM_BLOBS = CLUSTER_PAGE_COUNT * 4;
	struct spdk_bs_dev *dev;
	struct spdk_bs_opts opts;
	struct spdk_blob *blob;
	struct spdk_blob_opts blob_opts;
	uint32_t cluster_sz;
	spdk_blob_id blobids[NUM_BLOBS];
	int i;


	dev = init_dev();
	spdk_bs_opts_init(&opts, sizeof(opts));
	opts.cluster_sz = CLUSTER_PAGE_COUNT * g_phys_blocklen;
	cluster_sz = opts.cluster_sz;

	/* Initialize a new blob store */
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	CU_ASSERT(spdk_bs_get_cluster_size(bs) == cluster_sz);

	ut_spdk_blob_opts_init(&blob_opts);

	for (i = 0; i < NUM_BLOBS; i++) {
		g_bserrno = -1;
		g_blobid = SPDK_BLOBID_INVALID;
		spdk_bs_create_blob_ext(bs, &blob_opts, blob_op_with_id_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(g_blobid !=  SPDK_BLOBID_INVALID);
		blobids[i] = g_blobid;
	}

	ut_bs_reload(&bs, &opts);

	CU_ASSERT(spdk_bs_get_cluster_size(bs) == cluster_sz);

	for (i = 0; i < NUM_BLOBS; i++) {
		g_bserrno = -1;
		g_blob = NULL;
		spdk_bs_open_blob(bs, blobids[i], blob_op_with_handle_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(g_blob !=  NULL);
		blob = g_blob;
		g_bserrno = -1;
		spdk_blob_close(blob, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
	}

	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
bs_destroy(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_power_failure_thresholds thresholds = {};

	/* Initialize a new blob store */
	dev = init_dev();
	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	/* Destroy the blobstore, should fail due to I/O error */
	thresholds.general_threshold = 1;
	dev_set_power_failure_thresholds(thresholds);
	g_bserrno = -1;
	spdk_bs_destroy(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EIO);
	dev_reset_power_failure_event();

	/* Destroy the blob store */
	g_bserrno = -1;
	spdk_bs_destroy(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Loading an non-existent blob store should fail. */
	g_bs = NULL;
	dev = init_dev();

	g_bserrno = 0;
	spdk_bs_load(dev, NULL, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno != 0);
}

/* Try to hit all of the corner cases associated with serializing
 * a blob to disk
 */
static void
blob_serialize_test(void)
{
	struct spdk_bs_dev *dev;
	struct spdk_bs_opts opts;
	struct spdk_blob_store *bs;
	spdk_blob_id blobid[2];
	struct spdk_blob *blob[2];
	uint64_t i;
	char *value;
	int rc;

	dev = init_dev();

	/* Initialize a new blobstore with very small clusters */
	spdk_bs_opts_init(&opts, sizeof(opts));
	opts.cluster_sz = dev->blocklen * 8;
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	/* Create and open two blobs */
	for (i = 0; i < 2; i++) {
		blob[i] = ut_blob_create_and_open(bs, NULL);
		blobid[i] = spdk_blob_get_id(blob[i]);

		/* Set a fairly large xattr on both blobs to eat up
		 * metadata space
		 */
		value = calloc(dev->blocklen - 64, sizeof(char));
		SPDK_CU_ASSERT_FATAL(value != NULL);
		memset(value, i, dev->blocklen / 2);
		rc = spdk_blob_set_xattr(blob[i], "name", value, dev->blocklen - 64);
		CU_ASSERT(rc == 0);
		free(value);
	}

	/* Resize the blobs, alternating 1 cluster at a time.
	 * This thwarts run length encoding and will cause spill
	 * over of the extents.
	 */
	for (i = 0; i < 6; i++) {
		spdk_blob_resize(blob[i % 2], (i / 2) + 1, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
	}

	for (i = 0; i < 2; i++) {
		spdk_blob_sync_md(blob[i], blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
	}

	/* Close the blobs */
	for (i = 0; i < 2; i++) {
		spdk_blob_close(blob[i], blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
	}

	ut_bs_reload(&bs, &opts);

	for (i = 0; i < 2; i++) {
		blob[i] = NULL;

		spdk_bs_open_blob(bs, blobid[i], blob_op_with_handle_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(g_blob != NULL);
		blob[i] = g_blob;

		CU_ASSERT(spdk_blob_get_num_clusters(blob[i]) == 3);

		spdk_blob_close(blob[i], blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
	}

	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_crc(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob;
	spdk_blob_id blobid;
	uint32_t page_num;
	int index;
	struct spdk_blob_md_page *page;

	blob = ut_blob_create_and_open(bs, NULL);
	blobid = spdk_blob_get_id(blob);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	page_num = bs_blobid_to_page(blobid);
	index = g_phys_blocklen * (bs->md_start + page_num);
	page = (struct spdk_blob_md_page *)&g_dev_buffer[index];
	page->crc = 0;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EINVAL);
	CU_ASSERT(g_blob == NULL);
	g_bserrno = 0;

	spdk_bs_delete_blob(bs, blobid, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EINVAL);
}

static void
super_block_crc(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_bs_super_block *super_block;

	dev = init_dev();
	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

	super_block = (struct spdk_bs_super_block *)g_dev_buffer;
	super_block->crc = 0;
	dev = init_dev();

	/* Load an existing blob store */
	g_bserrno = 0;
	spdk_bs_load(dev, NULL, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EILSEQ);
}

/* For blob dirty shutdown test case we do the following sub-test cases:
 * 1 Initialize new blob store and create 1 super blob with some xattrs, then we
 *   dirty shutdown and reload the blob store and verify the xattrs.
 * 2 Resize the blob from 10 clusters to 20 clusters and then dirty shutdown,
 *   reload the blob store and verify the clusters number.
 * 3 Create the second blob and then dirty shutdown, reload the blob store
 *   and verify the second blob.
 * 4 Delete the second blob and then dirty shutdown, reload the blob store
 *   and verify the second blob is invalid.
 * 5 Create the second blob again and also create the third blob, modify the
 *   md of second blob which makes the md invalid, and then dirty shutdown,
 *   reload the blob store verify the second blob, it should invalid and also
 *   verify the third blob, it should correct.
 */
static void
blob_dirty_shutdown(void)
{
	int rc;
	int index;
	struct spdk_blob_store *bs = g_bs;
	spdk_blob_id blobid1, blobid2, blobid3;
	struct spdk_blob *blob = g_blob;
	uint64_t length;
	uint64_t free_clusters;
	const void *value;
	size_t value_len;
	uint32_t page_num;
	struct spdk_blob_md_page *page;
	struct spdk_blob_opts blob_opts;

	/* Create first blob */
	blobid1 = spdk_blob_get_id(blob);

	/* Set some xattrs */
	rc = spdk_blob_set_xattr(blob, "name", "log.txt", strlen("log.txt") + 1);
	CU_ASSERT(rc == 0);

	length = 2345;
	rc = spdk_blob_set_xattr(blob, "length", &length, sizeof(length));
	CU_ASSERT(rc == 0);

	/* Put xattr that fits exactly single page.
	 * This results in adding additional pages to MD.
	 * First is flags and smaller xattr, second the large xattr,
	 * third are just the extents.
	 */
	size_t xattr_length = 4072 - sizeof(struct spdk_blob_md_descriptor_xattr) -
			      strlen("large_xattr");
	char *xattr = calloc(xattr_length, sizeof(char));
	SPDK_CU_ASSERT_FATAL(xattr != NULL);
	rc = spdk_blob_set_xattr(blob, "large_xattr", xattr, xattr_length);
	free(xattr);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	/* Resize the blob */
	spdk_blob_resize(blob, 10, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Set the blob as the super blob */
	spdk_bs_set_super(bs, blobid1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	free_clusters = spdk_bs_free_cluster_count(bs);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	blob = NULL;
	g_blob = NULL;
	g_blobid = SPDK_BLOBID_INVALID;

	ut_bs_dirty_load(&bs, NULL);

	/* Get the super blob */
	spdk_bs_get_super(bs, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(blobid1 == g_blobid);

	spdk_bs_open_blob(bs, blobid1, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));

	/* Get the xattrs */
	value = NULL;
	rc = spdk_blob_get_xattr_value(blob, "length", &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(*(uint64_t *)value == length);
	CU_ASSERT(value_len == 8);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 10);

	/* Resize the blob */
	spdk_blob_resize(blob, 20, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	free_clusters = spdk_bs_free_cluster_count(bs);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	blob = NULL;
	g_blob = NULL;
	g_blobid = SPDK_BLOBID_INVALID;

	ut_bs_dirty_load(&bs, NULL);

	spdk_bs_open_blob(bs, blobid1, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 20);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	blob = NULL;
	g_blob = NULL;
	g_blobid = SPDK_BLOBID_INVALID;

	/* Create second blob */
	blob = ut_blob_create_and_open(bs, NULL);
	blobid2 = spdk_blob_get_id(blob);

	/* Set some xattrs */
	rc = spdk_blob_set_xattr(blob, "name", "log1.txt", strlen("log1.txt") + 1);
	CU_ASSERT(rc == 0);

	length = 5432;
	rc = spdk_blob_set_xattr(blob, "length", &length, sizeof(length));
	CU_ASSERT(rc == 0);

	/* Resize the blob */
	spdk_blob_resize(blob, 10, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	free_clusters = spdk_bs_free_cluster_count(bs);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	blob = NULL;
	g_blob = NULL;
	g_blobid = SPDK_BLOBID_INVALID;

	ut_bs_dirty_load(&bs, NULL);

	spdk_bs_open_blob(bs, blobid2, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	/* Get the xattrs */
	value = NULL;
	rc = spdk_blob_get_xattr_value(blob, "length", &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(*(uint64_t *)value == length);
	CU_ASSERT(value_len == 8);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 10);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));

	ut_blob_close_and_delete(bs, blob);

	free_clusters = spdk_bs_free_cluster_count(bs);

	ut_bs_dirty_load(&bs, NULL);

	spdk_bs_open_blob(bs, blobid2, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno != 0);
	CU_ASSERT(g_blob == NULL);

	spdk_bs_open_blob(bs, blobid1, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	ut_bs_reload(&bs, NULL);

	/* Create second blob */
	ut_spdk_blob_opts_init(&blob_opts);
	spdk_bs_create_blob_ext(bs, &blob_opts, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid2 = g_blobid;

	/* Create third blob */
	spdk_bs_create_blob_ext(bs, &blob_opts, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid3 = g_blobid;

	spdk_bs_open_blob(bs, blobid2, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	/* Set some xattrs for second blob */
	rc = spdk_blob_set_xattr(blob, "name", "log1.txt", strlen("log1.txt") + 1);
	CU_ASSERT(rc == 0);

	length = 5432;
	rc = spdk_blob_set_xattr(blob, "length", &length, sizeof(length));
	CU_ASSERT(rc == 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	blob = NULL;
	g_blob = NULL;
	g_blobid = SPDK_BLOBID_INVALID;

	spdk_bs_open_blob(bs, blobid3, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	/* Set some xattrs for third blob */
	rc = spdk_blob_set_xattr(blob, "name", "log2.txt", strlen("log2.txt") + 1);
	CU_ASSERT(rc == 0);

	length = 5432;
	rc = spdk_blob_set_xattr(blob, "length", &length, sizeof(length));
	CU_ASSERT(rc == 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	blob = NULL;
	g_blob = NULL;
	g_blobid = SPDK_BLOBID_INVALID;

	/* Mark second blob as invalid */
	page_num = bs_blobid_to_page(blobid2);

	index = g_phys_blocklen * (bs->md_start + page_num);
	page = (struct spdk_blob_md_page *)&g_dev_buffer[index];
	page->sequence_num = 1;
	page->crc = blob_md_page_calc_crc(page);

	free_clusters = spdk_bs_free_cluster_count(bs);

	ut_bs_dirty_load(&bs, NULL);

	spdk_bs_open_blob(bs, blobid2, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno != 0);
	CU_ASSERT(g_blob == NULL);

	spdk_bs_open_blob(bs, blobid3, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
}

static void
blob_flags(void)
{
	struct spdk_blob_store *bs = g_bs;
	spdk_blob_id blobid_invalid, blobid_data_ro, blobid_md_ro;
	struct spdk_blob *blob_invalid, *blob_data_ro, *blob_md_ro;
	struct spdk_blob_opts blob_opts;
	int rc;

	/* Create three blobs - one each for testing invalid, data_ro and md_ro flags. */
	blob_invalid = ut_blob_create_and_open(bs, NULL);
	blobid_invalid = spdk_blob_get_id(blob_invalid);

	blob_data_ro = ut_blob_create_and_open(bs, NULL);
	blobid_data_ro = spdk_blob_get_id(blob_data_ro);

	ut_spdk_blob_opts_init(&blob_opts);
	blob_opts.clear_method = BLOB_CLEAR_WITH_WRITE_ZEROES;
	blob_md_ro = ut_blob_create_and_open(bs, &blob_opts);
	blobid_md_ro = spdk_blob_get_id(blob_md_ro);
	CU_ASSERT((blob_md_ro->md_ro_flags & SPDK_BLOB_MD_RO_FLAGS_MASK) == BLOB_CLEAR_WITH_WRITE_ZEROES);

	/* Change the size of blob_data_ro to check if flags are serialized
	 * when blob has non zero number of extents */
	spdk_blob_resize(blob_data_ro, 10, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Set the xattr to check if flags are serialized
	 * when blob has non zero number of xattrs */
	rc = spdk_blob_set_xattr(blob_md_ro, "name", "log.txt", strlen("log.txt") + 1);
	CU_ASSERT(rc == 0);

	blob_invalid->invalid_flags = (1ULL << 63);
	blob_invalid->state = SPDK_BLOB_STATE_DIRTY;
	blob_data_ro->data_ro_flags = (1ULL << 62);
	blob_data_ro->state = SPDK_BLOB_STATE_DIRTY;
	blob_md_ro->md_ro_flags = (1ULL << 61);
	blob_md_ro->state = SPDK_BLOB_STATE_DIRTY;

	g_bserrno = -1;
	spdk_blob_sync_md(blob_invalid, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bserrno = -1;
	spdk_blob_sync_md(blob_data_ro, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bserrno = -1;
	spdk_blob_sync_md(blob_md_ro, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	g_bserrno = -1;
	spdk_blob_close(blob_invalid, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	blob_invalid = NULL;
	g_bserrno = -1;
	spdk_blob_close(blob_data_ro, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	blob_data_ro = NULL;
	g_bserrno = -1;
	spdk_blob_close(blob_md_ro, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	blob_md_ro = NULL;

	g_blob = NULL;
	g_blobid = SPDK_BLOBID_INVALID;

	ut_bs_reload(&bs, NULL);

	g_blob = NULL;
	g_bserrno = 0;
	spdk_bs_open_blob(bs, blobid_invalid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno != 0);
	CU_ASSERT(g_blob == NULL);

	g_blob = NULL;
	g_bserrno = -1;
	spdk_bs_open_blob(bs, blobid_data_ro, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob_data_ro = g_blob;
	/* If an unknown data_ro flag was found, the blob should be marked both data and md read-only. */
	CU_ASSERT(blob_data_ro->data_ro == true);
	CU_ASSERT(blob_data_ro->md_ro == true);
	CU_ASSERT(spdk_blob_get_num_clusters(blob_data_ro) == 10);

	g_blob = NULL;
	g_bserrno = -1;
	spdk_bs_open_blob(bs, blobid_md_ro, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob_md_ro = g_blob;
	CU_ASSERT(blob_md_ro->data_ro == false);
	CU_ASSERT(blob_md_ro->md_ro == true);

	g_bserrno = -1;
	spdk_blob_sync_md(blob_md_ro, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	ut_blob_close_and_delete(bs, blob_data_ro);
	ut_blob_close_and_delete(bs, blob_md_ro);
}

static void
bs_version(void)
{
	struct spdk_bs_super_block *super;
	struct spdk_blob_store *bs = g_bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	struct spdk_blob_opts blob_opts;
	spdk_blob_id blobid;

	/* Unload the blob store */
	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;

	/*
	 * Change the bs version on disk.  This will allow us to
	 *  test that the version does not get modified automatically
	 *  when loading and unloading the blobstore.
	 */
	super = (struct spdk_bs_super_block *)&g_dev_buffer[0];
	CU_ASSERT(super->version == SPDK_BS_VERSION);
	CU_ASSERT(super->clean == 1);
	super->version = 2;
	/*
	 * Version 2 metadata does not have a used blobid mask, so clear
	 *  those fields in the super block and zero the corresponding
	 *  region on "disk".  We will use this to ensure blob IDs are
	 *  correctly reconstructed.
	 */
	memset(&g_dev_buffer[super->used_blobid_mask_start * SPDK_BS_PAGE_SIZE], 0,
	       super->used_blobid_mask_len * SPDK_BS_PAGE_SIZE);
	super->used_blobid_mask_start = 0;
	super->used_blobid_mask_len = 0;
	super->crc = blob_md_page_calc_crc(super);

	/* Load an existing blob store */
	dev = init_dev();
	spdk_bs_load(dev, NULL, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	CU_ASSERT(super->clean == 1);
	bs = g_bs;

	/*
	 * Create a blob - just to make sure that when we unload it
	 *  results in writing the super block (since metadata pages
	 *  were allocated.
	 */
	ut_spdk_blob_opts_init(&blob_opts);
	spdk_bs_create_blob_ext(bs, &blob_opts, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	/* Unload the blob store */
	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	CU_ASSERT(super->version == 2);
	CU_ASSERT(super->used_blobid_mask_start == 0);
	CU_ASSERT(super->used_blobid_mask_len == 0);

	dev = init_dev();
	spdk_bs_load(dev, NULL, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	g_blob = NULL;
	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	ut_blob_close_and_delete(bs, blob);

	CU_ASSERT(super->version == 2);
	CU_ASSERT(super->used_blobid_mask_start == 0);
	CU_ASSERT(super->used_blobid_mask_len == 0);
}

static void
blob_set_xattrs_test(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob;
	struct spdk_blob_opts opts;
	const void *value;
	size_t value_len;
	char *xattr;
	size_t xattr_length;
	int rc;

	/* Create blob with extra attributes */
	ut_spdk_blob_opts_init(&opts);

	opts.xattrs.names = g_xattr_names;
	opts.xattrs.get_value = _get_xattr_value;
	opts.xattrs.count = 3;
	opts.xattrs.ctx = &g_ctx;

	blob = ut_blob_create_and_open(bs, &opts);

	/* Get the xattrs */
	value = NULL;

	rc = spdk_blob_get_xattr_value(blob, g_xattr_names[0], &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(value_len == strlen(g_xattr_values[0]));
	CU_ASSERT_NSTRING_EQUAL_FATAL(value, g_xattr_values[0], value_len);

	rc = spdk_blob_get_xattr_value(blob, g_xattr_names[1], &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(value_len == strlen(g_xattr_values[1]));
	CU_ASSERT_NSTRING_EQUAL((char *)value, g_xattr_values[1], value_len);

	rc = spdk_blob_get_xattr_value(blob, g_xattr_names[2], &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(value_len == strlen(g_xattr_values[2]));
	CU_ASSERT_NSTRING_EQUAL((char *)value, g_xattr_values[2], value_len);

	/* Try to get non existing attribute */

	rc = spdk_blob_get_xattr_value(blob, "foobar", &value, &value_len);
	CU_ASSERT(rc == -ENOENT);

	/* Try xattr exceeding maximum length of descriptor in single page */
	xattr_length = SPDK_BS_MAX_DESC_SIZE - sizeof(struct spdk_blob_md_descriptor_xattr) -
		       strlen("large_xattr") + 1;
	xattr = calloc(xattr_length, sizeof(char));
	SPDK_CU_ASSERT_FATAL(xattr != NULL);
	rc = spdk_blob_set_xattr(blob, "large_xattr", xattr, xattr_length);
	free(xattr);
	SPDK_CU_ASSERT_FATAL(rc == -ENOMEM);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	blob = NULL;
	g_blob = NULL;
	g_blobid = SPDK_BLOBID_INVALID;

	/* NULL callback */
	ut_spdk_blob_opts_init(&opts);
	opts.xattrs.names = g_xattr_names;
	opts.xattrs.get_value = NULL;
	opts.xattrs.count = 1;
	opts.xattrs.ctx = &g_ctx;

	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EINVAL);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);

	/* NULL values */
	ut_spdk_blob_opts_init(&opts);
	opts.xattrs.names = g_xattr_names;
	opts.xattrs.get_value = _get_xattr_value_null;
	opts.xattrs.count = 1;
	opts.xattrs.ctx = NULL;

	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EINVAL);
}

static void
blob_thin_prov_alloc(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob;
	struct spdk_blob_opts opts;
	spdk_blob_id blobid;
	uint64_t free_clusters;

	free_clusters = spdk_bs_free_cluster_count(bs);

	/* Set blob as thin provisioned */
	ut_spdk_blob_opts_init(&opts);
	opts.thin_provision = true;

	blob = ut_blob_create_and_open(bs, &opts);
	blobid = spdk_blob_get_id(blob);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));

	CU_ASSERT(blob->active.num_clusters == 0);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 0);
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 0);

	/* The blob started at 0 clusters. Resize it to be 5, but still unallocated. */
	spdk_blob_resize(blob, 5, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob->active.num_clusters == 5);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 5);
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 0);

	/* Grow it to 1TB - still unallocated */
	spdk_blob_resize(blob, 262144, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob->active.num_clusters == 262144);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 262144);
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 0);

	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	/* Sync must not change anything */
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob->active.num_clusters == 262144);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 262144);
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 0);
	/* Since clusters are not allocated,
	 * number of metadata pages is expected to be minimal.
	 */
	CU_ASSERT(blob->active.num_pages == 1);

	/* Shrink the blob to 3 clusters - still unallocated */
	spdk_blob_resize(blob, 3, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob->active.num_clusters == 3);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 3);
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 0);

	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	/* Sync must not change anything */
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob->active.num_clusters == 3);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 3);
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	ut_bs_reload(&bs, NULL);

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	/* Check that clusters allocation and size is still the same */
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob->active.num_clusters == 3);

	ut_blob_close_and_delete(bs, blob);
}

static void
blob_insert_cluster_msg_test(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob;
	struct spdk_blob_opts opts;
	/* For now, even if md_page_size is > 4KB, we still only use the first
	 * 4KB of it. The rest is left unused. Future changes may allow using the
	 * rest of the md_page, but that will require more extensive changes since
	 * then the struct spdk_blob_md_page cannot be used directly (since some
	 * fields such as crc would have variable placement in the struct).
	 */
	struct {
		struct spdk_blob_md_page page;
		uint8_t pad[DEV_MAX_PHYS_BLOCKLEN - sizeof(struct spdk_blob_md_page)];
	} md = {};
	spdk_blob_id blobid;
	uint64_t free_clusters;
	uint64_t new_cluster = 0;
	uint32_t cluster_num = 3;
	uint32_t extent_page = 0;

	free_clusters = spdk_bs_free_cluster_count(bs);

	/* Set blob as thin provisioned */
	ut_spdk_blob_opts_init(&opts);
	opts.thin_provision = true;
	opts.num_clusters = 4;

	blob = ut_blob_create_and_open(bs, &opts);
	blobid = spdk_blob_get_id(blob);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));

	CU_ASSERT(blob->active.num_clusters == 4);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 4);
	CU_ASSERT(blob->active.clusters[cluster_num] == 0);

	/* Specify cluster_num to allocate and new_cluster will be returned to insert on md_thread.
	 * This is to simulate behaviour when cluster is allocated after blob creation.
	 * Such as _spdk_bs_allocate_and_copy_cluster(). */
	spdk_spin_lock(&bs->used_lock);
	bs_allocate_cluster(blob, cluster_num, &new_cluster, &extent_page, false);
	CU_ASSERT(blob->active.clusters[cluster_num] == 0);
	spdk_spin_unlock(&bs->used_lock);

	blob_insert_cluster_on_md_thread(blob, cluster_num, new_cluster, extent_page, &md.page,
					 blob_op_complete, NULL);
	poll_threads();

	CU_ASSERT(blob->active.clusters[cluster_num] != 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	ut_bs_reload(&bs, NULL);

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	CU_ASSERT(blob->active.clusters[cluster_num] != 0);

	ut_blob_close_and_delete(bs, blob);
}

static void
blob_thin_prov_rw(void)
{
	static const uint8_t zero[10 * BLOCKLEN] = { 0 };
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob, *blob_id0;
	struct spdk_io_channel *channel, *channel_thread1;
	struct spdk_blob_opts opts;
	uint64_t free_clusters;
	uint64_t io_unit_size;
	uint8_t payload_read[10 * BLOCKLEN];
	uint8_t payload_write[10 * BLOCKLEN];
	uint64_t write_bytes;
	uint64_t read_bytes;
	uint64_t expected_bytes;

	free_clusters = spdk_bs_free_cluster_count(bs);
	io_unit_size = spdk_bs_get_io_unit_size(bs);

	channel = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(channel != NULL);

	ut_spdk_blob_opts_init(&opts);
	opts.thin_provision = true;

	/* Create and delete blob at md page 0, so that next md page allocation
	 * for extent will use that. */
	blob_id0 = ut_blob_create_and_open(bs, &opts);
	blob = ut_blob_create_and_open(bs, &opts);
	ut_blob_close_and_delete(bs, blob_id0);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));

	CU_ASSERT(blob->active.num_clusters == 0);
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 0);

	/* The blob started at 0 clusters. Resize it to be 5, but still unallocated. */
	spdk_blob_resize(blob, 5, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob->active.num_clusters == 5);
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 0);

	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	/* Sync must not change anything */
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob->active.num_clusters == 5);
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 0);

	/* Payload should be all zeros from unallocated clusters */
	memset(payload_read, 0xFF, sizeof(payload_read));
	spdk_blob_io_read(blob, channel, payload_read, 4, 10, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(zero, payload_read, 10 * BLOCKLEN) == 0);

	write_bytes = g_dev_write_bytes;
	read_bytes = g_dev_read_bytes;

	/* Perform write on thread 1. That will allocate cluster on thread 0 via send_msg */
	set_thread(1);
	channel_thread1 = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(channel_thread1 != NULL);
	memset(payload_write, 0xE5, sizeof(payload_write));
	spdk_blob_io_write(blob, channel_thread1, payload_write, 4, 10, blob_op_complete, NULL);
	CU_ASSERT(free_clusters - 1 == spdk_bs_free_cluster_count(bs));
	/* Perform write on thread 0. That will try to allocate cluster,
	 * but fail due to another thread issuing the cluster allocation first. */
	set_thread(0);
	memset(payload_write, 0xE5, sizeof(payload_write));
	spdk_blob_io_write(blob, channel, payload_write, 4, 10, blob_op_complete, NULL);
	CU_ASSERT(free_clusters - 2 == spdk_bs_free_cluster_count(bs));
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(free_clusters - 1 == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 1);
	/* For thin-provisioned blob we need to write 20 io_units plus one page metadata and
	 * read 0 bytes */
	expected_bytes = 20 * io_unit_size + spdk_bs_get_page_size(bs);
	if (g_use_extent_table) {
		/* Add one more page for EXTENT_PAGE write */
		expected_bytes += spdk_bs_get_page_size(bs);
	}
	CU_ASSERT(g_dev_write_bytes - write_bytes == expected_bytes);
	CU_ASSERT(g_dev_read_bytes - read_bytes == 0);

	spdk_blob_io_read(blob, channel, payload_read, 4, 10, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_write, payload_read, 10 * BLOCKLEN) == 0);

	ut_blob_close_and_delete(bs, blob);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));

	set_thread(1);
	spdk_bs_free_io_channel(channel_thread1);
	set_thread(0);
	spdk_bs_free_io_channel(channel);
	poll_threads();
	g_blob = NULL;
	g_blobid = 0;
}

static void
blob_thin_prov_write_count_io(void)
{
	struct spdk_blob_store *bs;
	struct spdk_blob *blob;
	struct spdk_io_channel *ch;
	struct spdk_bs_dev *dev;
	struct spdk_bs_opts bs_opts;
	struct spdk_blob_opts opts;
	uint64_t free_clusters;
	uint64_t io_unit_size;
	uint8_t payload_write[BLOCKLEN];
	uint64_t write_bytes;
	uint64_t read_bytes;
	uint64_t expected_bytes;
	const uint32_t CLUSTER_SZ = g_phys_blocklen * 4;
	uint32_t io_units_per_cluster;
	uint32_t io_units_per_extent_page;
	uint32_t i;

	/* Use a very small cluster size for this test.  This ensures we need multiple
	 * extent pages to hold all of the clusters even for relatively small blobs like
	 * we are restricted to for the unit tests (i.e. we don't want to allocate multi-GB
	 * buffers).
	 */
	dev = init_dev();
	spdk_bs_opts_init(&bs_opts, sizeof(bs_opts));
	bs_opts.cluster_sz = CLUSTER_SZ;

	spdk_bs_init(dev, &bs_opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	free_clusters = spdk_bs_free_cluster_count(bs);
	io_unit_size = spdk_bs_get_io_unit_size(bs);
	io_units_per_cluster = CLUSTER_SZ / io_unit_size;
	io_units_per_extent_page = SPDK_EXTENTS_PER_EP * io_units_per_cluster;

	ch = spdk_bs_alloc_io_channel(bs);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	ut_spdk_blob_opts_init(&opts);
	opts.thin_provision = true;

	blob = ut_blob_create_and_open(bs, &opts);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));

	/* Resize the blob so that it will require 8 extent pages to hold all of
	 * the clusters.
	 */
	g_bserrno = -1;
	spdk_blob_resize(blob, SPDK_EXTENTS_PER_EP * 8, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	g_bserrno = -1;
	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob->active.num_clusters == SPDK_EXTENTS_PER_EP * 8);

	memset(payload_write, 0, sizeof(payload_write));
	for (i = 0; i < 8; i++) {
		write_bytes = g_dev_write_bytes;
		read_bytes = g_dev_read_bytes;

		g_bserrno = -1;
		spdk_blob_io_write(blob, ch, payload_write, io_units_per_extent_page * i, 1, blob_op_complete,
				   NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(free_clusters - (2 * i + 1) == spdk_bs_free_cluster_count(bs));

		CU_ASSERT(g_dev_read_bytes == read_bytes);
		if (!g_use_extent_table) {
			/* For legacy metadata, we should have written the io_unit for
			 * the write I/O, plus the blob's primary metadata page
			 */
			expected_bytes = io_unit_size + spdk_bs_get_page_size(bs);
		} else {
			/* For extent table metadata, we should have written the io_unit for
			 * the write I/O, plus 2 metadata pages - the extent page and the
			 * blob's primary metadata page
			 */
			expected_bytes = io_unit_size + 2 * spdk_bs_get_page_size(bs);
		}
		CU_ASSERT((g_dev_write_bytes - write_bytes) == expected_bytes);

		/* The write should have synced the metadata already.  Do another sync here
		 * just to confirm.
		 */
		write_bytes = g_dev_write_bytes;
		read_bytes = g_dev_read_bytes;

		g_bserrno = -1;
		spdk_blob_sync_md(blob, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(free_clusters - (2 * i + 1) == spdk_bs_free_cluster_count(bs));
		CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 2 * i + 1);

		CU_ASSERT(g_dev_read_bytes == read_bytes);
		CU_ASSERT(g_dev_write_bytes == write_bytes);

		/* Now write to another unallocated cluster that is part of the same extent page. */
		g_bserrno = -1;
		spdk_blob_io_write(blob, ch, payload_write, io_units_per_extent_page * i + io_units_per_cluster,
				   1, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(free_clusters - (2 * i + 2) == spdk_bs_free_cluster_count(bs));
		CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 2 * i + 2);

		CU_ASSERT(g_dev_read_bytes == read_bytes);
		/*
		 * For legacy metadata, we should have written the I/O and the primary metadata page.
		 * For extent table metadata, we should have written the I/O and the extent metadata page.
		 */
		expected_bytes = io_unit_size + spdk_bs_get_page_size(bs);
		CU_ASSERT((g_dev_write_bytes - write_bytes) == expected_bytes);

		/* Send unmap aligned to the whole cluster - should free it up */
		g_bserrno = -1;
		spdk_blob_io_unmap(blob, ch, io_units_per_extent_page * i, io_units_per_cluster, blob_op_complete,
				   NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(free_clusters - (2 * i + 1) == spdk_bs_free_cluster_count(bs));

		/* Write back to the freed cluster */
		g_bserrno = -1;
		spdk_blob_io_write(blob, ch, payload_write, io_units_per_extent_page * i, 1, blob_op_complete,
				   NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(free_clusters - (2 * i + 2) == spdk_bs_free_cluster_count(bs));
	}

	ut_blob_close_and_delete(bs, blob);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));

	spdk_bs_free_io_channel(ch);
	poll_threads();
	g_blob = NULL;
	g_blobid = 0;

	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_thin_prov_unmap_cluster(void)
{
	struct spdk_blob_store *bs;
	struct spdk_blob *blob, *snapshot;
	struct spdk_io_channel *ch;
	struct spdk_bs_dev *dev;
	struct spdk_bs_opts bs_opts;
	struct spdk_blob_opts opts;
	uint64_t free_clusters;
	uint64_t io_unit_size;
	uint8_t payload_write[BLOCKLEN];
	uint8_t payload_read[BLOCKLEN];
	const uint32_t CLUSTER_COUNT = 3;
	uint32_t io_units_per_cluster;
	spdk_blob_id blobid, snapshotid;
	uint32_t i;
	int err;

	/* Use a very large cluster size for this test. Check how the unmap/release cluster code path behaves when
	 * clusters are fully used.
	 */
	dev = init_dev();
	spdk_bs_opts_init(&bs_opts, sizeof(bs_opts));
	bs_opts.cluster_sz = dev->blocklen * dev->blockcnt / (CLUSTER_COUNT + 1);

	spdk_bs_init(dev, &bs_opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	free_clusters = spdk_bs_free_cluster_count(bs);
	io_unit_size = spdk_bs_get_io_unit_size(bs);
	io_units_per_cluster = bs_opts.cluster_sz / io_unit_size;

	ch = spdk_bs_alloc_io_channel(bs);
	SPDK_CU_ASSERT_FATAL(ch != NULL);

	ut_spdk_blob_opts_init(&opts);
	opts.thin_provision = true;

	blob = ut_blob_create_and_open(bs, &opts);
	CU_ASSERT(free_clusters == CLUSTER_COUNT);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	blobid = spdk_blob_get_id(blob);

	g_bserrno = -1;
	spdk_blob_resize(blob, CLUSTER_COUNT, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	g_bserrno = -1;
	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob->active.num_clusters == CLUSTER_COUNT);

	/* Triggers a potential out-of-bounds access on blob->active.clusters,
	 * when checking whether a extPage could be freed */
	g_bserrno = -1;
	spdk_blob_io_write(blob, ch, payload_write, 0, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(CLUSTER_COUNT - 1 == spdk_bs_free_cluster_count(bs));

	g_bserrno = -1;
	spdk_blob_io_unmap(blob, ch, 0, io_units_per_cluster, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(CLUSTER_COUNT == spdk_bs_free_cluster_count(bs));

	/* Fill all clusters */
	for (i = 0; i < CLUSTER_COUNT; i++) {
		memset(payload_write, i + 1, sizeof(payload_write));
		g_bserrno = -1;
		spdk_blob_io_write(blob, ch, payload_write, io_units_per_cluster * i, 1, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(free_clusters - (i + 1) == spdk_bs_free_cluster_count(bs));
	}
	CU_ASSERT(0 == spdk_bs_free_cluster_count(bs));

	/* Unmap one whole cluster */
	g_bserrno = -1;
	spdk_blob_io_unmap(blob, ch, io_units_per_cluster, io_units_per_cluster, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(1 == spdk_bs_free_cluster_count(bs));

	/* Verify the data read from the cluster is zeroed out */
	memset(payload_write, 0, sizeof(payload_write));
	spdk_blob_io_read(blob, ch, payload_read, io_units_per_cluster, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_write, payload_read, BLOCKLEN) == 0);

	/* Fill the same cluster with data */
	memset(payload_write, 3, sizeof(payload_write));
	g_bserrno = -1;
	spdk_blob_io_write(blob, ch, payload_write, io_units_per_cluster, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(0 == spdk_bs_free_cluster_count(bs));

	/* Verify the data read from the cluster has the expected data */
	spdk_blob_io_read(blob, ch, payload_read, io_units_per_cluster, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_write, payload_read, BLOCKLEN) == 0);

	/* Send an unaligned unmap that ecompasses one whole cluster */
	g_bserrno = -1;
	spdk_blob_io_unmap(blob, ch, io_units_per_cluster - 1, io_units_per_cluster + 2, blob_op_complete,
			   NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(1 == spdk_bs_free_cluster_count(bs));

	/* Verify the data read from the cluster is zeroed out */
	g_bserrno = -1;
	memset(payload_write, 0, sizeof(payload_write));
	spdk_blob_io_read(blob, ch, payload_read, io_units_per_cluster, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_write, payload_read, BLOCKLEN) == 0);

	/* Send a simultaneous unmap with a write to an unallocated area -
	 * check that writes don't claim the currently unmapped cluster */
	g_bserrno = -1;
	memset(payload_write, 7, sizeof(payload_write));
	spdk_blob_io_unmap(blob, ch, 0, io_units_per_cluster, blob_op_complete, NULL);
	spdk_blob_io_write(blob, ch, payload_write, io_units_per_cluster, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(1 == spdk_bs_free_cluster_count(bs));

	/* Verify the contents of written sector */
	g_bserrno = -1;
	spdk_blob_io_read(blob, ch, payload_read, io_units_per_cluster, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_write, payload_read, BLOCKLEN) == 0);

	/* Verify the contents of unmapped sector */
	g_bserrno = -1;
	memset(payload_write, 0, sizeof(payload_write));
	spdk_blob_io_read(blob, ch, payload_read, 0, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_write, payload_read, BLOCKLEN) == 0);

	/* Make sure clusters are not freed until the unmap to the drive is done */
	g_bserrno = -1;
	memset(payload_write, 7, sizeof(payload_write));
	spdk_blob_io_write(blob, ch, payload_write, 0, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(0 == spdk_bs_free_cluster_count(bs));

	g_bserrno = -1;
	spdk_blob_io_unmap(blob, ch, 0, io_units_per_cluster, blob_op_complete, NULL);
	while (memcmp(payload_write, &g_dev_buffer[BLOCKLEN * io_units_per_cluster], BLOCKLEN) == 0) {
		CU_ASSERT(0 == spdk_bs_free_cluster_count(bs));
		poll_thread_times(0, 1);
	}
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(1 == spdk_bs_free_cluster_count(bs));

	/* Issue #3358 had a bug with concurrent trims to the same cluster causing an assert, check for regressions.
	 * Send three concurrent unmaps to the same cluster.
	 */
	g_bserrno = -1;
	memset(payload_write, 7, sizeof(payload_write));
	spdk_blob_io_write(blob, ch, payload_write, 0, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(0 == spdk_bs_free_cluster_count(bs));

	g_bserrno = -1;
	err = -1;
	spdk_blob_io_unmap(blob, ch, 0, io_units_per_cluster, blob_op_complete, NULL);
	spdk_blob_io_unmap(blob, ch, 0, io_units_per_cluster, blob_op_complete, NULL);
	spdk_blob_io_unmap(blob, ch, 0, io_units_per_cluster, blob_op_complete, &err);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(err == 0);
	CU_ASSERT(1 == spdk_bs_free_cluster_count(bs));

	/* Test thin-provisioned blob that is backed */
	spdk_blob_resize(blob, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));

	g_bserrno = -1;
	memset(payload_write, 1, sizeof(payload_write));
	spdk_blob_io_write(blob, ch, payload_write, 0, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(free_clusters - 1 == spdk_bs_free_cluster_count(bs));

	/* Create a snapshot */
	CU_ASSERT_EQUAL(_get_snapshots_count(bs), 0);
	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	CU_ASSERT_EQUAL(_get_snapshots_count(bs), 1);
	snapshotid = g_blobid;
	CU_ASSERT(free_clusters - 1 == spdk_bs_free_cluster_count(bs));
	spdk_bs_open_blob(bs, snapshotid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot = g_blob;

	/* Write data to blob, it will alloc new cluster */
	g_bserrno = -1;
	memset(payload_write, 2, sizeof(payload_write));
	spdk_blob_io_write(blob, ch, payload_write, 0, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(free_clusters - 2 == spdk_bs_free_cluster_count(bs));

	/* Unmap one whole cluster, but do not release this cluster */
	g_bserrno = -1;
	spdk_blob_io_unmap(blob, ch, 0, io_units_per_cluster, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(free_clusters - 2 == spdk_bs_free_cluster_count(bs));

	/* Verify the data read from the cluster is zeroed out */
	g_bserrno = -1;
	memset(payload_write, 0, sizeof(payload_write));
	spdk_blob_io_read(blob, ch, payload_read, 0, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_write, payload_read, BLOCKLEN) == 0);

	ut_blob_close_and_delete(bs, blob);
	ut_blob_close_and_delete(bs, snapshot);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));

	spdk_bs_free_io_channel(ch);
	poll_threads();
	g_blob = NULL;
	g_blobid = 0;

	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_thin_prov_rle(void)
{
	static const uint8_t zero[10 * BLOCKLEN] = { 0 };
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob;
	struct spdk_io_channel *channel;
	struct spdk_blob_opts opts;
	spdk_blob_id blobid;
	uint64_t free_clusters;
	uint64_t io_unit_size;
	uint8_t payload_read[10 * BLOCKLEN];
	uint8_t payload_write[10 * BLOCKLEN];
	uint64_t write_bytes;
	uint64_t read_bytes;
	uint64_t expected_bytes;
	uint64_t io_unit;

	/* assert that the stack variables above are of correct size */
	CU_ASSERT(spdk_bs_get_io_unit_size(bs) == BLOCKLEN);

	free_clusters = spdk_bs_free_cluster_count(bs);
	io_unit_size = spdk_bs_get_io_unit_size(bs);

	ut_spdk_blob_opts_init(&opts);
	opts.thin_provision = true;
	opts.num_clusters = 5;

	blob = ut_blob_create_and_open(bs, &opts);
	blobid = spdk_blob_get_id(blob);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));

	channel = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(channel != NULL);

	/* Target specifically second cluster in a blob as first allocation */
	io_unit = bs_cluster_to_io_unit(bs, 1);

	/* Payload should be all zeros from unallocated clusters */
	memset(payload_read, 0xFF, sizeof(payload_read));
	spdk_blob_io_read(blob, channel, payload_read, io_unit, 10, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(zero, payload_read, 10 * BLOCKLEN) == 0);

	write_bytes = g_dev_write_bytes;
	read_bytes = g_dev_read_bytes;

	/* Issue write to second cluster in a blob */
	memset(payload_write, 0xE5, sizeof(payload_write));
	spdk_blob_io_write(blob, channel, payload_write, io_unit, 10, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(free_clusters - 1 == spdk_bs_free_cluster_count(bs));
	/* For thin-provisioned blob we need to write 10 pages plus one page metadata and
	 * read 0 bytes */
	expected_bytes = 10 * io_unit_size + spdk_bs_get_page_size(bs);
	if (g_use_extent_table) {
		/* Add one more page for EXTENT_PAGE write */
		expected_bytes += spdk_bs_get_page_size(bs);
	}
	CU_ASSERT(g_dev_write_bytes - write_bytes == expected_bytes);
	CU_ASSERT(g_dev_read_bytes - read_bytes == 0);

	spdk_blob_io_read(blob, channel, payload_read, io_unit, 10, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_write, payload_read, 10 * BLOCKLEN) == 0);

	spdk_bs_free_io_channel(channel);
	poll_threads();

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	ut_bs_reload(&bs, NULL);

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	channel = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(channel != NULL);

	/* Read second cluster after blob reload to confirm data written */
	spdk_blob_io_read(blob, channel, payload_read, io_unit, 10, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_write, payload_read, 10 * BLOCKLEN) == 0);

	spdk_bs_free_io_channel(channel);
	poll_threads();

	ut_blob_close_and_delete(bs, blob);
}

static void
blob_thin_prov_rw_iov(void)
{
	static const uint8_t zero[10 * BLOCKLEN] = { 0 };
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob;
	struct spdk_io_channel *channel;
	struct spdk_blob_opts opts;
	uint64_t free_clusters;
	uint8_t payload_read[10 * BLOCKLEN];
	uint8_t payload_write[10 * BLOCKLEN];
	struct iovec iov_read[3];
	struct iovec iov_write[3];

	free_clusters = spdk_bs_free_cluster_count(bs);

	channel = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(channel != NULL);

	ut_spdk_blob_opts_init(&opts);
	opts.thin_provision = true;

	blob = ut_blob_create_and_open(bs, &opts);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));

	CU_ASSERT(blob->active.num_clusters == 0);

	/* The blob started at 0 clusters. Resize it to be 5, but still unallocated. */
	spdk_blob_resize(blob, 5, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob->active.num_clusters == 5);

	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	/* Sync must not change anything */
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob->active.num_clusters == 5);

	/* Payload should be all zeros from unallocated clusters */
	memset(payload_read, 0xAA, sizeof(payload_read));
	iov_read[0].iov_base = payload_read;
	iov_read[0].iov_len = 3 * BLOCKLEN;
	iov_read[1].iov_base = payload_read + 3 * BLOCKLEN;
	iov_read[1].iov_len = 4 * BLOCKLEN;
	iov_read[2].iov_base = payload_read + 7 * BLOCKLEN;
	iov_read[2].iov_len = 3 * BLOCKLEN;
	spdk_blob_io_readv(blob, channel, iov_read, 3, 250, 10, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(zero, payload_read, 10 * BLOCKLEN) == 0);

	memset(payload_write, 0xE5, sizeof(payload_write));
	iov_write[0].iov_base = payload_write;
	iov_write[0].iov_len = 1 * BLOCKLEN;
	iov_write[1].iov_base = payload_write + 1 * BLOCKLEN;
	iov_write[1].iov_len = 5 * BLOCKLEN;
	iov_write[2].iov_base = payload_write + 6 * BLOCKLEN;
	iov_write[2].iov_len = 4 * BLOCKLEN;

	spdk_blob_io_writev(blob, channel, iov_write, 3, 250, 10, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	memset(payload_read, 0xAA, sizeof(payload_read));
	iov_read[0].iov_base = payload_read;
	iov_read[0].iov_len = 3 * BLOCKLEN;
	iov_read[1].iov_base = payload_read + 3 * BLOCKLEN;
	iov_read[1].iov_len = 4 * BLOCKLEN;
	iov_read[2].iov_base = payload_read + 7 * BLOCKLEN;
	iov_read[2].iov_len = 3 * BLOCKLEN;
	spdk_blob_io_readv(blob, channel, iov_read, 3, 250, 10, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_write, payload_read, 10 * BLOCKLEN) == 0);

	spdk_bs_free_io_channel(channel);
	poll_threads();

	ut_blob_close_and_delete(bs, blob);
}

struct iter_ctx {
	int		current_iter;
	spdk_blob_id	blobid[4];
};

static void
test_iter(void *arg, struct spdk_blob *blob, int bserrno)
{
	struct iter_ctx *iter_ctx = arg;
	spdk_blob_id blobid;

	CU_ASSERT(bserrno == 0);
	blobid = spdk_blob_get_id(blob);
	CU_ASSERT(blobid == iter_ctx->blobid[iter_ctx->current_iter++]);
}

static void
bs_load_iter_test(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct iter_ctx iter_ctx = { 0 };
	struct spdk_blob *blob;
	int i, rc;
	struct spdk_bs_opts opts;

	dev = init_dev();
	spdk_bs_opts_init(&opts, sizeof(opts));
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");

	/* Initialize a new blob store */
	spdk_bs_init(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	for (i = 0; i < 4; i++) {
		blob = ut_blob_create_and_open(bs, NULL);
		iter_ctx.blobid[i] = spdk_blob_get_id(blob);

		/* Just save the blobid as an xattr for testing purposes. */
		rc = spdk_blob_set_xattr(blob, "blobid", &iter_ctx.blobid[i], sizeof(spdk_blob_id));
		CU_ASSERT(rc == 0);

		/* Resize the blob */
		spdk_blob_resize(blob, i, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);

		spdk_blob_close(blob, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
	}

	g_bserrno = -1;
	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	dev = init_dev();
	spdk_bs_opts_init(&opts, sizeof(opts));
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");
	opts.iter_cb_fn = test_iter;
	opts.iter_cb_arg = &iter_ctx;

	/* Test blob iteration during load after a clean shutdown. */
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	/* Dirty shutdown */
	bs_free(bs);

	dev = init_dev();
	spdk_bs_opts_init(&opts, sizeof(opts));
	snprintf(opts.bstype.bstype, sizeof(opts.bstype.bstype), "TESTTYPE");
	opts.iter_cb_fn = test_iter;
	iter_ctx.current_iter = 0;
	opts.iter_cb_arg = &iter_ctx;

	/* Test blob iteration during load after a dirty shutdown. */
	spdk_bs_load(dev, &opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_snapshot_rw(void)
{
	static const uint8_t zero[10 * BLOCKLEN] = { 0 };
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob, *snapshot;
	struct spdk_io_channel *channel;
	struct spdk_blob_opts opts;
	spdk_blob_id blobid, snapshotid;
	uint64_t free_clusters;
	uint64_t cluster_size;
	uint64_t io_unit_size;
	uint8_t payload_read[10 * BLOCKLEN];
	uint8_t payload_write[10 * BLOCKLEN];
	uint64_t write_bytes_start;
	uint64_t read_bytes_start;
	uint64_t copy_bytes_start;
	uint64_t write_bytes;
	uint64_t read_bytes;
	uint64_t copy_bytes;
	uint64_t expected_bytes;

	free_clusters = spdk_bs_free_cluster_count(bs);
	cluster_size = spdk_bs_get_cluster_size(bs);
	io_unit_size = spdk_bs_get_io_unit_size(bs);

	channel = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(channel != NULL);

	ut_spdk_blob_opts_init(&opts);
	opts.thin_provision = true;
	opts.num_clusters = 5;

	blob = ut_blob_create_and_open(bs, &opts);
	blobid = spdk_blob_get_id(blob);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));

	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 5);

	memset(payload_read, 0xFF, sizeof(payload_read));
	spdk_blob_io_read(blob, channel, payload_read, 4, 10, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(zero, payload_read, 10 * BLOCKLEN) == 0);

	memset(payload_write, 0xE5, sizeof(payload_write));
	spdk_blob_io_write(blob, channel, payload_write, 4, 10, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(free_clusters != spdk_bs_free_cluster_count(bs));

	/* Create snapshot from blob */
	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snapshotid = g_blobid;

	spdk_bs_open_blob(bs, snapshotid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot = g_blob;
	CU_ASSERT(snapshot->data_ro == true);
	CU_ASSERT(snapshot->md_ro == true);

	CU_ASSERT(spdk_blob_get_num_clusters(snapshot) == 5);

	write_bytes_start = g_dev_write_bytes;
	read_bytes_start = g_dev_read_bytes;
	copy_bytes_start = g_dev_copy_bytes;

	memset(payload_write, 0xAA, sizeof(payload_write));
	spdk_blob_io_write(blob, channel, payload_write, 4, 10, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(free_clusters != spdk_bs_free_cluster_count(bs));

	/* For a clone we need to allocate and copy one cluster, update one page of metadata
	 * and then write 10 io units of payload.
	 */
	write_bytes = g_dev_write_bytes - write_bytes_start;
	read_bytes = g_dev_read_bytes - read_bytes_start;
	copy_bytes = g_dev_copy_bytes - copy_bytes_start;
	if (g_dev_copy_enabled) {
		CU_ASSERT(copy_bytes == cluster_size);
	} else {
		CU_ASSERT(copy_bytes == 0);
	}
	expected_bytes = 10 * io_unit_size + cluster_size + spdk_bs_get_page_size(bs);
	if (g_use_extent_table) {
		/* Add one more page for EXTENT_PAGE write */
		expected_bytes += spdk_bs_get_page_size(bs);
	}
	CU_ASSERT(write_bytes + copy_bytes == expected_bytes);
	CU_ASSERT(read_bytes + copy_bytes == cluster_size);

	spdk_blob_io_read(blob, channel, payload_read, 4, 10, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_write, payload_read, 10 * BLOCKLEN) == 0);

	/* Data on snapshot should not change after write to clone */
	memset(payload_write, 0xE5, sizeof(payload_write));
	spdk_blob_io_read(snapshot, channel, payload_read, 4, 10, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_write, payload_read, 10 * BLOCKLEN) == 0);

	ut_blob_close_and_delete(bs, blob);
	ut_blob_close_and_delete(bs, snapshot);

	spdk_bs_free_io_channel(channel);
	poll_threads();
	g_blob = NULL;
	g_blobid = 0;
}

static void
blob_snapshot_rw_iov(void)
{
	static const uint8_t zero[10 * BLOCKLEN] = { 0 };
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob, *snapshot;
	struct spdk_io_channel *channel;
	struct spdk_blob_opts opts;
	spdk_blob_id blobid, snapshotid;
	uint64_t free_clusters;
	uint8_t payload_read[10 * BLOCKLEN];
	uint8_t payload_write[10 * BLOCKLEN];
	struct iovec iov_read[3];
	struct iovec iov_write[3];

	free_clusters = spdk_bs_free_cluster_count(bs);

	channel = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(channel != NULL);

	ut_spdk_blob_opts_init(&opts);
	opts.thin_provision = true;
	opts.num_clusters = 5;

	blob = ut_blob_create_and_open(bs, &opts);
	blobid = spdk_blob_get_id(blob);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));

	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 5);

	/* Create snapshot from blob */
	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snapshotid = g_blobid;

	spdk_bs_open_blob(bs, snapshotid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot = g_blob;
	CU_ASSERT(snapshot->data_ro == true);
	CU_ASSERT(snapshot->md_ro == true);
	CU_ASSERT(spdk_blob_get_num_clusters(snapshot) == 5);

	/* Payload should be all zeros from unallocated clusters */
	memset(payload_read, 0xAA, sizeof(payload_read));
	iov_read[0].iov_base = payload_read;
	iov_read[0].iov_len = 3 * BLOCKLEN;
	iov_read[1].iov_base = payload_read + 3 * BLOCKLEN;
	iov_read[1].iov_len = 4 * BLOCKLEN;
	iov_read[2].iov_base = payload_read + 7 * BLOCKLEN;
	iov_read[2].iov_len = 3 * BLOCKLEN;
	spdk_blob_io_readv(blob, channel, iov_read, 3, 250, 10, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(zero, payload_read, 10 * BLOCKLEN) == 0);

	memset(payload_write, 0xE5, sizeof(payload_write));
	iov_write[0].iov_base = payload_write;
	iov_write[0].iov_len = 1 * BLOCKLEN;
	iov_write[1].iov_base = payload_write + 1 * BLOCKLEN;
	iov_write[1].iov_len = 5 * BLOCKLEN;
	iov_write[2].iov_base = payload_write + 6 * BLOCKLEN;
	iov_write[2].iov_len = 4 * BLOCKLEN;

	spdk_blob_io_writev(blob, channel, iov_write, 3, 250, 10, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	memset(payload_read, 0xAA, sizeof(payload_read));
	iov_read[0].iov_base = payload_read;
	iov_read[0].iov_len = 3 * BLOCKLEN;
	iov_read[1].iov_base = payload_read + 3 * BLOCKLEN;
	iov_read[1].iov_len = 4 * BLOCKLEN;
	iov_read[2].iov_base = payload_read + 7 * BLOCKLEN;
	iov_read[2].iov_len = 3 * BLOCKLEN;
	spdk_blob_io_readv(blob, channel, iov_read, 3, 250, 10, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_write, payload_read, 10 * BLOCKLEN) == 0);

	spdk_bs_free_io_channel(channel);
	poll_threads();

	ut_blob_close_and_delete(bs, blob);
	ut_blob_close_and_delete(bs, snapshot);
}

/**
 * Inflate / decouple parent rw unit tests.
 *
 * --------------
 * original blob:         0         1         2         3         4
 *                   ,---------+---------+---------+---------+---------.
 *         snapshot  |xxxxxxxxx|xxxxxxxxx|xxxxxxxxx|xxxxxxxxx|    -    |
 *                   +---------+---------+---------+---------+---------+
 *         snapshot2 |    -    |yyyyyyyyy|    -    |yyyyyyyyy|    -    |
 *                   +---------+---------+---------+---------+---------+
 *         blob      |    -    |zzzzzzzzz|    -    |    -    |    -    |
 *                   '---------+---------+---------+---------+---------'
 *                   .         .         .         .         .         .
 * --------          .         .         .         .         .         .
 * inflate:          .         .         .         .         .         .
 *                   ,---------+---------+---------+---------+---------.
 *         blob      |xxxxxxxxx|zzzzzzzzz|xxxxxxxxx|yyyyyyyyy|000000000|
 *                   '---------+---------+---------+---------+---------'
 *
 *         NOTE: needs to allocate 4 clusters, thin provisioning removed, dependency
 *               on snapshot2 and snapshot removed .         .         .
 *                   .         .         .         .         .         .
 * ----------------  .         .         .         .         .         .
 * decouple parent:  .         .         .         .         .         .
 *                   ,---------+---------+---------+---------+---------.
 *         snapshot  |xxxxxxxxx|xxxxxxxxx|xxxxxxxxx|xxxxxxxxx|    -    |
 *                   +---------+---------+---------+---------+---------+
 *         blob      |    -    |zzzzzzzzz|    -    |yyyyyyyyy|    -    |
 *                   '---------+---------+---------+---------+---------'
 *
 *         NOTE: needs to allocate 1 cluster, 3 clusters unallocated, dependency
 *               on snapshot2 removed and on snapshot still exists. Snapshot2
 *               should remain a clone of snapshot.
 */
static void
_blob_inflate_rw(bool decouple_parent)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob, *snapshot, *snapshot2;
	struct spdk_io_channel *channel;
	struct spdk_blob_opts opts;
	spdk_blob_id blobid, snapshotid, snapshot2id;
	uint64_t free_clusters;
	uint64_t cluster_size;

	uint64_t payload_size;
	uint8_t *payload_read;
	uint8_t *payload_write;
	uint8_t *payload_clone;

	uint64_t io_units_per_cluster;
	uint64_t io_units_per_payload;

	int i;
	spdk_blob_id ids[2];
	size_t count;

	free_clusters = spdk_bs_free_cluster_count(bs);
	cluster_size = spdk_bs_get_cluster_size(bs);
	io_units_per_cluster = cluster_size / spdk_bs_get_io_unit_size(bs);
	io_units_per_payload = io_units_per_cluster * 5;

	payload_size = cluster_size * 5;

	payload_read = malloc(payload_size);
	SPDK_CU_ASSERT_FATAL(payload_read != NULL);

	payload_write = malloc(payload_size);
	SPDK_CU_ASSERT_FATAL(payload_write != NULL);

	payload_clone = malloc(payload_size);
	SPDK_CU_ASSERT_FATAL(payload_clone != NULL);

	channel = spdk_bs_alloc_io_channel(bs);
	SPDK_CU_ASSERT_FATAL(channel != NULL);

	/* Create blob */
	ut_spdk_blob_opts_init(&opts);
	opts.thin_provision = true;
	opts.num_clusters = 5;

	blob = ut_blob_create_and_open(bs, &opts);
	blobid = spdk_blob_get_id(blob);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));

	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 5);

	/* 1) Initial read should return zeroed payload */
	memset(payload_read, 0xFF, payload_size);
	spdk_blob_io_read(blob, channel, payload_read, 0, io_units_per_payload,
			  blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(spdk_mem_all_zero(payload_read, payload_size));

	/* Fill whole blob with a pattern, except last cluster (to be sure it
	 * isn't allocated) */
	memset(payload_write, 0xE5, payload_size - cluster_size);
	spdk_blob_io_write(blob, channel, payload_write, 0, io_units_per_payload -
			   io_units_per_cluster, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(free_clusters != spdk_bs_free_cluster_count(bs));

	/* 2) Create snapshot from blob (first level) */
	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snapshotid = g_blobid;

	spdk_bs_open_blob(bs, snapshotid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot = g_blob;
	CU_ASSERT(snapshot->data_ro == true);
	CU_ASSERT(snapshot->md_ro == true);

	CU_ASSERT(spdk_blob_get_num_clusters(snapshot) == 5);

	/* Write every second cluster with a pattern.
	 *
	 * Last cluster shouldn't be written, to be sure that snapshot nor clone
	 * doesn't allocate it.
	 *
	 * payload_clone stores expected result on "blob" read at the time and
	 * is used only to check data consistency on clone before and after
	 * inflation. Initially we fill it with a backing snapshots pattern
	 * used before.
	 */
	memset(payload_clone, 0xE5, payload_size - cluster_size);
	memset(payload_clone + payload_size - cluster_size, 0x00, cluster_size);
	memset(payload_write, 0xAA, payload_size);
	for (i = 1; i < 5; i += 2) {
		spdk_blob_io_write(blob, channel, payload_write, i * io_units_per_cluster,
				   io_units_per_cluster, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);

		/* Update expected result */
		memcpy(payload_clone + (cluster_size * i), payload_write,
		       cluster_size);
	}
	CU_ASSERT(free_clusters != spdk_bs_free_cluster_count(bs));

	/* Check data consistency on clone */
	memset(payload_read, 0xFF, payload_size);
	spdk_blob_io_read(blob, channel, payload_read, 0, io_units_per_payload,
			  blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_clone, payload_read, payload_size) == 0);

	/* 3) Create second levels snapshot from blob */
	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snapshot2id = g_blobid;

	spdk_bs_open_blob(bs, snapshot2id, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot2 = g_blob;
	CU_ASSERT(snapshot2->data_ro == true);
	CU_ASSERT(snapshot2->md_ro == true);

	CU_ASSERT(spdk_blob_get_num_clusters(snapshot2) == 5);

	CU_ASSERT(snapshot2->parent_id == snapshotid);

	/* Write one cluster on the top level blob. This cluster (1) covers
	 * already allocated cluster in the snapshot2, so shouldn't be inflated
	 * at all */
	spdk_blob_io_write(blob, channel, payload_write, io_units_per_cluster,
			   io_units_per_cluster, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Update expected result */
	memcpy(payload_clone + cluster_size, payload_write, cluster_size);

	/* Check data consistency on clone */
	memset(payload_read, 0xFF, payload_size);
	spdk_blob_io_read(blob, channel, payload_read, 0, io_units_per_payload,
			  blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_clone, payload_read, payload_size) == 0);


	/* Close all blobs */
	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(snapshot2, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(snapshot, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Check snapshot-clone relations */
	count = 2;
	CU_ASSERT(spdk_blob_get_clones(bs, snapshotid, ids, &count) == 0);
	CU_ASSERT(count == 1);
	CU_ASSERT(ids[0] == snapshot2id);

	count = 2;
	CU_ASSERT(spdk_blob_get_clones(bs, snapshot2id, ids, &count) == 0);
	CU_ASSERT(count == 1);
	CU_ASSERT(ids[0] == blobid);

	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, blobid) == snapshot2id);

	free_clusters = spdk_bs_free_cluster_count(bs);
	if (!decouple_parent) {
		/* Do full blob inflation */
		spdk_bs_inflate_blob(bs, channel, blobid, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);

		/* All clusters should be inflated (except one already allocated
		 * in a top level blob) */
		CU_ASSERT(spdk_bs_free_cluster_count(bs) == free_clusters - 4);

		/* Check if relation tree updated correctly */
		count = 2;
		CU_ASSERT(spdk_blob_get_clones(bs, snapshotid, ids, &count) == 0);

		/* snapshotid have one clone */
		CU_ASSERT(count == 1);
		CU_ASSERT(ids[0] == snapshot2id);

		/* snapshot2id have no clones */
		count = 2;
		CU_ASSERT(spdk_blob_get_clones(bs, snapshot2id, ids, &count) == 0);
		CU_ASSERT(count == 0);

		CU_ASSERT(spdk_blob_get_parent_snapshot(bs, blobid) == SPDK_BLOBID_INVALID);
	} else {
		/* Decouple parent of blob */
		spdk_bs_blob_decouple_parent(bs, channel, blobid, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);

		/* Only one cluster from a parent should be inflated (second one
		 * is covered by a cluster written on a top level blob, and
		 * already allocated) */
		CU_ASSERT(spdk_bs_free_cluster_count(bs) == free_clusters - 1);

		/* Check if relation tree updated correctly */
		count = 2;
		CU_ASSERT(spdk_blob_get_clones(bs, snapshotid, ids, &count) == 0);

		/* snapshotid have two clones now */
		CU_ASSERT(count == 2);
		CU_ASSERT(ids[0] == blobid || ids[1] == blobid);
		CU_ASSERT(ids[0] == snapshot2id || ids[1] == snapshot2id);

		/* snapshot2id have no clones */
		count = 2;
		CU_ASSERT(spdk_blob_get_clones(bs, snapshot2id, ids, &count) == 0);
		CU_ASSERT(count == 0);

		CU_ASSERT(spdk_blob_get_parent_snapshot(bs, blobid) == snapshotid);
	}

	/* Try to delete snapshot2 (should pass) */
	spdk_bs_delete_blob(bs, snapshot2id, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Try to delete base snapshot */
	spdk_bs_delete_blob(bs, snapshotid, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Reopen blob after snapshot deletion */
	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 5);

	/* Check data consistency on inflated blob */
	memset(payload_read, 0xFF, payload_size);
	spdk_blob_io_read(blob, channel, payload_read, 0, io_units_per_payload,
			  blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_clone, payload_read, payload_size) == 0);

	spdk_bs_free_io_channel(channel);
	poll_threads();

	free(payload_read);
	free(payload_write);
	free(payload_clone);

	ut_blob_close_and_delete(bs, blob);
}

static void
blob_inflate_rw(void)
{
	_blob_inflate_rw(false);
	_blob_inflate_rw(true);
}

static void
blob_inflate_unmap(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob_opts opts;
	struct spdk_blob *blob;
	spdk_blob_id blobid;
	struct spdk_io_channel *channel;
	uint64_t io_units_per_cluster;
	uint8_t	*payload;
	uint32_t payload_size;

	channel = spdk_bs_alloc_io_channel(bs);
	SPDK_CU_ASSERT_FATAL(channel != NULL);

	/* Create blob with 10 clusters */
	ut_spdk_blob_opts_init(&opts);
	opts.num_clusters = 10;
	opts.thin_provision = true;

	blob = ut_blob_create_and_open(bs, &opts);
	blobid = spdk_blob_get_id(blob);
	io_units_per_cluster = spdk_bs_get_cluster_size(bs) / spdk_bs_get_io_unit_size(bs);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 10);
	CU_ASSERT(spdk_blob_is_thin_provisioned(blob) == true);
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 0);

	payload_size = opts.num_clusters * spdk_bs_get_cluster_size(bs);
	payload = malloc(payload_size);
	SPDK_CU_ASSERT_FATAL(payload != NULL);

	spdk_blob_io_write(blob, channel, payload, 0,
			   io_units_per_cluster, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 1);

	/* During inflate, submit unmap with cluster size, the cluster should not be release to ensure the blob is thick */
	spdk_bs_inflate_blob(bs, channel, blobid, blob_op_complete, NULL);
	spdk_blob_io_unmap(blob, channel, 0, io_units_per_cluster,
			   blob_op_complete, NULL);
	poll_threads();

	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(spdk_blob_is_thin_provisioned(blob) == false);
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 10);

	spdk_bs_free_io_channel(channel);
	poll_threads();

	free(payload);

	ut_blob_close_and_delete(bs, blob);
}

/**
 * Snapshot-clones relation test
 *
 *         snapshot
 *            |
 *      +-----+-----+
 *      |           |
 *   blob(ro)   snapshot2
 *      |           |
 *   clone2      clone
 */
static void
blob_relations(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_bs_opts bs_opts;
	struct spdk_blob_opts opts;
	struct spdk_blob *blob, *snapshot, *snapshot2, *clone, *clone2;
	spdk_blob_id blobid, cloneid, snapshotid, cloneid2, snapshotid2;
	int rc;
	size_t count;
	spdk_blob_id ids[10] = {};

	dev = init_dev();
	spdk_bs_opts_init(&bs_opts, sizeof(bs_opts));
	snprintf(bs_opts.bstype.bstype, sizeof(bs_opts.bstype.bstype), "TESTTYPE");

	spdk_bs_init(dev, &bs_opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	/* 1. Create blob with 10 clusters */

	ut_spdk_blob_opts_init(&opts);
	opts.num_clusters = 10;

	blob = ut_blob_create_and_open(bs, &opts);
	blobid = spdk_blob_get_id(blob);

	CU_ASSERT(!spdk_blob_is_read_only(blob));
	CU_ASSERT(!spdk_blob_is_snapshot(blob));
	CU_ASSERT(!spdk_blob_is_clone(blob));
	CU_ASSERT(!spdk_blob_is_thin_provisioned(blob));

	/* blob should not have underlying snapshot nor clones */
	CU_ASSERT(blob->parent_id == SPDK_BLOBID_INVALID);
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, blobid) == SPDK_BLOBID_INVALID);
	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, blobid, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 0);


	/* 2. Create snapshot */

	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snapshotid = g_blobid;

	spdk_bs_open_blob(bs, snapshotid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot = g_blob;

	CU_ASSERT(spdk_blob_is_read_only(snapshot));
	CU_ASSERT(spdk_blob_is_snapshot(snapshot));
	CU_ASSERT(!spdk_blob_is_clone(snapshot));
	CU_ASSERT(snapshot->parent_id == SPDK_BLOBID_INVALID);
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, snapshotid) == SPDK_BLOBID_INVALID);

	/* Check if original blob is converted to the clone of snapshot */
	CU_ASSERT(!spdk_blob_is_read_only(blob));
	CU_ASSERT(!spdk_blob_is_snapshot(blob));
	CU_ASSERT(spdk_blob_is_clone(blob));
	CU_ASSERT(spdk_blob_is_thin_provisioned(blob));
	CU_ASSERT(blob->parent_id == snapshotid);
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, blobid) == snapshotid);

	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, snapshotid, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 1);
	CU_ASSERT(ids[0] == blobid);


	/* 3. Create clone from snapshot */

	spdk_bs_create_clone(bs, snapshotid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	cloneid = g_blobid;

	spdk_bs_open_blob(bs, cloneid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	clone = g_blob;

	CU_ASSERT(!spdk_blob_is_read_only(clone));
	CU_ASSERT(!spdk_blob_is_snapshot(clone));
	CU_ASSERT(spdk_blob_is_clone(clone));
	CU_ASSERT(spdk_blob_is_thin_provisioned(clone));
	CU_ASSERT(clone->parent_id == snapshotid);
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, cloneid) == snapshotid);

	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, cloneid, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 0);

	/* Check if clone is on the snapshot's list */
	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, snapshotid, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ids[0] == blobid || ids[1] == blobid);
	CU_ASSERT(ids[0] == cloneid || ids[1] == cloneid);


	/* 4. Create snapshot of the clone */

	spdk_bs_create_snapshot(bs, cloneid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snapshotid2 = g_blobid;

	spdk_bs_open_blob(bs, snapshotid2, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot2 = g_blob;

	CU_ASSERT(spdk_blob_is_read_only(snapshot2));
	CU_ASSERT(spdk_blob_is_snapshot(snapshot2));
	CU_ASSERT(spdk_blob_is_clone(snapshot2));
	CU_ASSERT(snapshot2->parent_id == snapshotid);
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, snapshotid2) == snapshotid);

	/* Check if clone is converted to the clone of snapshot2 and snapshot2
	 * is a child of snapshot */
	CU_ASSERT(!spdk_blob_is_read_only(clone));
	CU_ASSERT(!spdk_blob_is_snapshot(clone));
	CU_ASSERT(spdk_blob_is_clone(clone));
	CU_ASSERT(spdk_blob_is_thin_provisioned(clone));
	CU_ASSERT(clone->parent_id == snapshotid2);
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, cloneid) == snapshotid2);

	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, snapshotid2, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 1);
	CU_ASSERT(ids[0] == cloneid);


	/* 5. Try to create clone from read only blob */

	/* Mark blob as read only */
	spdk_blob_set_read_only(blob);
	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Check if previously created blob is read only clone */
	CU_ASSERT(spdk_blob_is_read_only(blob));
	CU_ASSERT(!spdk_blob_is_snapshot(blob));
	CU_ASSERT(spdk_blob_is_clone(blob));
	CU_ASSERT(spdk_blob_is_thin_provisioned(blob));

	/* Create clone from read only blob */
	spdk_bs_create_clone(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	cloneid2 = g_blobid;

	spdk_bs_open_blob(bs, cloneid2, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	clone2 = g_blob;

	CU_ASSERT(!spdk_blob_is_read_only(clone2));
	CU_ASSERT(!spdk_blob_is_snapshot(clone2));
	CU_ASSERT(spdk_blob_is_clone(clone2));
	CU_ASSERT(spdk_blob_is_thin_provisioned(clone2));

	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, cloneid2) == blobid);

	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, blobid, ids, &count);
	CU_ASSERT(rc == 0);

	CU_ASSERT(count == 1);
	CU_ASSERT(ids[0] == cloneid2);

	/* Close blobs */

	spdk_blob_close(clone2, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(clone, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(snapshot, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(snapshot2, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Try to delete snapshot with more than 1 clone */
	spdk_bs_delete_blob(bs, snapshotid, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno != 0);

	ut_bs_reload(&bs, &bs_opts);

	/* NULL ids array should return number of clones in count */
	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, snapshotid, NULL, &count);
	CU_ASSERT(rc == -ENOMEM);
	CU_ASSERT(count == 2);

	/* incorrect array size */
	count = 1;
	rc = spdk_blob_get_clones(bs, snapshotid, ids, &count);
	CU_ASSERT(rc == -ENOMEM);
	CU_ASSERT(count == 2);


	/* Verify structure of loaded blob store */

	/* snapshot */
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, snapshotid) == SPDK_BLOBID_INVALID);

	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, snapshotid, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 2);
	CU_ASSERT(ids[0] == blobid || ids[1] == blobid);
	CU_ASSERT(ids[0] == snapshotid2 || ids[1] == snapshotid2);

	/* blob */
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, blobid) == snapshotid);
	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, blobid, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 1);
	CU_ASSERT(ids[0] == cloneid2);

	/* clone */
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, cloneid) == snapshotid2);
	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, cloneid, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 0);

	/* snapshot2 */
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, snapshotid2) == snapshotid);
	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, snapshotid2, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 1);
	CU_ASSERT(ids[0] == cloneid);

	/* clone2 */
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, cloneid2) == blobid);
	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, cloneid2, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 0);

	/* Try to delete blob that user should not be able to remove */

	spdk_bs_delete_blob(bs, snapshotid, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno != 0);

	/* Remove all blobs */

	spdk_bs_delete_blob(bs, cloneid, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_delete_blob(bs, snapshotid2, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_delete_blob(bs, cloneid2, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_delete_blob(bs, blobid, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_delete_blob(bs, snapshotid, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	g_bs = NULL;
}

/**
 * Snapshot-clones relation test 2
 *
 *         snapshot1
 *            |
 *         snapshot2
 *            |
 *      +-----+-----+
 *      |           |
 *   blob(ro)   snapshot3
 *      |           |
 *      |       snapshot4
 *      |        |     |
 *   clone2   clone  clone3
 */
static void
blob_relations2(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_bs_opts bs_opts;
	struct spdk_blob_opts opts;
	struct spdk_blob *blob, *snapshot1, *snapshot2, *snapshot3, *snapshot4, *clone, *clone2;
	spdk_blob_id blobid, snapshotid1, snapshotid2, snapshotid3, snapshotid4, cloneid, cloneid2,
		     cloneid3;
	int rc;
	size_t count;
	spdk_blob_id ids[10] = {};

	dev = init_dev();
	spdk_bs_opts_init(&bs_opts, sizeof(bs_opts));
	snprintf(bs_opts.bstype.bstype, sizeof(bs_opts.bstype.bstype), "TESTTYPE");

	spdk_bs_init(dev, &bs_opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	/* 1. Create blob with 10 clusters */

	ut_spdk_blob_opts_init(&opts);
	opts.num_clusters = 10;

	blob = ut_blob_create_and_open(bs, &opts);
	blobid = spdk_blob_get_id(blob);

	/* 2. Create snapshot1 */

	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snapshotid1 = g_blobid;

	spdk_bs_open_blob(bs, snapshotid1, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot1 = g_blob;

	CU_ASSERT(snapshot1->parent_id == SPDK_BLOBID_INVALID);
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, snapshotid1) == SPDK_BLOBID_INVALID);

	CU_ASSERT(blob->parent_id == snapshotid1);
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, blobid) == snapshotid1);

	/* Check if blob is the clone of snapshot1 */
	CU_ASSERT(blob->parent_id == snapshotid1);
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, blobid) == snapshotid1);

	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, snapshotid1, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 1);
	CU_ASSERT(ids[0] == blobid);

	/* 3. Create another snapshot */

	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snapshotid2 = g_blobid;

	spdk_bs_open_blob(bs, snapshotid2, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot2 = g_blob;

	CU_ASSERT(spdk_blob_is_clone(snapshot2));
	CU_ASSERT(snapshot2->parent_id == snapshotid1);
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, snapshotid2) == snapshotid1);

	/* Check if snapshot2 is the clone of snapshot1 and blob
	 * is a child of snapshot2 */
	CU_ASSERT(blob->parent_id == snapshotid2);
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, blobid) == snapshotid2);

	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, snapshotid2, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 1);
	CU_ASSERT(ids[0] == blobid);

	/* 4. Create clone from snapshot */

	spdk_bs_create_clone(bs, snapshotid2, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	cloneid = g_blobid;

	spdk_bs_open_blob(bs, cloneid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	clone = g_blob;

	CU_ASSERT(clone->parent_id == snapshotid2);
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, cloneid) == snapshotid2);

	/* Check if clone is on the snapshot's list */
	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, snapshotid2, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 2);
	CU_ASSERT(ids[0] == blobid || ids[1] == blobid);
	CU_ASSERT(ids[0] == cloneid || ids[1] == cloneid);

	/* 5. Create snapshot of the clone */

	spdk_bs_create_snapshot(bs, cloneid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snapshotid3 = g_blobid;

	spdk_bs_open_blob(bs, snapshotid3, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot3 = g_blob;

	CU_ASSERT(snapshot3->parent_id == snapshotid2);
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, snapshotid3) == snapshotid2);

	/* Check if clone is converted to the clone of snapshot3 and snapshot3
	 * is a child of snapshot2 */
	CU_ASSERT(clone->parent_id == snapshotid3);
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, cloneid) == snapshotid3);

	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, snapshotid3, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 1);
	CU_ASSERT(ids[0] == cloneid);

	/* 6. Create another snapshot of the clone */

	spdk_bs_create_snapshot(bs, cloneid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snapshotid4 = g_blobid;

	spdk_bs_open_blob(bs, snapshotid4, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot4 = g_blob;

	CU_ASSERT(snapshot4->parent_id == snapshotid3);
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, snapshotid4) == snapshotid3);

	/* Check if clone is converted to the clone of snapshot4 and snapshot4
	 * is a child of snapshot3 */
	CU_ASSERT(clone->parent_id == snapshotid4);
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, cloneid) == snapshotid4);

	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, snapshotid4, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 1);
	CU_ASSERT(ids[0] == cloneid);

	/* 7. Remove snapshot 4 */

	ut_blob_close_and_delete(bs, snapshot4);

	/* Check if relations are back to state from before creating snapshot 4 */
	CU_ASSERT(clone->parent_id == snapshotid3);
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, cloneid) == snapshotid3);

	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, snapshotid3, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 1);
	CU_ASSERT(ids[0] == cloneid);

	/* 8. Create second clone of snapshot 3 and try to remove snapshot 3 */

	spdk_bs_create_clone(bs, snapshotid3, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	cloneid3 = g_blobid;

	spdk_bs_delete_blob(bs, snapshotid3, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno != 0);

	/* 9. Open snapshot 3 again and try to remove it while clone 3 is closed */

	spdk_bs_open_blob(bs, snapshotid3, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot3 = g_blob;

	spdk_bs_delete_blob(bs, snapshotid3, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno != 0);

	spdk_blob_close(snapshot3, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_delete_blob(bs, cloneid3, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* 10. Remove snapshot 1 */

	/* Check snapshot 1 and snapshot 2 allocated clusters */
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(snapshot1) == 10);
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(snapshot2) == 0);

	ut_blob_close_and_delete(bs, snapshot1);

	/* Check if relations are back to state from before creating snapshot 4 (before step 6) */
	CU_ASSERT(snapshot2->parent_id == SPDK_BLOBID_INVALID);
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, snapshotid2) == SPDK_BLOBID_INVALID);

	/* Check that snapshot 2 has the clusters that were allocated to snapshot 1 */
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(snapshot2) == 10);

	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, snapshotid2, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 2);
	CU_ASSERT(ids[0] == blobid || ids[1] == blobid);
	CU_ASSERT(ids[0] == snapshotid3 || ids[1] == snapshotid3);

	/* 11. Try to create clone from read only blob */

	/* Mark blob as read only */
	spdk_blob_set_read_only(blob);
	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Create clone from read only blob */
	spdk_bs_create_clone(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	cloneid2 = g_blobid;

	spdk_bs_open_blob(bs, cloneid2, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	clone2 = g_blob;

	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, cloneid2) == blobid);

	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, blobid, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 1);
	CU_ASSERT(ids[0] == cloneid2);

	/* Close blobs */

	spdk_blob_close(clone2, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(clone, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(snapshot2, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(snapshot3, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	ut_bs_reload(&bs, &bs_opts);

	/* Verify structure of loaded blob store */

	/* snapshot2 */
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, snapshotid2) == SPDK_BLOBID_INVALID);

	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, snapshotid2, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 2);
	CU_ASSERT(ids[0] == blobid || ids[1] == blobid);
	CU_ASSERT(ids[0] == snapshotid3 || ids[1] == snapshotid3);

	/* blob */
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, blobid) == snapshotid2);
	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, blobid, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 1);
	CU_ASSERT(ids[0] == cloneid2);

	/* clone */
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, cloneid) == snapshotid3);
	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, cloneid, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 0);

	/* snapshot3 */
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, snapshotid3) == snapshotid2);
	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, snapshotid3, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 1);
	CU_ASSERT(ids[0] == cloneid);

	/* clone2 */
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, cloneid2) == blobid);
	count = SPDK_COUNTOF(ids);
	rc = spdk_blob_get_clones(bs, cloneid2, ids, &count);
	CU_ASSERT(rc == 0);
	CU_ASSERT(count == 0);

	/* Try to delete all blobs in the worse possible order */

	spdk_bs_delete_blob(bs, snapshotid2, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno != 0);

	spdk_bs_delete_blob(bs, snapshotid3, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_delete_blob(bs, snapshotid2, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno != 0);

	spdk_bs_delete_blob(bs, cloneid, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_delete_blob(bs, snapshotid2, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_delete_blob(bs, blobid, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_delete_blob(bs, cloneid2, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	g_bs = NULL;
}

/**
 * Snapshot-clones relation test 3
 *
 *         snapshot0
 *            |
 *         snapshot1
 *            |
 *         snapshot2
 *            |
 *           blob
 */
static void
blob_relations3(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_io_channel *channel;
	struct spdk_bs_opts bs_opts;
	struct spdk_blob_opts opts;
	struct spdk_blob *blob;
	spdk_blob_id blobid, snapshotid0, snapshotid1, snapshotid2;

	dev = init_dev();
	spdk_bs_opts_init(&bs_opts, sizeof(bs_opts));
	snprintf(bs_opts.bstype.bstype, sizeof(bs_opts.bstype.bstype), "TESTTYPE");

	spdk_bs_init(dev, &bs_opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	channel = spdk_bs_alloc_io_channel(bs);
	SPDK_CU_ASSERT_FATAL(channel != NULL);

	/* 1. Create blob with 10 clusters */
	ut_spdk_blob_opts_init(&opts);
	opts.num_clusters = 10;

	blob = ut_blob_create_and_open(bs, &opts);
	blobid = spdk_blob_get_id(blob);

	/* 2. Create snapshot0 */
	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snapshotid0 = g_blobid;

	/* 3. Create snapshot1 */
	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snapshotid1 = g_blobid;

	/* 4. Create snapshot2 */
	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snapshotid2 = g_blobid;

	/* 5. Decouple blob */
	spdk_bs_blob_decouple_parent(bs, channel, blobid, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* 6. Decouple snapshot2. Make sure updating md of snapshot2 is possible */
	spdk_bs_blob_decouple_parent(bs, channel, snapshotid2, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* 7. Delete blob */
	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_delete_blob(bs, blobid, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* 8. Delete snapshot2.
	 * If md of snapshot 2 was updated, it should be possible to delete it */
	spdk_bs_delete_blob(bs, snapshotid2, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Remove remaining blobs and unload bs */
	spdk_bs_delete_blob(bs, snapshotid1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_delete_blob(bs, snapshotid0, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_free_io_channel(channel);
	poll_threads();

	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	g_bs = NULL;
}

static void
blobstore_clean_power_failure(void)
{
	struct spdk_blob_store *bs;
	struct spdk_blob *blob;
	struct spdk_power_failure_thresholds thresholds = {};
	bool clean = false;
	struct spdk_bs_super_block *super = (struct spdk_bs_super_block *)&g_dev_buffer[0];
	struct spdk_bs_super_block super_copy = {};

	thresholds.general_threshold = 1;
	while (!clean) {
		/* Create bs and blob */
		suite_blob_setup();
		SPDK_CU_ASSERT_FATAL(g_bs != NULL);
		SPDK_CU_ASSERT_FATAL(g_blob != NULL);
		bs = g_bs;
		blob = g_blob;

		/* Super block should not change for rest of the UT,
		 * save it and compare later. */
		memcpy(&super_copy, super, sizeof(struct spdk_bs_super_block));
		SPDK_CU_ASSERT_FATAL(super->clean == 0);
		SPDK_CU_ASSERT_FATAL(bs->clean == 0);

		/* Force bs/super block in a clean state.
		 * Along with marking blob dirty, to cause blob persist. */
		blob->state = SPDK_BLOB_STATE_DIRTY;
		bs->clean = 1;
		super->clean = 1;
		super->crc = blob_md_page_calc_crc(super);

		g_bserrno = -1;
		dev_set_power_failure_thresholds(thresholds);
		spdk_blob_sync_md(blob, blob_op_complete, NULL);
		poll_threads();
		dev_reset_power_failure_event();

		if (g_bserrno == 0) {
			/* After successful md sync, both bs and super block
			 * should be marked as not clean. */
			SPDK_CU_ASSERT_FATAL(bs->clean == 0);
			SPDK_CU_ASSERT_FATAL(super->clean == 0);
			clean = true;
		}

		/* Depending on the point of failure, super block was either updated or not. */
		super_copy.clean = super->clean;
		super_copy.crc = blob_md_page_calc_crc(&super_copy);
		/* Compare that the values in super block remained unchanged. */
		SPDK_CU_ASSERT_FATAL(!memcmp(&super_copy, super, sizeof(struct spdk_bs_super_block)));

		/* Delete blob and unload bs */
		suite_blob_cleanup();

		thresholds.general_threshold++;
	}
}

static void
blob_delete_snapshot_power_failure(void)
{
	struct spdk_bs_dev *dev;
	struct spdk_blob_store *bs;
	struct spdk_blob_opts opts;
	struct spdk_blob *blob, *snapshot;
	struct spdk_power_failure_thresholds thresholds = {};
	spdk_blob_id blobid, snapshotid;
	const void *value;
	size_t value_len;
	size_t count;
	spdk_blob_id ids[3] = {};
	int rc;
	bool deleted = false;
	int delete_snapshot_bserrno = -1;
	uint32_t first_data_cluster;

	thresholds.general_threshold = 1;
	while (!deleted) {
		dev = init_dev();

		spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		SPDK_CU_ASSERT_FATAL(g_bs != NULL);
		bs = g_bs;

		first_data_cluster = FIRST_DATA_CLUSTER(bs);

		/* Create blob */
		ut_spdk_blob_opts_init(&opts);
		opts.num_clusters = 10;

		spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
		blobid = g_blobid;

		/* Create snapshot */
		spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
		snapshotid = g_blobid;
		SPDK_CU_ASSERT_FATAL(spdk_bit_pool_is_allocated(bs->used_clusters, first_data_cluster));
		SPDK_CU_ASSERT_FATAL(!spdk_bit_pool_is_allocated(bs->used_clusters, first_data_cluster + 10));

		dev_set_power_failure_thresholds(thresholds);

		spdk_bs_delete_blob(bs, snapshotid, blob_op_complete, NULL);
		poll_threads();
		delete_snapshot_bserrno = g_bserrno;

		/* Do not shut down cleanly. Assumption is that after snapshot deletion
		 * reports success, changes to both blobs should already persisted. */
		dev_reset_power_failure_event();
		ut_bs_dirty_load(&bs, NULL);

		SPDK_CU_ASSERT_FATAL(spdk_bit_pool_is_allocated(bs->used_clusters, first_data_cluster));
		SPDK_CU_ASSERT_FATAL(!spdk_bit_pool_is_allocated(bs->used_clusters, first_data_cluster + 10));

		spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		SPDK_CU_ASSERT_FATAL(g_blob != NULL);
		blob = g_blob;
		SPDK_CU_ASSERT_FATAL(spdk_blob_is_thin_provisioned(blob) == true);

		spdk_bs_open_blob(bs, snapshotid, blob_op_with_handle_complete, NULL);
		poll_threads();

		if (g_bserrno == 0) {
			SPDK_CU_ASSERT_FATAL(g_blob != NULL);
			snapshot = g_blob;
			CU_ASSERT(spdk_blob_get_parent_snapshot(bs, blobid) == snapshotid);
			count = SPDK_COUNTOF(ids);
			rc = spdk_blob_get_clones(bs, snapshotid, ids, &count);
			CU_ASSERT(rc == 0);
			CU_ASSERT(count == 1);
			CU_ASSERT(ids[0] == blobid);
			rc = spdk_blob_get_xattr_value(snapshot, SNAPSHOT_PENDING_REMOVAL, &value, &value_len);
			CU_ASSERT(rc != 0);
			SPDK_CU_ASSERT_FATAL(spdk_blob_is_thin_provisioned(snapshot) == false);
			CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 0);
			CU_ASSERT(spdk_blob_get_num_allocated_clusters(snapshot) == 10);

			spdk_blob_close(snapshot, blob_op_complete, NULL);
			poll_threads();
			CU_ASSERT(g_bserrno == 0);
		} else {
			CU_ASSERT(spdk_blob_get_parent_snapshot(bs, blobid) == SPDK_BLOBID_INVALID);
			CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 10);
			/* Snapshot might have been left in unrecoverable state, so it does not open.
			 * Yet delete might perform further changes to the clone after that.
			 * This UT should test until snapshot is deleted and delete call succeeds. */
			if (delete_snapshot_bserrno == 0) {
				deleted = true;
			}
		}

		spdk_blob_close(blob, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);

		spdk_bs_unload(bs, bs_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);

		thresholds.general_threshold++;
	}
}

static void
blob_create_snapshot_power_failure(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob_opts opts;
	struct spdk_blob *blob, *snapshot;
	struct spdk_power_failure_thresholds thresholds = {};
	spdk_blob_id blobid, snapshotid;
	const void *value;
	size_t value_len;
	size_t count;
	spdk_blob_id ids[3] = {};
	int rc;
	bool created = false;
	int create_snapshot_bserrno = -1;
	uint32_t first_data_cluster;

	thresholds.general_threshold = 1;
	while (!created) {
		dev = init_dev();

		spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		SPDK_CU_ASSERT_FATAL(g_bs != NULL);
		bs = g_bs;

		first_data_cluster = FIRST_DATA_CLUSTER(bs);

		/* Create blob */
		ut_spdk_blob_opts_init(&opts);
		opts.num_clusters = 10;

		spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
		blobid = g_blobid;
		SPDK_CU_ASSERT_FATAL(spdk_bit_pool_is_allocated(bs->used_clusters, first_data_cluster));
		SPDK_CU_ASSERT_FATAL(!spdk_bit_pool_is_allocated(bs->used_clusters, first_data_cluster + 10));

		dev_set_power_failure_thresholds(thresholds);

		/* Create snapshot */
		spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
		poll_threads();
		create_snapshot_bserrno = g_bserrno;
		snapshotid = g_blobid;
		SPDK_CU_ASSERT_FATAL(spdk_bit_pool_is_allocated(bs->used_clusters, first_data_cluster));
		SPDK_CU_ASSERT_FATAL(!spdk_bit_pool_is_allocated(bs->used_clusters, first_data_cluster + 10));

		/* Do not shut down cleanly. Assumption is that after create snapshot
		 * reports success, both blobs should be power-fail safe. */
		dev_reset_power_failure_event();
		ut_bs_dirty_load(&bs, NULL);

		SPDK_CU_ASSERT_FATAL(spdk_bit_pool_is_allocated(bs->used_clusters, first_data_cluster));
		SPDK_CU_ASSERT_FATAL(!spdk_bit_pool_is_allocated(bs->used_clusters, first_data_cluster + 10));

		spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		SPDK_CU_ASSERT_FATAL(g_blob != NULL);
		blob = g_blob;

		if (snapshotid != SPDK_BLOBID_INVALID) {
			spdk_bs_open_blob(bs, snapshotid, blob_op_with_handle_complete, NULL);
			poll_threads();
		}

		if ((snapshotid != SPDK_BLOBID_INVALID) && (g_bserrno == 0)) {
			SPDK_CU_ASSERT_FATAL(g_blob != NULL);
			snapshot = g_blob;
			SPDK_CU_ASSERT_FATAL(spdk_blob_is_thin_provisioned(blob) == true);
			SPDK_CU_ASSERT_FATAL(spdk_blob_is_thin_provisioned(snapshot) == false);
			CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 0);
			CU_ASSERT(spdk_blob_get_num_allocated_clusters(snapshot) == 10);
			CU_ASSERT(spdk_blob_get_parent_snapshot(bs, blobid) == snapshotid);
			count = SPDK_COUNTOF(ids);
			rc = spdk_blob_get_clones(bs, snapshotid, ids, &count);
			CU_ASSERT(rc == 0);
			CU_ASSERT(count == 1);
			CU_ASSERT(ids[0] == blobid);
			rc = spdk_blob_get_xattr_value(snapshot, SNAPSHOT_IN_PROGRESS, &value, &value_len);
			CU_ASSERT(rc != 0);

			spdk_blob_close(snapshot, blob_op_complete, NULL);
			poll_threads();
			CU_ASSERT(g_bserrno == 0);
			if (create_snapshot_bserrno == 0) {
				created = true;
			}
		} else {
			CU_ASSERT(spdk_blob_get_parent_snapshot(bs, blobid) == SPDK_BLOBID_INVALID);
			SPDK_CU_ASSERT_FATAL(spdk_blob_is_thin_provisioned(blob) == false);
			CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 10);
		}

		spdk_blob_close(blob, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);

		spdk_bs_unload(bs, bs_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);

		thresholds.general_threshold++;
	}
}

#define IO_UT_BLOCKS_PER_CLUSTER 64

static void
test_io_write(struct spdk_bs_dev *dev, struct spdk_blob *blob, struct spdk_io_channel *channel)
{
	const uint32_t SZ = IO_UT_BLOCKS_PER_CLUSTER;
	uint8_t payload_ff[SZ * 512];
	uint8_t payload_aa[SZ * 512];
	uint8_t payload_00[SZ * 512];
	uint8_t *cluster0, *cluster1;

	memset(payload_ff, 0xFF, sizeof(payload_ff));
	memset(payload_aa, 0xAA, sizeof(payload_aa));
	memset(payload_00, 0x00, sizeof(payload_00));

	/* Try to perform I/O with io unit = 512 */
	spdk_blob_io_write(blob, channel, payload_ff, 0, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* If thin provisioned is set cluster should be allocated now */
	SPDK_CU_ASSERT_FATAL(blob->active.clusters[0] != 0);
	cluster0 = &g_dev_buffer[blob->active.clusters[0] * dev->blocklen];

	/* Each character 0-F symbolizes single io_unit containing 512 bytes block filled with that character.
	* Each page is separated by |. Whole block [...] symbolizes one cluster (containing 4 pages). */
	/* cluster0: [ F000 0000 | 0000 0000 | 0000 0000 | 0000 0000 ] */
	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 1 * 512, payload_00, (SZ - 1) * 512) == 0);

	/* Verify write with offset on first page */
	spdk_blob_io_write(blob, channel, payload_ff, 2, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* cluster0: [ F0F0 0000 | 0000 0000 | 0000 0000 | 0000 0000 ] */
	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 2 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 3 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 4 * 512, payload_00, (SZ - 4) * 512) == 0);

	/* Verify write with offset on first page */
	spdk_blob_io_write(blob, channel, payload_ff, 4, 4, blob_op_complete, NULL);
	poll_threads();

	/* cluster0: [ F0F0 FFFF | 0000 0000 | 0000 0000 | 0000 0000 ] */
	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 2 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 3 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 4 * 512, payload_ff, 4 * 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 8 * 512, payload_00, (SZ - 8) * 512) == 0);

	/* Verify write with offset on second page */
	spdk_blob_io_write(blob, channel, payload_ff, 8, 4, blob_op_complete, NULL);
	poll_threads();

	/* cluster0: [ F0F0 FFFF | FFFF 0000 | 0000 0000 | 0000 0000 ] */
	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 2 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 3 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 4 * 512, payload_ff, 8 * 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 12 * 512, payload_00, (SZ - 12) * 512) == 0);

	/* Verify write across multiple pages */
	spdk_blob_io_write(blob, channel, payload_aa, 4, 8, blob_op_complete, NULL);
	poll_threads();

	/* cluster0: [ F0F0 AAAA | AAAA 0000 | 0000 0000 | 0000 0000 ] */
	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 2 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 3 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 4 * 512, payload_aa, 8 * 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 12 * 512, payload_00, (SZ - 12) * 512) == 0);

	/* Verify write across multiple clusters */
	spdk_blob_io_write(blob, channel, payload_ff, SZ - 4, 8, blob_op_complete, NULL);
	poll_threads();

	SPDK_CU_ASSERT_FATAL(blob->active.clusters[1] != 0);
	cluster1 = &g_dev_buffer[blob->active.clusters[1] * dev->blocklen];

	/* cluster0: [ F0F0 AAAA | AAAA 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ FFFF 0000 | 0000 0000 | 0000 0000 | 0000 0000 ] */
	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 2 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 3 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 4 * 512, payload_aa, 8 * 512) == 0);
	CU_ASSERT(memcmp(cluster0 + (SZ - 4) * 512, payload_ff, 4 * 512) == 0);

	CU_ASSERT(memcmp(cluster1 + 0 * 512, payload_ff, 4 * 512) == 0);
	CU_ASSERT(memcmp(cluster1 + 4 * 512, payload_00, (SZ - 4) * 512) == 0);

	/* Verify write to second cluster */
	spdk_blob_io_write(blob, channel, payload_ff, SZ + 12, 2, blob_op_complete, NULL);
	poll_threads();

	SPDK_CU_ASSERT_FATAL(blob->active.clusters[1] != 0);
	cluster1 = &g_dev_buffer[blob->active.clusters[1] * dev->blocklen];

	/* cluster0: [ F0F0 AAAA | AAAA 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ FFFF 0000 | 0000 FF00 | 0000 0000 | 0000 0000 ] */
	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 2 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 3 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 4 * 512, payload_aa, 8 * 512) == 0);
	CU_ASSERT(memcmp(cluster0 + (SZ - 4) * 512, payload_ff, 4 * 512) == 0);

	CU_ASSERT(memcmp(cluster1 + 0 * 512, payload_ff, 4 * 512) == 0);
	CU_ASSERT(memcmp(cluster1 + 4 * 512, payload_00, 8 * 512) == 0);
	CU_ASSERT(memcmp(cluster1 + 12 * 512, payload_ff, 2 * 512) == 0);
	CU_ASSERT(memcmp(cluster1 + 14 * 512, payload_00, (SZ - 14) * 512) == 0);
}

static void
test_io_read(struct spdk_bs_dev *dev, struct spdk_blob *blob, struct spdk_io_channel *channel)
{
	const uint32_t SZ = IO_UT_BLOCKS_PER_CLUSTER;
	uint8_t payload_read[2 * SZ * 512];
	uint8_t payload_ff[SZ * 512];
	uint8_t payload_aa[SZ * 512];
	uint8_t payload_00[SZ * 512];

	memset(payload_ff, 0xFF, sizeof(payload_ff));
	memset(payload_aa, 0xAA, sizeof(payload_aa));
	memset(payload_00, 0x00, sizeof(payload_00));

	/* Read only first io unit */
	/* cluster0: [ (F)0F0 AAAA | AAAA 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ FFFF 0000 | 0000 FF00 | 0000 0000 | 0000 0000 ]
	 * payload_read: F000 0000 | 0000 0000 ... */
	memset(payload_read, 0x00, sizeof(payload_read));
	spdk_blob_io_read(blob, channel, payload_read, 0, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_read + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 1 * 512, payload_00, (SZ - 1) * 512) == 0);

	/* Read four io_units starting from offset = 2
	 * cluster0: [ F0(F0 AA)AA | AAAA 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ FFFF 0000 | 0000 FF00 | 0000 0000 | 0000 0000 ]
	 * payload_read: F0AA 0000 | 0000 0000 ... */

	memset(payload_read, 0x00, sizeof(payload_read));
	spdk_blob_io_read(blob, channel, payload_read, 2, 4, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	CU_ASSERT(memcmp(payload_read + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 2 * 512, payload_aa, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 3 * 512, payload_aa, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 4 * 512, payload_00, (SZ - 4) * 512) == 0);

	/* Read eight io_units across multiple pages
	 * cluster0: [ F0F0 (AAAA | AAAA) 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ FFFF 0000 | 0000 FF00 | 0000 0000 | 0000 0000 ]
	 * payload_read: AAAA AAAA | 0000 0000 ... */
	memset(payload_read, 0x00, sizeof(payload_read));
	spdk_blob_io_read(blob, channel, payload_read, 4, 8, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	CU_ASSERT(memcmp(payload_read + 0 * 512, payload_aa, 8 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 8 * 512, payload_00, (SZ - 8) * 512) == 0);

	/* Read eight io_units across multiple clusters
	 * cluster0: [ F0F0 AAAA | AAAA 0000 | 0000 0000 | 0000 (FFFF ]
	 * cluster1: [ FFFF) 0000 | 0000 FF00 | 0000 0000 | 0000 0000 ]
	 * payload_read: FFFF FFFF | 0000 0000 ... */
	memset(payload_read, 0x00, sizeof(payload_read));
	spdk_blob_io_read(blob, channel, payload_read, SZ - 4, 8, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	CU_ASSERT(memcmp(payload_read + 0 * 512, payload_ff, 8 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 8 * 512, payload_00, (SZ - 8) * 512) == 0);

	/* Read four io_units from second cluster
	 * cluster0: [ F0F0 AAAA | AAAA 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ FFFF 0000 | 00(00 FF)00 | 0000 0000 | 0000 0000 ]
	 * payload_read: 00FF 0000 | 0000 0000 ... */
	memset(payload_read, 0x00, sizeof(payload_read));
	spdk_blob_io_read(blob, channel, payload_read, SZ + 10, 4, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	CU_ASSERT(memcmp(payload_read + 0 * 512, payload_00, 2 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 2 * 512, payload_ff, 2 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 4 * 512, payload_00, (SZ - 4) * 512) == 0);

	/* Read second cluster
	 * cluster0: [ F0F0 AAAA | AAAA 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ (FFFF 0000 | 0000 FF00 | 0000 0000 | 0000 0000) ]
	 * payload_read: FFFF 0000 | 0000 FF00 ... */
	memset(payload_read, 0x00, sizeof(payload_read));
	spdk_blob_io_read(blob, channel, payload_read, SZ, SZ, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_read + 0 * 512, payload_ff, 4 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 4 * 512, payload_00, 8 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 12 * 512, payload_ff, 2 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 14 * 512, payload_00, (SZ - 14) * 512) == 0);

	/* Read whole two clusters
	 * cluster0: [ (F0F0 AAAA | AAAA) 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ FFFF 0000 | 0000 FF00 | 0000 0000 | 0000 0000) ] */
	memset(payload_read, 0x00, sizeof(payload_read));
	spdk_blob_io_read(blob, channel, payload_read, 0, SZ * 2, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	CU_ASSERT(memcmp(payload_read + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 2 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 3 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 4 * 512, payload_aa, 8 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + (SZ - 4) * 512, payload_ff, 4 * 512) == 0);

	CU_ASSERT(memcmp(payload_read + (SZ + 0) * 512, payload_ff, 4 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + (SZ + 4) * 512, payload_00, 8 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + (SZ + 12) * 512, payload_ff, 2 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + (SZ + 14) * 512, payload_00, (SZ - 14) * 512) == 0);
}


static void
test_io_unmap(struct spdk_bs_dev *dev, struct spdk_blob *blob, struct spdk_io_channel *channel)
{
	const uint32_t SZ = IO_UT_BLOCKS_PER_CLUSTER;
	uint8_t payload_ff[SZ * 512];
	uint8_t payload_aa[SZ * 512];
	uint8_t payload_00[SZ * 512];
	uint8_t *cluster0, *cluster1;

	memset(payload_ff, 0xFF, sizeof(payload_ff));
	memset(payload_aa, 0xAA, sizeof(payload_aa));
	memset(payload_00, 0x00, sizeof(payload_00));

	cluster0 = &g_dev_buffer[blob->active.clusters[0] * dev->blocklen];
	cluster1 = &g_dev_buffer[blob->active.clusters[1] * dev->blocklen];

	/* Unmap */
	spdk_blob_io_unmap(blob, channel, 0, SZ * 2, blob_op_complete, NULL);
	poll_threads();

	CU_ASSERT(g_bserrno == 0);

	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_00, SZ * 512) == 0);
	CU_ASSERT(memcmp(cluster1 + 0 * 512, payload_00, SZ * 512) == 0);
}

static void
test_io_zeroes(struct spdk_bs_dev *dev, struct spdk_blob *blob, struct spdk_io_channel *channel)
{
	const uint32_t SZ = IO_UT_BLOCKS_PER_CLUSTER;
	uint8_t payload_ff[SZ * 512];
	uint8_t payload_aa[SZ * 512];
	uint8_t payload_00[SZ * 512];
	uint8_t *cluster0, *cluster1;

	memset(payload_ff, 0xFF, sizeof(payload_ff));
	memset(payload_aa, 0xAA, sizeof(payload_aa));
	memset(payload_00, 0x00, sizeof(payload_00));

	cluster0 = &g_dev_buffer[blob->active.clusters[0] * dev->blocklen];
	cluster1 = &g_dev_buffer[blob->active.clusters[1] * dev->blocklen];

	/* Write zeroes  */
	spdk_blob_io_write_zeroes(blob, channel, 0, SZ * 2, blob_op_complete, NULL);
	poll_threads();

	CU_ASSERT(g_bserrno == 0);

	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_00, SZ * 512) == 0);
	CU_ASSERT(memcmp(cluster1 + 0 * 512, payload_00, SZ * 512) == 0);
}

static inline void
test_blob_io_writev(struct spdk_blob *blob, struct spdk_io_channel *channel,
		    struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
		    spdk_blob_op_complete cb_fn, void *cb_arg, struct spdk_blob_ext_io_opts *io_opts)
{
	if (io_opts) {
		g_dev_writev_ext_called = false;
		memset(&g_blob_ext_io_opts, 0, sizeof(g_blob_ext_io_opts));
		spdk_blob_io_writev_ext(blob, channel, iov, iovcnt, offset, length, blob_op_complete, NULL,
					io_opts);
	} else {
		spdk_blob_io_writev(blob, channel, iov, iovcnt, offset, length, blob_op_complete, NULL);
	}
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	if (io_opts) {
		CU_ASSERT(g_dev_writev_ext_called);
		CU_ASSERT(memcmp(io_opts, &g_blob_ext_io_opts, sizeof(g_blob_ext_io_opts)) == 0);
	}
}

static void
test_iov_write(struct spdk_bs_dev *dev, struct spdk_blob *blob, struct spdk_io_channel *channel,
	       bool ext_api)
{
	const uint32_t SZ = IO_UT_BLOCKS_PER_CLUSTER;
	uint8_t payload_ff[SZ * 512];
	uint8_t payload_aa[SZ * 512];
	uint8_t payload_00[SZ * 512];
	uint8_t *cluster0, *cluster1;
	struct iovec iov[4];
	struct spdk_blob_ext_io_opts ext_opts = {
		.memory_domain = (struct spdk_memory_domain *)0xfeedbeef,
		.memory_domain_ctx = (void *)0xf00df00d,
		.size = sizeof(struct spdk_blob_ext_io_opts),
		.user_ctx = (void *)123,
	};

	memset(payload_ff, 0xFF, sizeof(payload_ff));
	memset(payload_aa, 0xAA, sizeof(payload_aa));
	memset(payload_00, 0x00, sizeof(payload_00));

	/* Try to perform I/O with io unit = 512 */
	iov[0].iov_base = payload_ff;
	iov[0].iov_len = 1 * 512;

	test_blob_io_writev(blob, channel, iov, 1, 0, 1, blob_op_complete, NULL,
			    ext_api ? &ext_opts : NULL);

	/* If thin provisioned is set cluster should be allocated now */
	SPDK_CU_ASSERT_FATAL(blob->active.clusters[0] != 0);
	cluster0 = &g_dev_buffer[blob->active.clusters[0] * dev->blocklen];

	/* Each character 0-F symbolizes single io_unit containing 512 bytes block filled with that character.
	* Each page is separated by |. Whole block [...] symbolizes one cluster (containing 4 pages). */
	/* cluster0: [ F000 0000 | 0000 0000 | 0000 0000 | 0000 0000 ] */
	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 1 * 512, payload_00, (SZ - 1) * 512) == 0);

	/* Verify write with offset on first page */
	iov[0].iov_base = payload_ff;
	iov[0].iov_len = 1 * 512;

	test_blob_io_writev(blob, channel, iov, 1, 2, 1, blob_op_complete, NULL,
			    ext_api ? &ext_opts : NULL);

	/* cluster0: [ F0F0 0000 | 0000 0000 | 0000 0000 | 0000 0000 ] */
	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 2 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 3 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 4 * 512, payload_00, (SZ - 4) * 512) == 0);

	/* Verify write with offset on first page */
	iov[0].iov_base = payload_ff;
	iov[0].iov_len = 4 * 512;
	spdk_blob_io_writev(blob, channel, iov, 1, 4, 4, blob_op_complete, NULL);
	poll_threads();

	/* cluster0: [ F0F0 FFFF | 0000 0000 | 0000 0000 | 0000 0000 ] */
	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 2 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 3 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 4 * 512, payload_ff, 4 * 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 8 * 512, payload_00, (SZ - 8) * 512) == 0);

	/* Verify write with offset on second page */
	iov[0].iov_base = payload_ff;
	iov[0].iov_len = 4 * 512;
	spdk_blob_io_writev(blob, channel, iov, 1, 8, 4, blob_op_complete, NULL);
	poll_threads();

	/* cluster0: [ F0F0 FFFF | FFFF 0000 | 0000 0000 | 0000 0000 ] */
	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 2 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 3 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 4 * 512, payload_ff, 8 * 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 12 * 512, payload_00, (SZ - 12) * 512) == 0);

	/* Verify write across multiple pages */
	iov[0].iov_base = payload_aa;
	iov[0].iov_len = 8 * 512;

	test_blob_io_writev(blob, channel, iov, 1, 4, 8, blob_op_complete, NULL,
			    ext_api ? &ext_opts : NULL);

	/* cluster0: [ F0F0 AAAA | AAAA 0000 | 0000 0000 | 0000 0000 ] */
	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 2 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 3 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 4 * 512, payload_aa, 8 * 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 12 * 512, payload_00, (SZ - 12) * 512) == 0);

	/* Verify write across multiple clusters */

	iov[0].iov_base = payload_ff;
	iov[0].iov_len = 8 * 512;

	test_blob_io_writev(blob, channel, iov, 1, (SZ - 4), 8, blob_op_complete, NULL,
			    ext_api ? &ext_opts : NULL);

	SPDK_CU_ASSERT_FATAL(blob->active.clusters[1] != 0);
	cluster1 = &g_dev_buffer[blob->active.clusters[1] * dev->blocklen];

	/* cluster0: [ F0F0 AAAA | AAAA 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ FFFF 0000 | 0000 0000 | 0000 0000 | 0000 0000 ] */
	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 2 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 3 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 4 * 512, payload_aa, 8 * 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 12 * 512, payload_00, (SZ - 16) * 512) == 0);
	CU_ASSERT(memcmp(cluster0 + (SZ - 4) * 512, payload_ff, 4 * 512) == 0);

	CU_ASSERT(memcmp(cluster1 + 0 * 512, payload_ff, 4 * 512) == 0);
	CU_ASSERT(memcmp(cluster1 + 4 * 512, payload_00, (SZ - 4) * 512) == 0);

	/* Verify write to second cluster */

	iov[0].iov_base = payload_ff;
	iov[0].iov_len = 2 * 512;

	test_blob_io_writev(blob, channel, iov, 1, SZ + 12, 2, blob_op_complete, NULL,
			    ext_api ? &ext_opts : NULL);

	SPDK_CU_ASSERT_FATAL(blob->active.clusters[1] != 0);
	cluster1 = &g_dev_buffer[blob->active.clusters[1] * dev->blocklen];

	/* cluster0: [ F0F0 AAAA | AAAA 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ FFFF 0000 | 0000 FF00 | 0000 0000 | 0000 0000 ] */
	CU_ASSERT(memcmp(cluster0 + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 2 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 3 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(cluster0 + 4 * 512, payload_aa, 8 * 512) == 0);
	CU_ASSERT(memcmp(cluster0 + (SZ - 4) * 512, payload_ff, 4 * 512) == 0);

	CU_ASSERT(memcmp(cluster1 + 0 * 512, payload_ff, 4 * 512) == 0);
	CU_ASSERT(memcmp(cluster1 + 4 * 512, payload_00, 8 * 512) == 0);
	CU_ASSERT(memcmp(cluster1 + 12 * 512, payload_ff, 2 * 512) == 0);
	CU_ASSERT(memcmp(cluster1 + 14 * 512, payload_00, (SZ - 14) * 512) == 0);
}

static inline void
test_blob_io_readv(struct spdk_blob *blob, struct spdk_io_channel *channel,
		   struct iovec *iov, int iovcnt, uint64_t offset, uint64_t length,
		   spdk_blob_op_complete cb_fn, void *cb_arg, struct spdk_blob_ext_io_opts *io_opts)
{
	if (io_opts) {
		g_dev_readv_ext_called = false;
		memset(&g_blob_ext_io_opts, 0, sizeof(g_blob_ext_io_opts));
		spdk_blob_io_readv_ext(blob, channel, iov, iovcnt, offset, length, blob_op_complete, NULL, io_opts);
	} else {
		spdk_blob_io_readv(blob, channel, iov, iovcnt, offset, length, blob_op_complete, NULL);
	}
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	if (io_opts) {
		CU_ASSERT(g_dev_readv_ext_called);
		CU_ASSERT(memcmp(io_opts, &g_blob_ext_io_opts, sizeof(g_blob_ext_io_opts)) == 0);
	}
}

static void
test_iov_read(struct spdk_bs_dev *dev, struct spdk_blob *blob, struct spdk_io_channel *channel,
	      bool ext_api)
{
	const uint32_t SZ = IO_UT_BLOCKS_PER_CLUSTER;
	uint8_t payload_read[2 * SZ * 512];
	uint8_t payload_ff[SZ * 512];
	uint8_t payload_aa[SZ * 512];
	uint8_t payload_00[SZ * 512];
	struct iovec iov[4];
	struct spdk_blob_ext_io_opts ext_opts = {
		.memory_domain = (struct spdk_memory_domain *)0xfeedbeef,
		.memory_domain_ctx = (void *)0xf00df00d,
		.size = sizeof(struct spdk_blob_ext_io_opts),
		.user_ctx = (void *)123,
	};

	memset(payload_ff, 0xFF, sizeof(payload_ff));
	memset(payload_aa, 0xAA, sizeof(payload_aa));
	memset(payload_00, 0x00, sizeof(payload_00));

	/* Read only first io unit */
	/* cluster0: [ (F)0F0 AAAA | AAAA 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ FFFF 0000 | 0000 FF00 | 0000 0000 | 0000 0000 ]
	 * payload_read: F000 0000 | 0000 0000 ... */
	memset(payload_read, 0x00, sizeof(payload_read));
	iov[0].iov_base = payload_read;
	iov[0].iov_len = 1 * 512;

	test_blob_io_readv(blob, channel, iov, 1, 0, 1, blob_op_complete, NULL, ext_api ? &ext_opts : NULL);

	CU_ASSERT(memcmp(payload_read + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 1 * 512, payload_00, (SZ - 1) * 512) == 0);

	/* Read four io_units starting from offset = 2
	 * cluster0: [ F0(F0 AA)AA | AAAA 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ FFFF 0000 | 0000 FF00 | 0000 0000 | 0000 0000 ]
	 * payload_read: F0AA 0000 | 0000 0000 ... */

	memset(payload_read, 0x00, sizeof(payload_read));
	iov[0].iov_base = payload_read;
	iov[0].iov_len = 4 * 512;

	test_blob_io_readv(blob, channel, iov, 1, 2, 4, blob_op_complete, NULL, ext_api ? &ext_opts : NULL);

	CU_ASSERT(memcmp(payload_read + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 2 * 512, payload_aa, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 3 * 512, payload_aa, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 4 * 512, payload_00, (SZ - 4) * 512) == 0);

	/* Read eight io_units across multiple pages
	 * cluster0: [ F0F0 (AAAA | AAAA) 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ FFFF 0000 | 0000 FF00 | 0000 0000 | 0000 0000 ]
	 * payload_read: AAAA AAAA | 0000 0000 ... */
	memset(payload_read, 0x00, sizeof(payload_read));
	iov[0].iov_base = payload_read;
	iov[0].iov_len = 4 * 512;
	iov[1].iov_base = payload_read + 4 * 512;
	iov[1].iov_len = 4 * 512;

	test_blob_io_readv(blob, channel, iov, 2, 4, 8, blob_op_complete, NULL, ext_api ? &ext_opts : NULL);

	CU_ASSERT(memcmp(payload_read + 0 * 512, payload_aa, 8 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 8 * 512, payload_00, (SZ - 8) * 512) == 0);

	/* Read eight io_units across multiple clusters
	 * cluster0: [ F0F0 AAAA | AAAA 0000 | 0000 0000 | 0000 (FFFF ]
	 * cluster1: [ FFFF) 0000 | 0000 FF00 | 0000 0000 | 0000 0000 ]
	 * payload_read: FFFF FFFF | 0000 0000 ... */
	memset(payload_read, 0x00, sizeof(payload_read));
	iov[0].iov_base = payload_read;
	iov[0].iov_len = 2 * 512;
	iov[1].iov_base = payload_read + 2 * 512;
	iov[1].iov_len = 2 * 512;
	iov[2].iov_base = payload_read + 4 * 512;
	iov[2].iov_len = 2 * 512;
	iov[3].iov_base = payload_read + 6 * 512;
	iov[3].iov_len = 2 * 512;

	test_blob_io_readv(blob, channel, iov, 4, SZ - 4, 8, blob_op_complete, NULL,
			   ext_api ? &ext_opts : NULL);

	CU_ASSERT(memcmp(payload_read + 0 * 512, payload_ff, 8 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 8 * 512, payload_00, (SZ - 8) * 512) == 0);

	/* Read four io_units from second cluster
	 * cluster0: [ F0F0 AAAA | AAAA 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ FFFF 0000 | 00(00 FF)00 | 0000 0000 | 0000 0000 ]
	 * payload_read: 00FF 0000 | 0000 0000 ... */
	memset(payload_read, 0x00, sizeof(payload_read));
	iov[0].iov_base = payload_read;
	iov[0].iov_len = 1 * 512;
	iov[1].iov_base = payload_read + 1 * 512;
	iov[1].iov_len = 3 * 512;

	test_blob_io_readv(blob, channel, iov, 2, SZ + 10, 4, blob_op_complete, NULL,
			   ext_api ? &ext_opts : NULL);

	CU_ASSERT(memcmp(payload_read + 0 * 512, payload_00, 2 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 2 * 512, payload_ff, 2 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 4 * 512, payload_00, (SZ - 4) * 512) == 0);

	/* Read second cluster
	 * cluster0: [ F0F0 AAAA | AAAA 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ (FFFF 0000 | 0000 FF00 | 0000 0000 | 0000 0000) ]
	 * payload_read: FFFF 0000 | 0000 FF00 ... */
	memset(payload_read, 0x00, sizeof(payload_read));
	iov[0].iov_base = payload_read;
	iov[0].iov_len = 1 * 512;
	iov[1].iov_base = payload_read + 1 * 512;
	iov[1].iov_len = 2 * 512;
	iov[2].iov_base = payload_read + 3 * 512;
	iov[2].iov_len = 4 * 512;
	iov[3].iov_base = payload_read + 7 * 512;
	iov[3].iov_len = (SZ - 7) * 512;

	test_blob_io_readv(blob, channel, iov, 4, SZ, SZ, blob_op_complete, NULL,
			   ext_api ? &ext_opts : NULL);

	CU_ASSERT(memcmp(payload_read + 0 * 512, payload_ff, 4 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 4 * 512, payload_00, 8 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 12 * 512, payload_ff, 2 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + 14 * 512, payload_00, (SZ - 14) * 512) == 0);

	/* Read whole two clusters
	 * cluster0: [ (F0F0 AAAA | AAAA) 0000 | 0000 0000 | 0000 FFFF ]
	 * cluster1: [ FFFF 0000 | 0000 FF00 | 0000 0000 | 0000 0000) ] */
	memset(payload_read, 0x00, sizeof(payload_read));
	iov[0].iov_base = payload_read;
	iov[0].iov_len = 1 * 512;
	iov[1].iov_base = payload_read + 1 * 512;
	iov[1].iov_len = 8 * 512;
	iov[2].iov_base = payload_read + 9 * 512;
	iov[2].iov_len = 16 * 512;
	iov[3].iov_base = payload_read + 25 * 512;
	iov[3].iov_len = (2 * SZ - 25) * 512;

	test_blob_io_readv(blob, channel, iov, 4, 0, SZ * 2, blob_op_complete, NULL,
			   ext_api ? &ext_opts : NULL);

	CU_ASSERT(memcmp(payload_read + 0 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 1 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 2 * 512, payload_ff, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 3 * 512, payload_00, 512) == 0);
	CU_ASSERT(memcmp(payload_read + 4 * 512, payload_aa, 8 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + (SZ - 4) * 512, payload_ff, 4 * 512) == 0);

	CU_ASSERT(memcmp(payload_read + (SZ + 0) * 512, payload_ff, 4 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + (SZ + 4) * 512, payload_00, 8 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + (SZ + 12) * 512, payload_ff, 2 * 512) == 0);
	CU_ASSERT(memcmp(payload_read + (SZ + 14) * 512, payload_00, (SZ - 14) * 512) == 0);
}

static void
blob_io_unit(void)
{
	struct spdk_bs_opts bsopts;
	struct spdk_blob_opts opts;
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob, *snapshot, *clone;
	spdk_blob_id blobid;
	struct spdk_io_channel *channel;

	/* Create dev with 512 bytes io unit size */

	spdk_bs_opts_init(&bsopts, sizeof(bsopts));
	bsopts.cluster_sz = IO_UT_BLOCKS_PER_CLUSTER * 512;
	snprintf(bsopts.bstype.bstype, sizeof(bsopts.bstype.bstype), "TESTTYPE");

	/* Try to initialize a new blob store with unsupported io_unit */
	dev = init_dev();
	dev->blocklen = 512;
	dev->blockcnt =  DEV_BUFFER_SIZE / dev->blocklen;

	/* Initialize a new blob store */
	spdk_bs_init(dev, &bsopts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	CU_ASSERT(spdk_bs_get_io_unit_size(bs) == 512);
	channel = spdk_bs_alloc_io_channel(bs);

	/* Create thick provisioned blob */
	ut_spdk_blob_opts_init(&opts);
	opts.thin_provision = false;
	opts.num_clusters = 32;

	blob = ut_blob_create_and_open(bs, &opts);
	blobid = spdk_blob_get_id(blob);

	test_io_write(dev, blob, channel);
	test_io_read(dev, blob, channel);
	test_io_zeroes(dev, blob, channel);

	test_iov_write(dev, blob, channel, false);
	test_iov_read(dev, blob, channel, false);
	test_io_zeroes(dev, blob, channel);

	test_iov_write(dev, blob, channel, true);
	test_iov_read(dev, blob, channel, true);

	test_io_unmap(dev, blob, channel);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	blob = NULL;
	g_blob = NULL;

	/* Create thin provisioned blob */

	ut_spdk_blob_opts_init(&opts);
	opts.thin_provision = true;
	opts.num_clusters = 32;

	blob = ut_blob_create_and_open(bs, &opts);
	blobid = spdk_blob_get_id(blob);

	test_io_write(dev, blob, channel);
	test_io_read(dev, blob, channel);
	test_io_zeroes(dev, blob, channel);

	test_iov_write(dev, blob, channel, false);
	test_iov_read(dev, blob, channel, false);
	test_io_zeroes(dev, blob, channel);

	test_iov_write(dev, blob, channel, true);
	test_iov_read(dev, blob, channel, true);

	/* Create snapshot */

	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	snapshot = g_blob;

	spdk_bs_create_clone(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	clone = g_blob;

	test_io_read(dev, blob, channel);
	test_io_read(dev, snapshot, channel);
	test_io_read(dev, clone, channel);

	test_iov_read(dev, blob, channel, false);
	test_iov_read(dev, snapshot, channel, false);
	test_iov_read(dev, clone, channel, false);

	test_iov_read(dev, blob, channel, true);
	test_iov_read(dev, snapshot, channel, true);
	test_iov_read(dev, clone, channel, true);

	/* Inflate clone */

	spdk_bs_inflate_blob(bs, channel, blobid, blob_op_complete, NULL);
	poll_threads();

	CU_ASSERT(g_bserrno == 0);

	test_io_read(dev, clone, channel);

	test_io_unmap(dev, clone, channel);

	test_iov_write(dev, clone, channel, false);
	test_iov_read(dev, clone, channel, false);
	test_io_unmap(dev, clone, channel);

	test_iov_write(dev, clone, channel, true);
	test_iov_read(dev, clone, channel, true);

	spdk_blob_close(blob, blob_op_complete, NULL);
	spdk_blob_close(snapshot, blob_op_complete, NULL);
	spdk_blob_close(clone, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	blob = NULL;
	g_blob = NULL;

	spdk_bs_free_io_channel(channel);
	poll_threads();

	/* Unload the blob store */
	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	g_blob = NULL;
	g_blobid = 0;
}

static void
blob_io_unit_compatibility(void)
{
	struct spdk_bs_opts bsopts;
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_bs_super_block *super;

	/* Create dev with 512 bytes io unit size */

	spdk_bs_opts_init(&bsopts, sizeof(bsopts));
	bsopts.cluster_sz = g_phys_blocklen * 4;
	snprintf(bsopts.bstype.bstype, sizeof(bsopts.bstype.bstype), "TESTTYPE");

	/* Try to initialize a new blob store with unsupported io_unit */
	dev = init_dev();
	dev->blocklen = 512;
	dev->blockcnt =  DEV_BUFFER_SIZE / dev->blocklen;

	/* Initialize a new blob store */
	spdk_bs_init(dev, &bsopts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	CU_ASSERT(spdk_bs_get_io_unit_size(bs) == 512);

	/* Unload the blob store */
	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Modify super block to behave like older version.
	 * Check if loaded io unit size equals SPDK_BS_PAGE_SIZE */
	super = (struct spdk_bs_super_block *)&g_dev_buffer[0];
	super->io_unit_size = 0;
	super->crc = blob_md_page_calc_crc(super);

	dev = init_dev();
	dev->blocklen = 512;
	dev->blockcnt =  DEV_BUFFER_SIZE / dev->blocklen;

	spdk_bs_load(dev, &bsopts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	CU_ASSERT(spdk_bs_get_io_unit_size(bs) == SPDK_BS_PAGE_SIZE);

	/* Unload the blob store */
	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	g_bs = NULL;
	g_blob = NULL;
	g_blobid = 0;
}

static void
first_sync_complete(void *cb_arg, int bserrno)
{
	struct spdk_blob *blob = cb_arg;
	int rc;

	CU_ASSERT(bserrno == 0);
	rc = spdk_blob_set_xattr(blob, "sync", "second", strlen("second") + 1);
	CU_ASSERT(rc == 0);
	CU_ASSERT(g_bserrno == -1);

	/* Keep g_bserrno at -1, only the
	 * second sync completion should set it at 0. */
}

static void
second_sync_complete(void *cb_arg, int bserrno)
{
	struct spdk_blob *blob = cb_arg;
	const void *value;
	size_t value_len;
	int rc;

	CU_ASSERT(bserrno == 0);

	/* Verify that the first sync completion had a chance to execute */
	rc = spdk_blob_get_xattr_value(blob, "sync", &value, &value_len);
	CU_ASSERT(rc == 0);
	SPDK_CU_ASSERT_FATAL(value != NULL);
	CU_ASSERT(value_len == strlen("second") + 1);
	CU_ASSERT_NSTRING_EQUAL_FATAL(value, "second", value_len);

	CU_ASSERT(g_bserrno == -1);
	g_bserrno = bserrno;
}

static void
blob_simultaneous_operations(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob_opts opts;
	struct spdk_blob *blob, *snapshot;
	spdk_blob_id blobid, snapshotid;
	struct spdk_io_channel *channel;
	int rc;

	channel = spdk_bs_alloc_io_channel(bs);
	SPDK_CU_ASSERT_FATAL(channel != NULL);

	ut_spdk_blob_opts_init(&opts);
	opts.num_clusters = 10;

	blob = ut_blob_create_and_open(bs, &opts);
	blobid = spdk_blob_get_id(blob);

	/* Create snapshot and try to remove blob in the same time:
	 * - snapshot should be created successfully
	 * - delete operation should fail w -EBUSY */
	CU_ASSERT(blob->locked_operation_in_progress == false);
	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	CU_ASSERT(blob->locked_operation_in_progress == true);
	spdk_bs_delete_blob(bs, blobid, blob_op_complete, NULL);
	CU_ASSERT(blob->locked_operation_in_progress == true);
	/* Deletion failure */
	CU_ASSERT(g_bserrno == -EBUSY);
	poll_threads();
	CU_ASSERT(blob->locked_operation_in_progress == false);
	/* Snapshot creation success */
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);

	snapshotid = g_blobid;

	spdk_bs_open_blob(bs, snapshotid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snapshot = g_blob;

	/* Inflate blob and try to remove blob in the same time:
	 * - blob should be inflated successfully
	 * - delete operation should fail w -EBUSY */
	CU_ASSERT(blob->locked_operation_in_progress == false);
	spdk_bs_inflate_blob(bs, channel, blobid, blob_op_complete, NULL);
	CU_ASSERT(blob->locked_operation_in_progress == true);
	spdk_bs_delete_blob(bs, blobid, blob_op_complete, NULL);
	CU_ASSERT(blob->locked_operation_in_progress == true);
	/* Deletion failure */
	CU_ASSERT(g_bserrno == -EBUSY);
	poll_threads();
	CU_ASSERT(blob->locked_operation_in_progress == false);
	/* Inflation success */
	CU_ASSERT(g_bserrno == 0);

	/* Clone snapshot and try to remove snapshot in the same time:
	 * - snapshot should be cloned successfully
	 * - delete operation should fail w -EBUSY */
	CU_ASSERT(blob->locked_operation_in_progress == false);
	spdk_bs_create_clone(bs, snapshotid, NULL, blob_op_with_id_complete, NULL);
	spdk_bs_delete_blob(bs, snapshotid, blob_op_complete, NULL);
	/* Deletion failure */
	CU_ASSERT(g_bserrno == -EBUSY);
	poll_threads();
	CU_ASSERT(blob->locked_operation_in_progress == false);
	/* Clone created */
	CU_ASSERT(g_bserrno == 0);

	/* Resize blob and try to remove blob in the same time:
	 * - blob should be resized successfully
	 * - delete operation should fail w -EBUSY */
	CU_ASSERT(blob->locked_operation_in_progress == false);
	spdk_blob_resize(blob, 50, blob_op_complete, NULL);
	CU_ASSERT(blob->locked_operation_in_progress == true);
	spdk_bs_delete_blob(bs, blobid, blob_op_complete, NULL);
	CU_ASSERT(blob->locked_operation_in_progress == true);
	/* Deletion failure */
	CU_ASSERT(g_bserrno == -EBUSY);
	poll_threads();
	CU_ASSERT(blob->locked_operation_in_progress == false);
	/* Blob resized successfully */
	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Issue two consecutive blob syncs, neither should fail.
	 * Force sync to actually occur by marking blob dirty each time.
	 * Execution of sync should not be enough to complete the operation,
	 * since disk I/O is required to complete it. */
	g_bserrno = -1;

	rc = spdk_blob_set_xattr(blob, "sync", "first", strlen("first") + 1);
	CU_ASSERT(rc == 0);
	spdk_blob_sync_md(blob, first_sync_complete, blob);
	CU_ASSERT(g_bserrno == -1);

	spdk_blob_sync_md(blob, second_sync_complete, blob);
	CU_ASSERT(g_bserrno == -1);

	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_free_io_channel(channel);
	poll_threads();

	ut_blob_close_and_delete(bs, snapshot);
	ut_blob_close_and_delete(bs, blob);
}

static void
blob_persist_test(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob_opts opts;
	struct spdk_blob *blob;
	spdk_blob_id blobid;
	struct spdk_io_channel *channel;
	char *xattr;
	size_t xattr_length;
	int rc;
	uint32_t page_count_clear, page_count_xattr;
	uint64_t poller_iterations;
	bool run_poller;

	channel = spdk_bs_alloc_io_channel(bs);
	SPDK_CU_ASSERT_FATAL(channel != NULL);

	ut_spdk_blob_opts_init(&opts);
	opts.num_clusters = 10;

	blob = ut_blob_create_and_open(bs, &opts);
	blobid = spdk_blob_get_id(blob);

	/* Save the amount of md pages used after creation of a blob.
	 * This should be consistent after removing xattr. */
	page_count_clear = spdk_bit_array_count_set(bs->used_md_pages);
	SPDK_CU_ASSERT_FATAL(blob->active.num_pages + blob->active.num_extent_pages == page_count_clear);
	SPDK_CU_ASSERT_FATAL(blob->clean.num_pages + blob->clean.num_extent_pages == page_count_clear);

	/* Add xattr with maximum length of descriptor to exceed single metadata page. */
	xattr_length = SPDK_BS_MAX_DESC_SIZE - sizeof(struct spdk_blob_md_descriptor_xattr) -
		       strlen("large_xattr");
	xattr = calloc(xattr_length, sizeof(char));
	SPDK_CU_ASSERT_FATAL(xattr != NULL);

	rc = spdk_blob_set_xattr(blob, "large_xattr", xattr, xattr_length);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	poll_threads();
	SPDK_CU_ASSERT_FATAL(g_bserrno == 0);

	/* Save the amount of md pages used after adding the large xattr */
	page_count_xattr = spdk_bit_array_count_set(bs->used_md_pages);
	SPDK_CU_ASSERT_FATAL(blob->active.num_pages + blob->active.num_extent_pages == page_count_xattr);
	SPDK_CU_ASSERT_FATAL(blob->clean.num_pages + blob->clean.num_extent_pages == page_count_xattr);

	/* Add xattr to a blob and sync it. While sync is occurring, remove the xattr and sync again.
	 * Interrupt the first sync after increasing number of poller iterations, until it succeeds.
	 * Expectation is that after second sync completes no xattr is saved in metadata. */
	poller_iterations = 1;
	run_poller = true;
	while (run_poller) {
		rc = spdk_blob_set_xattr(blob, "large_xattr", xattr, xattr_length);
		SPDK_CU_ASSERT_FATAL(rc == 0);
		g_bserrno = -1;
		spdk_blob_sync_md(blob, blob_op_complete, NULL);
		poll_thread_times(0, poller_iterations);
		if (g_bserrno == 0) {
			/* Poller iteration count was high enough for first sync to complete.
			 * Verify that blob takes up enough of md_pages to store the xattr. */
			SPDK_CU_ASSERT_FATAL(blob->active.num_pages + blob->active.num_extent_pages == page_count_xattr);
			SPDK_CU_ASSERT_FATAL(blob->clean.num_pages + blob->clean.num_extent_pages == page_count_xattr);
			SPDK_CU_ASSERT_FATAL(spdk_bit_array_count_set(bs->used_md_pages) == page_count_xattr);
			run_poller = false;
		}
		rc = spdk_blob_remove_xattr(blob, "large_xattr");
		SPDK_CU_ASSERT_FATAL(rc == 0);
		spdk_blob_sync_md(blob, blob_op_complete, NULL);
		poll_threads();
		SPDK_CU_ASSERT_FATAL(g_bserrno == 0);
		SPDK_CU_ASSERT_FATAL(blob->active.num_pages + blob->active.num_extent_pages == page_count_clear);
		SPDK_CU_ASSERT_FATAL(blob->clean.num_pages + blob->clean.num_extent_pages == page_count_clear);
		SPDK_CU_ASSERT_FATAL(spdk_bit_array_count_set(bs->used_md_pages) == page_count_clear);

		/* Reload bs and re-open blob to verify that xattr was not persisted. */
		spdk_blob_close(blob, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);

		ut_bs_reload(&bs, NULL);

		spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		SPDK_CU_ASSERT_FATAL(g_blob != NULL);
		blob = g_blob;

		rc = spdk_blob_get_xattr_value(blob, "large_xattr", (const void **)&xattr, &xattr_length);
		SPDK_CU_ASSERT_FATAL(rc == -ENOENT);

		poller_iterations++;
		/* Stop at high iteration count to prevent infinite loop.
		 * This value should be enough for first md sync to complete in any case. */
		SPDK_CU_ASSERT_FATAL(poller_iterations < 50);
	}

	free(xattr);

	ut_blob_close_and_delete(bs, blob);

	spdk_bs_free_io_channel(channel);
	poll_threads();
}

static void
blob_decouple_snapshot(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob_opts opts;
	struct spdk_blob *blob, *snapshot1, *snapshot2;
	struct spdk_io_channel *channel;
	spdk_blob_id blobid, snapshotid;
	uint64_t cluster;

	for (int delete_snapshot_first = 0; delete_snapshot_first <= 1; delete_snapshot_first++) {
		channel = spdk_bs_alloc_io_channel(bs);
		SPDK_CU_ASSERT_FATAL(channel != NULL);

		ut_spdk_blob_opts_init(&opts);
		opts.num_clusters = 10;
		opts.thin_provision = false;

		blob = ut_blob_create_and_open(bs, &opts);
		blobid = spdk_blob_get_id(blob);

		/* Create first snapshot */
		CU_ASSERT_EQUAL(_get_snapshots_count(bs), 0);
		spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
		CU_ASSERT_EQUAL(_get_snapshots_count(bs), 1);
		snapshotid = g_blobid;

		spdk_bs_open_blob(bs, snapshotid, blob_op_with_handle_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		SPDK_CU_ASSERT_FATAL(g_blob != NULL);
		snapshot1 = g_blob;

		/* Create the second one */
		CU_ASSERT_EQUAL(_get_snapshots_count(bs), 1);
		spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
		CU_ASSERT_EQUAL(_get_snapshots_count(bs), 2);
		snapshotid = g_blobid;

		spdk_bs_open_blob(bs, snapshotid, blob_op_with_handle_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		SPDK_CU_ASSERT_FATAL(g_blob != NULL);
		snapshot2 = g_blob;
		CU_ASSERT_EQUAL(spdk_blob_get_parent_snapshot(bs, snapshot2->id), snapshot1->id);

		/* Now decouple the second snapshot forcing it to copy the written clusters */
		spdk_bs_blob_decouple_parent(bs, channel, snapshot2->id, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);

		/* Verify that the snapshot has been decoupled and that the clusters have been copied */
		CU_ASSERT_EQUAL(spdk_blob_get_parent_snapshot(bs, snapshot2->id), SPDK_BLOBID_INVALID);
		for (cluster = 0; cluster < snapshot2->active.num_clusters; ++cluster) {
			CU_ASSERT_NOT_EQUAL(snapshot2->active.clusters[cluster], 0);
			CU_ASSERT_NOT_EQUAL(snapshot2->active.clusters[cluster],
					    snapshot1->active.clusters[cluster]);
		}

		spdk_bs_free_io_channel(channel);

		if (delete_snapshot_first) {
			ut_blob_close_and_delete(bs, snapshot2);
			ut_blob_close_and_delete(bs, snapshot1);
			ut_blob_close_and_delete(bs, blob);
		} else {
			ut_blob_close_and_delete(bs, blob);
			ut_blob_close_and_delete(bs, snapshot2);
			ut_blob_close_and_delete(bs, snapshot1);
		}
		poll_threads();
	}
}

static void
blob_seek_io_unit(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob;
	struct spdk_io_channel *channel;
	struct spdk_blob_opts opts;
	uint64_t free_clusters;
	uint8_t payload[10 * BLOCKLEN];
	uint64_t offset;
	uint64_t io_unit, io_units_per_cluster;

	free_clusters = spdk_bs_free_cluster_count(bs);

	channel = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(channel != NULL);

	/* Set blob as thin provisioned */
	ut_spdk_blob_opts_init(&opts);
	opts.thin_provision = true;

	/* Create a blob */
	blob = ut_blob_create_and_open(bs, &opts);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));

	io_units_per_cluster = bs_io_units_per_cluster(blob);

	/* The blob started at 0 clusters. Resize it to be 5, but still unallocated. */
	spdk_blob_resize(blob, 5, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(free_clusters == spdk_bs_free_cluster_count(bs));
	CU_ASSERT(blob->active.num_clusters == 5);

	/* Write at the beginning of first cluster */
	offset = 0;
	spdk_blob_io_write(blob, channel, payload, offset, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	io_unit = spdk_blob_get_next_allocated_io_unit(blob, 0);
	CU_ASSERT(io_unit == offset);

	io_unit = spdk_blob_get_next_unallocated_io_unit(blob, 0);
	CU_ASSERT(io_unit == io_units_per_cluster);

	/* Write in the middle of third cluster */
	offset = 2 * io_units_per_cluster + io_units_per_cluster / 2;
	spdk_blob_io_write(blob, channel, payload, offset, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	io_unit = spdk_blob_get_next_allocated_io_unit(blob, io_units_per_cluster);
	CU_ASSERT(io_unit == 2 * io_units_per_cluster);

	io_unit = spdk_blob_get_next_unallocated_io_unit(blob, 2 * io_units_per_cluster);
	CU_ASSERT(io_unit == 3 * io_units_per_cluster);

	/* Write at the end of last cluster */
	offset = 5 * io_units_per_cluster - 1;
	spdk_blob_io_write(blob, channel, payload, offset, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	io_unit = spdk_blob_get_next_allocated_io_unit(blob, 3 * io_units_per_cluster);
	CU_ASSERT(io_unit == 4 * io_units_per_cluster);

	io_unit = spdk_blob_get_next_unallocated_io_unit(blob, 4 * io_units_per_cluster);
	CU_ASSERT(io_unit == UINT64_MAX);

	spdk_bs_free_io_channel(channel);
	poll_threads();

	ut_blob_close_and_delete(bs, blob);
}

static void
blob_esnap_create(void)
{
	struct spdk_blob_store	*bs = g_bs;
	struct spdk_bs_opts	bs_opts;
	struct ut_esnap_opts	esnap_opts;
	struct spdk_blob_opts	opts;
	struct spdk_blob_open_opts open_opts;
	struct spdk_blob	*blob;
	uint32_t		cluster_sz, block_sz;
	const uint32_t		esnap_num_clusters = 4;
	uint64_t		esnap_num_blocks;
	uint32_t		sz;
	spdk_blob_id		blobid;
	uint32_t		bs_ctx_count, blob_ctx_count;

	cluster_sz = spdk_bs_get_cluster_size(bs);
	block_sz = spdk_bs_get_io_unit_size(bs);
	esnap_num_blocks = cluster_sz * esnap_num_clusters / block_sz;

	/* Create a normal blob and verify it is not an esnap clone. */
	ut_spdk_blob_opts_init(&opts);
	blob = ut_blob_create_and_open(bs, &opts);
	CU_ASSERT(!spdk_blob_is_esnap_clone(blob));
	ut_blob_close_and_delete(bs, blob);

	/* Create an esnap clone blob then verify it is an esnap clone and has the right size */
	ut_spdk_blob_opts_init(&opts);
	ut_esnap_opts_init(block_sz, esnap_num_blocks, __func__, NULL, &esnap_opts);
	opts.esnap_id = &esnap_opts;
	opts.esnap_id_len = sizeof(esnap_opts);
	opts.num_clusters = esnap_num_clusters;
	blob = ut_blob_create_and_open(bs, &opts);
	SPDK_CU_ASSERT_FATAL(blob != NULL);
	SPDK_CU_ASSERT_FATAL(spdk_blob_is_esnap_clone(blob));
	SPDK_CU_ASSERT_FATAL(blob_is_esnap_clone(blob));
	SPDK_CU_ASSERT_FATAL(!spdk_blob_is_clone(blob));
	sz = spdk_blob_get_num_clusters(blob);
	CU_ASSERT(sz == esnap_num_clusters);
	ut_blob_close_and_delete(bs, blob);

	/* Create an esnap clone without the size and verify it can be grown */
	ut_spdk_blob_opts_init(&opts);
	ut_esnap_opts_init(block_sz, esnap_num_blocks, __func__, NULL, &esnap_opts);
	opts.esnap_id = &esnap_opts;
	opts.esnap_id_len = sizeof(esnap_opts);
	blob = ut_blob_create_and_open(bs, &opts);
	SPDK_CU_ASSERT_FATAL(spdk_blob_is_esnap_clone(blob));
	sz = spdk_blob_get_num_clusters(blob);
	CU_ASSERT(sz == 0);
	spdk_blob_resize(blob, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	sz = spdk_blob_get_num_clusters(blob);
	CU_ASSERT(sz == 1);
	spdk_blob_resize(blob, esnap_num_clusters, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	sz = spdk_blob_get_num_clusters(blob);
	CU_ASSERT(sz == esnap_num_clusters);
	spdk_blob_resize(blob, esnap_num_clusters + 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	sz = spdk_blob_get_num_clusters(blob);
	CU_ASSERT(sz == esnap_num_clusters + 1);

	/* Reload the blobstore and be sure that the blob can be opened. */
	blobid = spdk_blob_get_id(blob);
	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_blob = NULL;
	spdk_bs_opts_init(&bs_opts, sizeof(bs_opts));
	bs_opts.esnap_bs_dev_create = ut_esnap_create;
	ut_bs_reload(&bs, &bs_opts);
	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;
	SPDK_CU_ASSERT_FATAL(spdk_blob_is_esnap_clone(blob));
	sz = spdk_blob_get_num_clusters(blob);
	CU_ASSERT(sz == esnap_num_clusters + 1);

	/* Reload the blobstore without esnap_bs_dev_create: should fail to open blob. */
	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_blob = NULL;
	spdk_bs_opts_init(&bs_opts, sizeof(bs_opts));
	ut_bs_reload(&bs, &bs_opts);
	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno != 0);
	CU_ASSERT(g_blob == NULL);

	/* Reload the blobstore with ctx set and verify it is passed to the esnap create callback */
	bs_ctx_count = 0;
	spdk_bs_opts_init(&bs_opts, sizeof(bs_opts));
	bs_opts.esnap_bs_dev_create = ut_esnap_create_with_count;
	bs_opts.esnap_ctx = &bs_ctx_count;
	ut_bs_reload(&bs, &bs_opts);
	/* Loading the blobstore triggers the esnap to be loaded */
	CU_ASSERT(bs_ctx_count == 1);
	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	/* Opening the blob also triggers the esnap to be loaded */
	CU_ASSERT(bs_ctx_count == 2);
	blob = g_blob;
	SPDK_CU_ASSERT_FATAL(spdk_blob_is_esnap_clone(blob));
	sz = spdk_blob_get_num_clusters(blob);
	CU_ASSERT(sz == esnap_num_clusters + 1);
	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_blob = NULL;
	/* If open_opts.esnap_ctx is set it is passed to the esnap create callback */
	blob_ctx_count = 0;
	spdk_blob_open_opts_init(&open_opts, sizeof(open_opts));
	open_opts.esnap_ctx = &blob_ctx_count;
	spdk_bs_open_blob_ext(bs, blobid, &open_opts, blob_op_with_handle_complete, NULL);
	poll_threads();
	blob = g_blob;
	CU_ASSERT(bs_ctx_count == 3);
	CU_ASSERT(blob_ctx_count == 1);
	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_blob = NULL;
}

static void
blob_esnap_clone_reload(void)
{
	struct spdk_blob_store	*bs = g_bs;
	struct spdk_bs_opts	bs_opts;
	struct ut_esnap_opts	esnap_opts;
	struct spdk_blob_opts	opts;
	struct spdk_blob	*eclone1, *snap1, *clone1;
	uint32_t		cluster_sz = spdk_bs_get_cluster_size(bs);
	uint32_t		block_sz = spdk_bs_get_io_unit_size(bs);
	const uint32_t		esnap_num_clusters = 4;
	uint64_t		esnap_num_blocks = cluster_sz * esnap_num_clusters / block_sz;
	spdk_blob_id		eclone1_id, snap1_id, clone1_id;
	struct spdk_io_channel	*bs_ch;
	char			buf[block_sz];
	int			bserr1, bserr2, bserr3, bserr4;
	struct spdk_bs_dev	*dev;

	/* Create and open an esnap clone blob */
	ut_spdk_blob_opts_init(&opts);
	ut_esnap_opts_init(block_sz, esnap_num_blocks, __func__, NULL, &esnap_opts);
	opts.esnap_id = &esnap_opts;
	opts.esnap_id_len = sizeof(esnap_opts);
	opts.num_clusters = esnap_num_clusters;
	eclone1 = ut_blob_create_and_open(bs, &opts);
	CU_ASSERT(eclone1 != NULL);
	CU_ASSERT(spdk_blob_is_esnap_clone(eclone1));
	eclone1_id = eclone1->id;

	/* Create and open a snapshot of eclone1 */
	spdk_bs_create_snapshot(bs, eclone1_id, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	CU_ASSERT(g_bserrno == 0);
	snap1_id = g_blobid;
	spdk_bs_open_blob(bs, snap1_id, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	snap1 = g_blob;

	/* Create and open regular clone of snap1 */
	spdk_bs_create_clone(bs, snap1_id, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	SPDK_CU_ASSERT_FATAL(g_bserrno == 0);
	clone1_id = g_blobid;
	spdk_bs_open_blob(bs, clone1_id, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	clone1 = g_blob;

	/* Close the blobs in preparation for reloading the blobstore */
	spdk_blob_close(clone1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	spdk_blob_close(snap1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	spdk_blob_close(eclone1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_blob = NULL;

	/* Reload the blobstore */
	spdk_bs_opts_init(&bs_opts, sizeof(bs_opts));
	bs_opts.esnap_bs_dev_create = ut_esnap_create;
	ut_bs_reload(&bs, &bs_opts);

	/* Be sure each of the blobs can be opened */
	spdk_bs_open_blob(bs, eclone1_id, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	eclone1 = g_blob;
	spdk_bs_open_blob(bs, snap1_id, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	snap1 = g_blob;
	spdk_bs_open_blob(bs, clone1_id, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	clone1 = g_blob;

	/* Perform some reads on each of them to cause channels to be allocated */
	bs_ch = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(bs_ch != NULL);
	spdk_blob_io_read(eclone1, bs_ch, buf, 0, 1, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	spdk_blob_io_read(snap1, bs_ch, buf, 0, 1, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	spdk_blob_io_read(clone1, bs_ch, buf, 0, 1, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/*
	 * Unload the blobstore in a way similar to how lvstore unloads it.  This should exercise
	 * the deferred unload path in spdk_bs_unload().
	 */
	bserr1 = 0xbad;
	bserr2 = 0xbad;
	bserr3 = 0xbad;
	bserr4 = 0xbad;
	spdk_blob_close(eclone1, blob_op_complete, &bserr1);
	spdk_blob_close(snap1, blob_op_complete, &bserr2);
	spdk_blob_close(clone1, blob_op_complete, &bserr3);
	spdk_bs_unload(bs, blob_op_complete, &bserr4);
	spdk_bs_free_io_channel(bs_ch);
	poll_threads();
	CU_ASSERT(bserr1 == 0);
	CU_ASSERT(bserr2 == 0);
	CU_ASSERT(bserr3 == 0);
	CU_ASSERT(bserr4 == 0);
	g_blob = NULL;

	/* Reload the blobstore */
	spdk_bs_opts_init(&bs_opts, sizeof(bs_opts));
	bs_opts.esnap_bs_dev_create = ut_esnap_create;
	dev = init_dev();
	spdk_bs_load(dev, &bs_opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
}

static bool
blob_esnap_verify_contents(struct spdk_blob *blob, struct spdk_io_channel *ch,
			   uint64_t offset, uint64_t size, uint32_t readsize, const char *how)
{
	const uint32_t	bs_blksz = blob->bs->io_unit_size;
	const uint32_t	esnap_blksz = blob->back_bs_dev ? blob->back_bs_dev->blocklen : bs_blksz;
	const uint32_t	start_blk = offset / bs_blksz;
	const uint32_t	num_blocks = spdk_max(size, readsize) / bs_blksz;
	const uint32_t	blocks_per_read = spdk_min(size, readsize) / bs_blksz;
	uint32_t	blob_block;
	struct iovec	iov;
	uint8_t		buf[spdk_min(size, readsize)];
	bool		block_ok;

	SPDK_CU_ASSERT_FATAL(offset % bs_blksz == 0);
	SPDK_CU_ASSERT_FATAL(size % bs_blksz == 0);
	SPDK_CU_ASSERT_FATAL(readsize % bs_blksz == 0);

	memset(buf, 0, readsize);
	iov.iov_base = buf;
	iov.iov_len = readsize;
	for (blob_block = start_blk; blob_block < num_blocks; blob_block += blocks_per_read) {
		if (strcmp(how, "read") == 0) {
			spdk_blob_io_read(blob, ch, buf, blob_block, blocks_per_read,
					  bs_op_complete, NULL);
		} else if (strcmp(how, "readv") == 0) {
			spdk_blob_io_readv(blob, ch, &iov, 1, blob_block, blocks_per_read,
					   bs_op_complete, NULL);
		} else if (strcmp(how, "readv_ext") == 0) {
			/*
			 * This is currently pointless. NULL ext_opts leads to dev->readv(), not
			 * dev->readv_ext().
			 */
			spdk_blob_io_readv_ext(blob, ch, &iov, 1, blob_block, blocks_per_read,
					       bs_op_complete, NULL, NULL);
		} else {
			abort();
		}
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		if (g_bserrno != 0) {
			return false;
		}
		block_ok = ut_esnap_content_is_correct(buf, blocks_per_read * bs_blksz, blob->id,
						       blob_block * bs_blksz, esnap_blksz);
		CU_ASSERT(block_ok);
		if (!block_ok) {
			return false;
		}
	}

	return true;
}

static void
blob_esnap_io_size(uint32_t bs_blksz, uint32_t esnap_blksz)
{
	struct spdk_bs_dev	*dev;
	struct spdk_blob_store	*bs;
	struct spdk_bs_opts	bsopts;
	struct spdk_blob_opts	opts;
	struct ut_esnap_opts	esnap_opts;
	struct spdk_blob	*blob;
	const uint32_t		cluster_sz = 4 * g_phys_blocklen;
	const uint64_t		esnap_num_clusters = 4;
	const uint32_t		esnap_sz = cluster_sz * esnap_num_clusters;
	const uint64_t		esnap_num_blocks = esnap_sz / esnap_blksz;
	const uint64_t		blob_num_blocks = esnap_sz / bs_blksz;
	uint32_t		block;
	struct spdk_io_channel	*bs_ch;

	spdk_bs_opts_init(&bsopts, sizeof(bsopts));
	bsopts.cluster_sz = cluster_sz;
	bsopts.esnap_bs_dev_create = ut_esnap_create;

	/* Create device with desired block size */
	dev = init_dev();
	dev->blocklen = bs_blksz;
	dev->blockcnt = DEV_BUFFER_SIZE / dev->blocklen;

	/* Initialize a new blob store */
	spdk_bs_init(dev, &bsopts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	SPDK_CU_ASSERT_FATAL(g_bs->io_unit_size == bs_blksz);
	bs = g_bs;

	bs_ch = spdk_bs_alloc_io_channel(bs);
	SPDK_CU_ASSERT_FATAL(bs_ch != NULL);

	/* Create and open the esnap clone  */
	ut_spdk_blob_opts_init(&opts);
	ut_esnap_opts_init(esnap_blksz, esnap_num_blocks, __func__, NULL, &esnap_opts);
	opts.esnap_id = &esnap_opts;
	opts.esnap_id_len = sizeof(esnap_opts);
	opts.num_clusters = esnap_num_clusters;
	blob = ut_blob_create_and_open(bs, &opts);
	SPDK_CU_ASSERT_FATAL(blob != NULL);

	/* Verify that large reads return the content of the esnap device */
	CU_ASSERT(blob_esnap_verify_contents(blob, bs_ch, 0, esnap_sz, esnap_sz, "read"));
	CU_ASSERT(blob_esnap_verify_contents(blob, bs_ch, 0, esnap_sz, esnap_sz, "readv"));
	CU_ASSERT(blob_esnap_verify_contents(blob, bs_ch, 0, esnap_sz, esnap_sz, "readv_ext"));
	/* Verify that small reads return the content of the esnap device */
	CU_ASSERT(blob_esnap_verify_contents(blob, bs_ch, 0, esnap_sz, bs_blksz, "read"));
	CU_ASSERT(blob_esnap_verify_contents(blob, bs_ch, 0, esnap_sz, bs_blksz, "readv"));
	CU_ASSERT(blob_esnap_verify_contents(blob, bs_ch, 0, esnap_sz, bs_blksz, "readv_ext"));

	/* Write one blob block at a time; verify that the surrounding blocks are OK */
	for (block = 0; block < blob_num_blocks; block++) {
		char		buf[bs_blksz];
		union ut_word	word;

		word.f.blob_id = 0xfedcba90;
		word.f.lba = block;
		ut_memset8(buf, word.num, bs_blksz);

		spdk_blob_io_write(blob, bs_ch, buf, block, 1, bs_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		if (g_bserrno != 0) {
			break;
		}

		/* Read and verify the block before the current block */
		if (block != 0) {
			spdk_blob_io_read(blob, bs_ch, buf, block - 1, 1, bs_op_complete, NULL);
			poll_threads();
			CU_ASSERT(g_bserrno == 0);
			if (g_bserrno != 0) {
				break;
			}
			CU_ASSERT(ut_esnap_content_is_correct(buf, bs_blksz, word.f.blob_id,
							      (block - 1) * bs_blksz, bs_blksz));
		}

		/* Read and verify the current block */
		spdk_blob_io_read(blob, bs_ch, buf, block, 1, bs_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		if (g_bserrno != 0) {
			break;
		}
		CU_ASSERT(ut_esnap_content_is_correct(buf, bs_blksz, word.f.blob_id,
						      block * bs_blksz, bs_blksz));

		/* Check the block that follows */
		if (block + 1 < blob_num_blocks) {
			g_bserrno = 0xbad;
			spdk_blob_io_read(blob, bs_ch, buf, block + 1, 1, bs_op_complete, NULL);
			poll_threads();
			CU_ASSERT(g_bserrno == 0);
			if (g_bserrno != 0) {
				break;
			}
			CU_ASSERT(ut_esnap_content_is_correct(buf, bs_blksz, blob->id,
							      (block + 1) * bs_blksz,
							      esnap_blksz));
		}
	}

	/* Clean up */
	spdk_bs_free_io_channel(bs_ch);
	g_bserrno = 0xbad;
	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	memset(g_dev_buffer, 0, DEV_BUFFER_SIZE);
}

static void
blob_esnap_io_4096_4096(void)
{
	blob_esnap_io_size(4096, 4096);
}

static void
blob_esnap_io_512_512(void)
{
	blob_esnap_io_size(512, 512);
}

static void
blob_esnap_io_4096_512(void)
{
	blob_esnap_io_size(4096, 512);
}

static void
blob_esnap_io_512_4096(void)
{
	struct spdk_bs_dev	*dev;
	struct spdk_blob_store	*bs;
	struct spdk_bs_opts	bs_opts;
	struct spdk_blob_opts	blob_opts;
	struct ut_esnap_opts	esnap_opts;
	uint64_t		cluster_sz = 4 * g_phys_blocklen;
	uint32_t		bs_blksz = 512;
	uint32_t		esnap_blksz = BLOCKLEN;
	uint64_t		esnap_num_blocks = 64;
	spdk_blob_id		blobid;

	/* Create device with desired block size */
	dev = init_dev();
	dev->blocklen = bs_blksz;
	dev->blockcnt = DEV_BUFFER_SIZE / dev->blocklen;

	/* Initialize a new blob store */
	spdk_bs_opts_init(&bs_opts, sizeof(bs_opts));
	bs_opts.cluster_sz = cluster_sz;
	bs_opts.esnap_bs_dev_create = ut_esnap_create;
	spdk_bs_init(dev, &bs_opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	SPDK_CU_ASSERT_FATAL(g_bs->io_unit_size == bs_blksz);
	bs = g_bs;

	/* Try to create and open the esnap clone. Create should succeed, open should fail. */
	ut_spdk_blob_opts_init(&blob_opts);
	ut_esnap_opts_init(esnap_blksz, esnap_num_blocks, __func__, NULL, &esnap_opts);
	blob_opts.esnap_id = &esnap_opts;
	blob_opts.esnap_id_len = sizeof(esnap_opts);
	blob_opts.num_clusters = esnap_num_blocks * esnap_blksz / bs_blksz;
	spdk_bs_create_blob_ext(bs, &blob_opts, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;
	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EINVAL);
	CU_ASSERT(g_blob == NULL);

	/* Clean up */
	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	memset(g_dev_buffer, 0, DEV_BUFFER_SIZE);
}

static void
blob_esnap_thread_add_remove(void)
{
	struct spdk_blob_store	*bs = g_bs;
	struct spdk_blob_opts	opts;
	struct ut_esnap_opts	ut_esnap_opts;
	struct spdk_blob	*blob;
	struct ut_esnap_dev	*ut_dev;
	spdk_blob_id		blobid;
	uint64_t		start_thread = g_ut_thread_id;
	bool			destroyed = false;
	struct spdk_io_channel	*ch0, *ch1;
	struct ut_esnap_channel	*ut_ch0, *ut_ch1;
	const uint32_t		blocklen = bs->io_unit_size;
	char			buf[blocklen * 4];

	SPDK_CU_ASSERT_FATAL(g_ut_num_threads > 1);
	set_thread(0);

	/* Create the esnap clone */
	ut_esnap_opts_init(blocklen, 2048, "add_remove_1", &destroyed, &ut_esnap_opts);
	ut_spdk_blob_opts_init(&opts);
	opts.esnap_id = &ut_esnap_opts;
	opts.esnap_id_len = sizeof(ut_esnap_opts);
	opts.num_clusters = 10;
	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	/* Open the blob. No channels should be allocated yet. */
	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;
	ut_dev = (struct ut_esnap_dev *)blob->back_bs_dev;
	CU_ASSERT(ut_dev != NULL);
	CU_ASSERT(ut_dev->num_channels == 0);

	/* Create a channel on thread 0. It is lazily created on the first read. */
	ch0 = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(ch0 != NULL);
	ut_ch0 = ut_esnap_get_io_channel(ch0, blobid);
	CU_ASSERT(ut_ch0 == NULL);
	CU_ASSERT(ut_dev->num_channels == 0);
	spdk_blob_io_read(blob, ch0, buf, 0, 1, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(ut_dev->num_channels == 1);
	ut_ch0 = ut_esnap_get_io_channel(ch0, blobid);
	CU_ASSERT(ut_ch0 != NULL);
	CU_ASSERT(ut_ch0->blocks_read == 1);

	/* Create a channel on thread 1 and verify its lazy creation too. */
	set_thread(1);
	ch1 = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(ch1 != NULL);
	ut_ch1 = ut_esnap_get_io_channel(ch1, blobid);
	CU_ASSERT(ut_ch1 == NULL);
	CU_ASSERT(ut_dev->num_channels == 1);
	spdk_blob_io_read(blob, ch1, buf, 0, 4, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(ut_dev->num_channels == 2);
	ut_ch1 = ut_esnap_get_io_channel(ch1, blobid);
	CU_ASSERT(ut_ch1 != NULL);
	CU_ASSERT(ut_ch1->blocks_read == 4);

	/* Close the channel on thread 0 and verify the bs_dev channel is also gone. */
	set_thread(0);
	spdk_bs_free_io_channel(ch0);
	poll_threads();
	CU_ASSERT(ut_dev->num_channels == 1);

	/* Close the blob. There is no outstanding IO so it should close right away. */
	g_bserrno = 0xbad;
	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(destroyed);

	/* The esnap channel for the blob should be gone now too. */
	ut_ch1 = ut_esnap_get_io_channel(ch1, blobid);
	CU_ASSERT(ut_ch1 == NULL);

	/* Clean up */
	set_thread(1);
	spdk_bs_free_io_channel(ch1);
	set_thread(start_thread);
}

static void
freeze_done(void *cb_arg, int bserrno)
{
	uint32_t *freeze_cnt = cb_arg;

	CU_ASSERT(bserrno == 0);
	(*freeze_cnt)++;
}

static void
unfreeze_done(void *cb_arg, int bserrno)
{
	uint32_t *unfreeze_cnt = cb_arg;

	CU_ASSERT(bserrno == 0);
	(*unfreeze_cnt)++;
}

static void
blob_nested_freezes(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob *blob;
	struct spdk_io_channel *channel[2];
	struct spdk_blob_opts opts;
	uint32_t freeze_cnt, unfreeze_cnt;
	int i;

	for (i = 0; i < 2; i++) {
		set_thread(i);
		channel[i] = spdk_bs_alloc_io_channel(bs);
		SPDK_CU_ASSERT_FATAL(channel[i] != NULL);
	}

	set_thread(0);

	ut_spdk_blob_opts_init(&opts);
	blob = ut_blob_create_and_open(bs, &opts);

	/* First just test a single freeze/unfreeze. */
	freeze_cnt = 0;
	unfreeze_cnt = 0;
	CU_ASSERT(blob->frozen_refcnt == 0);
	blob_freeze_io(blob, freeze_done, &freeze_cnt);
	CU_ASSERT(blob->frozen_refcnt == 1);
	CU_ASSERT(freeze_cnt == 0);
	poll_threads();
	CU_ASSERT(freeze_cnt == 1);
	blob_unfreeze_io(blob, unfreeze_done, &unfreeze_cnt);
	CU_ASSERT(blob->frozen_refcnt == 0);
	CU_ASSERT(unfreeze_cnt == 0);
	poll_threads();
	CU_ASSERT(unfreeze_cnt == 1);

	/* Now nest multiple freeze/unfreeze operations.  We should
	 * expect a callback for each operation, but only after
	 * the threads have been polled to ensure a for_each_channel()
	 * was executed.
	 */
	freeze_cnt = 0;
	unfreeze_cnt = 0;
	CU_ASSERT(blob->frozen_refcnt == 0);
	blob_freeze_io(blob, freeze_done, &freeze_cnt);
	CU_ASSERT(blob->frozen_refcnt == 1);
	CU_ASSERT(freeze_cnt == 0);
	blob_freeze_io(blob, freeze_done, &freeze_cnt);
	CU_ASSERT(blob->frozen_refcnt == 2);
	CU_ASSERT(freeze_cnt == 0);
	poll_threads();
	CU_ASSERT(freeze_cnt == 2);
	blob_unfreeze_io(blob, unfreeze_done, &unfreeze_cnt);
	CU_ASSERT(blob->frozen_refcnt == 1);
	CU_ASSERT(unfreeze_cnt == 0);
	blob_unfreeze_io(blob, unfreeze_done, &unfreeze_cnt);
	CU_ASSERT(blob->frozen_refcnt == 0);
	CU_ASSERT(unfreeze_cnt == 0);
	poll_threads();
	CU_ASSERT(unfreeze_cnt == 2);

	for (i = 0; i < 2; i++) {
		set_thread(i);
		spdk_bs_free_io_channel(channel[i]);
	}
	set_thread(0);
	ut_blob_close_and_delete(bs, blob);

	poll_threads();
	g_blob = NULL;
	g_blobid = 0;
}

static void
blob_ext_md_pages(void)
{
	struct spdk_blob_store *bs;
	struct spdk_bs_dev *dev;
	struct spdk_blob *blob;
	struct spdk_blob_opts opts;
	struct spdk_bs_opts bs_opts;
	uint64_t free_clusters;

	dev = init_dev();
	spdk_bs_opts_init(&bs_opts, sizeof(bs_opts));
	snprintf(bs_opts.bstype.bstype, sizeof(bs_opts.bstype.bstype), "TESTTYPE");
	/* Issue #2932 was a bug in how we use bs_allocate_cluster() during resize.
	 * It requires num_md_pages that is much smaller than the number of clusters.
	 * Make sure we can create a blob that uses all of the free clusters.
	 */
	bs_opts.cluster_sz = 65536;
	bs_opts.num_md_pages = 16;

	/* Initialize a new blob store */
	spdk_bs_init(dev, &bs_opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	bs = g_bs;

	free_clusters = spdk_bs_free_cluster_count(bs);

	ut_spdk_blob_opts_init(&opts);
	opts.num_clusters = free_clusters;

	blob = ut_blob_create_and_open(bs, &opts);
	spdk_blob_close(blob, blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == 0);

	spdk_bs_unload(bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
}

static void
blob_esnap_clone_snapshot(void)
{
	/*
	 * When a snapshot is created, the blob that is being snapped becomes
	 * the leaf node (a clone of the snapshot) and the newly created
	 * snapshot sits between the snapped blob and the external snapshot.
	 *
	 * Before creating snap1
	 *
	 *   ,--------.     ,----------.
	 *   |  blob  |     |  vbdev   |
	 *   | blob1  |<----| nvme1n42 |
	 *   |  (rw)  |     |   (ro)   |
	 *   `--------'     `----------'
	 *       Figure 1
	 *
	 * After creating snap1
	 *
	 *   ,--------.     ,--------.     ,----------.
	 *   |  blob  |     |  blob  |     |  vbdev   |
	 *   | blob1  |<----| snap1  |<----| nvme1n42 |
	 *   |  (rw)  |     |  (ro)  |     |   (ro)   |
	 *   `--------'     `--------'     `----------'
	 *       Figure 2
	 *
	 * Starting from Figure 2, if snap1 is removed, the chain reverts to
	 * what it looks like in Figure 1.
	 *
	 * Starting from Figure 2, if blob1 is removed, the chain becomes:
	 *
	 *   ,--------.     ,----------.
	 *   |  blob  |     |  vbdev   |
	 *   | snap1  |<----| nvme1n42 |
	 *   |  (ro)  |     |   (ro)   |
	 *   `--------'     `----------'
	 *       Figure 3
	 *
	 * In each case, the blob pointed to by the nvme vbdev is considered
	 * the "esnap clone".  The esnap clone must have:
	 *
	 *   - XATTR_INTERNAL for BLOB_EXTERNAL_SNAPSHOT_ID (e.g. name or UUID)
	 *   - blob->invalid_flags must contain SPDK_BLOB_EXTERNAL_SNAPSHOT
	 *   - blob->parent_id must be SPDK_BLOBID_EXTERNAL_SNAPSHOT.
	 *
	 * No other blob that descends from the esnap clone may have any of
	 * those set.
	 */
	struct spdk_blob_store	*bs = g_bs;
	const uint32_t		blocklen = bs->io_unit_size;
	struct spdk_blob_opts	opts;
	struct ut_esnap_opts	esnap_opts;
	struct spdk_blob	*blob, *snap_blob;
	spdk_blob_id		blobid, snap_blobid;
	bool			destroyed = false;

	/* Create the esnap clone */
	ut_esnap_opts_init(blocklen, 2048, __func__, &destroyed, &esnap_opts);
	ut_spdk_blob_opts_init(&opts);
	opts.esnap_id = &esnap_opts;
	opts.esnap_id_len = sizeof(esnap_opts);
	opts.num_clusters = 10;
	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	/* Open the blob. */
	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;
	UT_ASSERT_IS_ESNAP_CLONE(blob, &esnap_opts, sizeof(esnap_opts));

	/*
	 * Create a snapshot of the blob. The snapshot becomes the esnap clone.
	 */
	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snap_blobid = g_blobid;

	spdk_bs_open_blob(bs, snap_blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snap_blob = g_blob;

	UT_ASSERT_IS_NOT_ESNAP_CLONE(blob);
	UT_ASSERT_IS_ESNAP_CLONE(snap_blob, &esnap_opts, sizeof(esnap_opts));

	/*
	 * Delete the snapshot.  The original blob becomes the esnap clone.
	 */
	ut_blob_close_and_delete(bs, snap_blob);
	snap_blob = NULL;
	snap_blobid = SPDK_BLOBID_INVALID;
	UT_ASSERT_IS_ESNAP_CLONE(blob, &esnap_opts, sizeof(esnap_opts));

	/*
	 * Create the snapshot again, then delete the original blob.  The
	 * snapshot should survive as the esnap clone.
	 */
	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snap_blobid = g_blobid;

	spdk_bs_open_blob(bs, snap_blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snap_blob = g_blob;

	UT_ASSERT_IS_NOT_ESNAP_CLONE(blob);
	UT_ASSERT_IS_ESNAP_CLONE(snap_blob, &esnap_opts, sizeof(esnap_opts));

	ut_blob_close_and_delete(bs, blob);
	blob = NULL;
	blobid = SPDK_BLOBID_INVALID;
	UT_ASSERT_IS_ESNAP_CLONE(snap_blob, &esnap_opts, sizeof(esnap_opts));

	/*
	 * Clone the snapshot.  The snapshot continues to be the esnap clone.
	 */
	spdk_bs_create_clone(bs, snap_blobid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;

	UT_ASSERT_IS_NOT_ESNAP_CLONE(blob);
	UT_ASSERT_IS_ESNAP_CLONE(snap_blob, &esnap_opts, sizeof(esnap_opts));

	/*
	 * Delete the snapshot. The clone becomes the esnap clone.
	 */
	ut_blob_close_and_delete(bs, snap_blob);
	snap_blob = NULL;
	snap_blobid = SPDK_BLOBID_INVALID;
	UT_ASSERT_IS_ESNAP_CLONE(blob, &esnap_opts, sizeof(esnap_opts));

	/*
	 * Clean up
	 */
	ut_blob_close_and_delete(bs, blob);
}

static uint64_t
_blob_esnap_clone_hydrate(bool inflate)
{
	struct spdk_blob_store	*bs = g_bs;
	struct spdk_blob_opts	opts;
	struct ut_esnap_opts	esnap_opts;
	struct spdk_blob	*blob;
	spdk_blob_id		blobid;
	struct spdk_io_channel *channel;
	bool			destroyed = false;
	const uint32_t		blocklen = spdk_bs_get_io_unit_size(bs);
	const uint32_t		cluster_sz = spdk_bs_get_cluster_size(bs);
	const uint64_t		esnap_num_clusters = 4;
	const uint32_t		esnap_sz = cluster_sz * esnap_num_clusters;
	const uint64_t		esnap_num_blocks = esnap_sz / blocklen;
	uint64_t		num_failures = CU_get_number_of_failures();

	channel = spdk_bs_alloc_io_channel(bs);
	SPDK_CU_ASSERT_FATAL(channel != NULL);

	/* Create the esnap clone */
	ut_spdk_blob_opts_init(&opts);
	ut_esnap_opts_init(blocklen, esnap_num_blocks, __func__, &destroyed, &esnap_opts);
	opts.esnap_id = &esnap_opts;
	opts.esnap_id_len = sizeof(esnap_opts);
	opts.num_clusters = esnap_num_clusters;
	spdk_bs_create_blob_ext(bs, &opts, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;

	/* Open the esnap clone */
	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	blob = g_blob;
	UT_ASSERT_IS_ESNAP_CLONE(blob, &esnap_opts, sizeof(esnap_opts));

	/*
	 * Inflate or decouple  the blob then verify that it is no longer an esnap clone and has
	 * right content
	 */
	if (inflate) {
		spdk_bs_inflate_blob(bs, channel, blobid, blob_op_complete, NULL);
	} else {
		spdk_bs_blob_decouple_parent(bs, channel, blobid, blob_op_complete, NULL);
	}
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	UT_ASSERT_IS_NOT_ESNAP_CLONE(blob);
	CU_ASSERT(blob_esnap_verify_contents(blob, channel, 0, esnap_sz, esnap_sz, "read"));
	ut_blob_close_and_delete(bs, blob);

	/*
	 * Clean up
	 */
	spdk_bs_free_io_channel(channel);
	poll_threads();

	/* Return number of new failures */
	return CU_get_number_of_failures() - num_failures;
}

static void
blob_esnap_clone_inflate(void)
{
	_blob_esnap_clone_hydrate(true);
}

static void
blob_esnap_clone_decouple(void)
{
	_blob_esnap_clone_hydrate(false);
}

static void
blob_esnap_hotplug(void)
{
	struct spdk_blob_store	*bs = g_bs;
	struct ut_esnap_opts	esnap1_opts, esnap2_opts;
	struct spdk_blob_opts	opts;
	struct spdk_blob	*blob;
	struct spdk_bs_dev	*bs_dev;
	struct ut_esnap_dev	*esnap_dev;
	uint32_t		cluster_sz = spdk_bs_get_cluster_size(bs);
	uint32_t		block_sz = spdk_bs_get_io_unit_size(bs);
	const uint32_t		esnap_num_clusters = 4;
	uint64_t		esnap_num_blocks = cluster_sz * esnap_num_clusters / block_sz;
	bool			destroyed1 = false, destroyed2 = false;
	uint64_t		start_thread = g_ut_thread_id;
	struct spdk_io_channel	*ch0, *ch1;
	char			buf[block_sz];

	/* Create and open an esnap clone blob */
	ut_spdk_blob_opts_init(&opts);
	ut_esnap_opts_init(block_sz, esnap_num_blocks, "esnap1", &destroyed1, &esnap1_opts);
	opts.esnap_id = &esnap1_opts;
	opts.esnap_id_len = sizeof(esnap1_opts);
	opts.num_clusters = esnap_num_clusters;
	blob = ut_blob_create_and_open(bs, &opts);
	CU_ASSERT(blob != NULL);
	CU_ASSERT(spdk_blob_is_esnap_clone(blob));
	SPDK_CU_ASSERT_FATAL(blob->back_bs_dev != NULL);
	esnap_dev = (struct ut_esnap_dev *)blob->back_bs_dev;
	CU_ASSERT(strcmp(esnap_dev->ut_opts.name, "esnap1") == 0);

	/* Replace the external snapshot */
	ut_esnap_opts_init(block_sz, esnap_num_blocks, "esnap2", &destroyed2, &esnap2_opts);
	bs_dev = ut_esnap_dev_alloc(&esnap2_opts);
	CU_ASSERT(!destroyed1);
	CU_ASSERT(!destroyed2);
	g_bserrno = 0xbad;
	spdk_blob_set_esnap_bs_dev(blob, bs_dev, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(destroyed1);
	CU_ASSERT(!destroyed2);
	SPDK_CU_ASSERT_FATAL(bs_dev == blob->back_bs_dev);
	SPDK_CU_ASSERT_FATAL(bs_dev == spdk_blob_get_esnap_bs_dev(blob));
	esnap_dev = (struct ut_esnap_dev *)blob->back_bs_dev;
	CU_ASSERT(strcmp(esnap_dev->ut_opts.name, "esnap2") == 0);

	/* Create a couple channels */
	set_thread(0);
	ch0 = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(ch0 != NULL);
	spdk_blob_io_read(blob, ch0, buf, 0, 1, bs_op_complete, NULL);
	set_thread(1);
	ch1 = spdk_bs_alloc_io_channel(bs);
	CU_ASSERT(ch1 != NULL);
	spdk_blob_io_read(blob, ch1, buf, 0, 1, bs_op_complete, NULL);
	set_thread(start_thread);
	poll_threads();
	CU_ASSERT(esnap_dev->num_channels == 2);

	/* Replace the external snapshot */
	ut_esnap_opts_init(block_sz, esnap_num_blocks, "esnap1a", &destroyed1, &esnap1_opts);
	bs_dev = ut_esnap_dev_alloc(&esnap1_opts);
	destroyed1 = destroyed2 = false;
	g_bserrno = 0xbad;
	spdk_blob_set_esnap_bs_dev(blob, bs_dev, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(!destroyed1);
	CU_ASSERT(destroyed2);
	SPDK_CU_ASSERT_FATAL(blob->back_bs_dev != NULL);
	esnap_dev = (struct ut_esnap_dev *)blob->back_bs_dev;
	CU_ASSERT(strcmp(esnap_dev->ut_opts.name, "esnap1a") == 0);

	/* Clean up */
	set_thread(0);
	spdk_bs_free_io_channel(ch0);
	set_thread(1);
	spdk_bs_free_io_channel(ch1);
	set_thread(start_thread);
	g_bserrno = 0xbad;
	spdk_blob_close(blob, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
}

static bool g_blob_is_degraded;
static int g_blob_is_degraded_called;

static bool
_blob_is_degraded(struct spdk_bs_dev *dev)
{
	g_blob_is_degraded_called++;
	return g_blob_is_degraded;
}

static void
blob_is_degraded(void)
{
	struct spdk_bs_dev bs_is_degraded_null = { 0 };
	struct spdk_bs_dev bs_is_degraded = { .is_degraded = _blob_is_degraded };

	/* No back_bs_dev, no bs->dev->is_degraded */
	g_blob_is_degraded_called = 0;
	CU_ASSERT(!spdk_blob_is_degraded(g_blob));
	CU_ASSERT(g_blob_is_degraded_called == 0);

	/* No back_bs_dev, blobstore device degraded */
	g_bs->dev->is_degraded = _blob_is_degraded;
	g_blob_is_degraded_called = 0;
	g_blob_is_degraded = true;
	CU_ASSERT(spdk_blob_is_degraded(g_blob));
	CU_ASSERT(g_blob_is_degraded_called == 1);

	/* No back_bs_dev, blobstore device not degraded */
	g_bs->dev->is_degraded = _blob_is_degraded;
	g_blob_is_degraded_called = 0;
	g_blob_is_degraded = false;
	CU_ASSERT(!spdk_blob_is_degraded(g_blob));
	CU_ASSERT(g_blob_is_degraded_called == 1);

	/* back_bs_dev does not define is_degraded, no bs->dev->is_degraded */
	g_bs->dev->is_degraded = NULL;
	g_blob->back_bs_dev = &bs_is_degraded_null;
	g_blob_is_degraded_called = 0;
	g_blob_is_degraded = false;
	CU_ASSERT(!spdk_blob_is_degraded(g_blob));
	CU_ASSERT(g_blob_is_degraded_called == 0);

	/* back_bs_dev is not degraded, no bs->dev->is_degraded */
	g_bs->dev->is_degraded = NULL;
	g_blob->back_bs_dev = &bs_is_degraded;
	g_blob_is_degraded_called = 0;
	g_blob_is_degraded = false;
	CU_ASSERT(!spdk_blob_is_degraded(g_blob));
	CU_ASSERT(g_blob_is_degraded_called == 1);

	/* back_bs_dev is degraded, no bs->dev->is_degraded */
	g_bs->dev->is_degraded = NULL;
	g_blob->back_bs_dev = &bs_is_degraded;
	g_blob_is_degraded_called = 0;
	g_blob_is_degraded = true;
	CU_ASSERT(spdk_blob_is_degraded(g_blob));
	CU_ASSERT(g_blob_is_degraded_called == 1);

	/* back_bs_dev is not degraded, blobstore device is not degraded */
	g_bs->dev->is_degraded = _blob_is_degraded;
	g_blob->back_bs_dev = &bs_is_degraded;
	g_blob_is_degraded_called = 0;
	g_blob_is_degraded = false;
	CU_ASSERT(!spdk_blob_is_degraded(g_blob));
	CU_ASSERT(g_blob_is_degraded_called == 2);

	g_blob->back_bs_dev = NULL;
}

/* Resize a blob which is a clone created from snapshot. Verify read/writes to
 * expanded clone blob. Then inflate the clone blob. */
static void
blob_clone_resize(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob_opts opts;
	struct spdk_blob *blob, *clone, *snap_blob, *snap_blob_rsz;
	spdk_blob_id blobid, cloneid, snapid1, snapid2;
	uint64_t pages_per_cluster;
	uint8_t payload_read[bs->dev->blocklen];
	uint8_t payload_write[bs->dev->blocklen];
	struct spdk_io_channel *channel;
	uint64_t free_clusters;

	channel = spdk_bs_alloc_io_channel(bs);
	SPDK_CU_ASSERT_FATAL(channel != NULL);

	pages_per_cluster = spdk_bs_get_cluster_size(bs) / spdk_bs_get_page_size(bs);

	/* Create blob with 10 clusters */
	ut_spdk_blob_opts_init(&opts);
	opts.num_clusters = 10;

	blob = ut_blob_create_and_open(bs, &opts);
	blobid = spdk_blob_get_id(blob);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == 10);

	/* Create snapshot */
	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snapid1 = g_blobid;

	spdk_bs_create_clone(bs, snapid1, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	cloneid = g_blobid;

	spdk_bs_open_blob(bs, cloneid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	clone = g_blob;
	CU_ASSERT(spdk_blob_get_num_clusters(clone) == 10);

	g_bserrno = -1;
	spdk_blob_resize(clone, 20, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(spdk_blob_get_num_clusters(clone) == 20);

	/* Create another snapshot after resizing the clone */
	spdk_bs_create_snapshot(bs, cloneid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	snapid2 = g_blobid;

	/* Open the snapshot blobs */
	spdk_bs_open_blob(bs, snapid1, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snap_blob = g_blob;
	CU_ASSERT(snap_blob->data_ro == true);
	CU_ASSERT(snap_blob->md_ro == true);
	CU_ASSERT(spdk_blob_get_num_clusters(snap_blob) == 10);

	spdk_bs_open_blob(bs, snapid2, blob_op_with_handle_complete, NULL);
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blob != NULL);
	snap_blob_rsz = g_blob;
	CU_ASSERT(snap_blob_rsz->data_ro == true);
	CU_ASSERT(snap_blob_rsz->md_ro == true);
	CU_ASSERT(spdk_blob_get_num_clusters(snap_blob_rsz) == 20);

	/* Confirm that clone is backed by snap_blob_rsz, and snap_blob_rsz is backed by snap_blob */
	SPDK_CU_ASSERT_FATAL(snap_blob->back_bs_dev == NULL);
	SPDK_CU_ASSERT_FATAL(blob->back_bs_dev != NULL);
	SPDK_CU_ASSERT_FATAL(snap_blob_rsz->back_bs_dev != NULL);

	/* Write and read from pre-resize ranges */
	g_bserrno = -1;
	memset(payload_write, 0xE5, sizeof(payload_write));
	spdk_blob_io_write(clone, channel, payload_write, 5 * pages_per_cluster, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	g_bserrno = -1;
	memset(payload_read, 0x00, sizeof(payload_read));
	spdk_blob_io_read(clone, channel, payload_read, 5 * pages_per_cluster, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_write, payload_read, BLOCKLEN) == 0);

	/* Write and read from post-resize ranges */
	g_bserrno = -1;
	memset(payload_write, 0xE5, sizeof(payload_write));
	spdk_blob_io_write(clone, channel, payload_write, 15 * pages_per_cluster, 1, blob_op_complete,
			   NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	g_bserrno = -1;
	memset(payload_read, 0x00, sizeof(payload_read));
	spdk_blob_io_read(clone, channel, payload_read, 15 * pages_per_cluster, 1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(memcmp(payload_write, payload_read, bs->dev->blocklen) == 0);

	/* Now do full blob inflation of the resized blob/clone. */
	free_clusters = spdk_bs_free_cluster_count(bs);
	spdk_bs_inflate_blob(bs, channel, cloneid, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	/* We wrote to 2 clusters earlier, all remaining 18 clusters in
	 * blob should get allocated after inflation */
	CU_ASSERT(spdk_bs_free_cluster_count(bs) == free_clusters - 18);

	spdk_blob_close(clone, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(snap_blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	spdk_blob_close(snap_blob_rsz, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	ut_blob_close_and_delete(bs, blob);

	spdk_bs_free_io_channel(channel);
}


static void
blob_esnap_clone_resize(void)
{
	struct spdk_bs_dev *dev;
	struct spdk_blob_store *bs;
	struct spdk_bs_opts bsopts;
	struct spdk_blob_opts opts;
	struct ut_esnap_opts esnap_opts;
	struct spdk_blob *blob;
	uint32_t block, esnap_blksz = 512, bs_blksz = 512;
	const uint32_t cluster_sz = 4 * g_phys_blocklen;
	const uint64_t esnap_num_clusters = 4;
	const uint32_t esnap_sz = cluster_sz * esnap_num_clusters;
	const uint64_t esnap_num_blocks = esnap_sz / esnap_blksz;
	uint64_t blob_num_blocks = esnap_sz / bs_blksz;
	struct spdk_io_channel *bs_ch;

	spdk_bs_opts_init(&bsopts, sizeof(bsopts));
	bsopts.cluster_sz = cluster_sz;
	bsopts.esnap_bs_dev_create = ut_esnap_create;
	/* Create device with desired block size */
	dev = init_dev();
	dev->blocklen = bs_blksz;
	dev->blockcnt = DEV_BUFFER_SIZE / dev->blocklen;
	/* Initialize a new blob store */
	spdk_bs_init(dev, &bsopts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
	SPDK_CU_ASSERT_FATAL(g_bs->io_unit_size == bs_blksz);
	bs = g_bs;

	bs_ch = spdk_bs_alloc_io_channel(bs);
	SPDK_CU_ASSERT_FATAL(bs_ch != NULL);

	/* Create and open the esnap clone  */
	ut_spdk_blob_opts_init(&opts);
	ut_esnap_opts_init(esnap_blksz, esnap_num_blocks, __func__, NULL, &esnap_opts);
	opts.esnap_id = &esnap_opts;
	opts.esnap_id_len = sizeof(esnap_opts);
	opts.num_clusters = esnap_num_clusters;
	blob = ut_blob_create_and_open(bs, &opts);
	SPDK_CU_ASSERT_FATAL(blob != NULL);

	g_bserrno = -1;
	spdk_blob_resize(blob, esnap_num_clusters * 2, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(spdk_blob_get_num_clusters(blob) == esnap_num_clusters * 2);

	/* Write one blob block at a time; verify that the surrounding blocks are OK */
	blob_num_blocks = (spdk_blob_get_num_clusters(blob) * cluster_sz) / bs_blksz;
	for (block = 0; block < blob_num_blocks; block++) {
		char buf[bs_blksz];
		union ut_word word;
		word.f.blob_id = 0xfedcba90;
		word.f.lba = block;
		ut_memset8(buf, word.num, bs_blksz);
		spdk_blob_io_write(blob, bs_ch, buf, block, 1, bs_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		if (g_bserrno != 0) {
			break;
		}
		/* Read and verify the block before the current block */
		if (block != 0) {
			spdk_blob_io_read(blob, bs_ch, buf, block - 1, 1, bs_op_complete, NULL);
			poll_threads();
			CU_ASSERT(g_bserrno == 0);
			if (g_bserrno != 0) {
				break;
			}
			CU_ASSERT(ut_esnap_content_is_correct(buf, bs_blksz, word.f.blob_id,
							      (block - 1) * bs_blksz, bs_blksz));
		}
		/* Read and verify the current block */
		spdk_blob_io_read(blob, bs_ch, buf, block, 1, bs_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		if (g_bserrno != 0) {
			break;
		}
		CU_ASSERT(ut_esnap_content_is_correct(buf, bs_blksz, word.f.blob_id,
						      block * bs_blksz, bs_blksz));
		/* Check the block that follows */
		if (block + 1 < blob_num_blocks) {
			g_bserrno = 0xbad;
			spdk_blob_io_read(blob, bs_ch, buf, block + 1, 1, bs_op_complete, NULL);
			poll_threads();
			CU_ASSERT(g_bserrno == 0);
			if (g_bserrno != 0) {
				break;
			}
			CU_ASSERT(ut_esnap_content_is_correct(buf, bs_blksz, blob->id,
							      (block + 1) * bs_blksz,
							      esnap_blksz));
		}
	}
	/* Clean up */
	spdk_bs_free_io_channel(bs_ch);
	g_bserrno = 0xbad;
	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	spdk_bs_unload(g_bs, bs_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bs = NULL;
	memset(g_dev_buffer, 0, DEV_BUFFER_SIZE);
}

static void
bs_dev_io_complete_cb(struct spdk_io_channel *channel, void *cb_arg, int bserrno)
{
	g_bserrno = bserrno;
}

static void
blob_shallow_copy(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob_opts blob_opts;
	struct spdk_blob *blob;
	spdk_blob_id blobid;
	uint64_t num_clusters = 4;
	struct spdk_bs_dev *ext_dev;
	struct spdk_bs_dev_cb_args ext_args;
	struct spdk_io_channel *bdev_ch, *blob_ch;
	uint8_t buf1[DEV_BUFFER_BLOCKLEN];
	uint8_t buf2[DEV_BUFFER_BLOCKLEN];
	uint64_t io_units_per_cluster;
	uint64_t offset;
	int rc;

	blob_ch = spdk_bs_alloc_io_channel(bs);
	SPDK_CU_ASSERT_FATAL(blob_ch != NULL);

	/* Set blob dimension and as thin provisioned */
	ut_spdk_blob_opts_init(&blob_opts);
	blob_opts.thin_provision = true;
	blob_opts.num_clusters = num_clusters;

	/* Create a blob */
	blob = ut_blob_create_and_open(bs, &blob_opts);
	SPDK_CU_ASSERT_FATAL(blob != NULL);
	blobid = spdk_blob_get_id(blob);
	io_units_per_cluster = bs_io_units_per_cluster(blob);

	/* Write on cluster 2 and 4 of blob */
	for (offset = io_units_per_cluster; offset < 2 * io_units_per_cluster; offset++) {
		memset(buf1, offset, DEV_BUFFER_BLOCKLEN);
		spdk_blob_io_write(blob, blob_ch, buf1, offset, 1, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
	}
	for (offset = 3 * io_units_per_cluster; offset < 4 * io_units_per_cluster; offset++) {
		memset(buf1, offset, DEV_BUFFER_BLOCKLEN);
		spdk_blob_io_write(blob, blob_ch, buf1, offset, 1, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
	}
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 2);

	/* Make a snapshot over blob */
	spdk_bs_create_snapshot(bs, blobid, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 0);

	/* Write on cluster 1 and 3 of blob */
	for (offset = 0; offset < io_units_per_cluster; offset++) {
		memset(buf1, offset, DEV_BUFFER_BLOCKLEN);
		spdk_blob_io_write(blob, blob_ch, buf1, offset, 1, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
	}
	for (offset = 2 * io_units_per_cluster; offset < 3 * io_units_per_cluster; offset++) {
		memset(buf1, offset, DEV_BUFFER_BLOCKLEN);
		spdk_blob_io_write(blob, blob_ch, buf1, offset, 1, blob_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
	}
	CU_ASSERT(spdk_blob_get_num_allocated_clusters(blob) == 2);

	/* Shallow copy with a not read only blob */
	ext_dev = init_ext_dev(num_clusters * 1024 * 1024, DEV_BUFFER_BLOCKLEN);
	rc = spdk_bs_blob_shallow_copy(bs, blob_ch, blobid, ext_dev,
				       blob_shallow_copy_status_cb, NULL,
				       blob_op_complete, NULL);
	CU_ASSERT(rc == 0);
	poll_threads();
	CU_ASSERT(g_bserrno == -EPERM);
	ext_dev->destroy(ext_dev);

	/* Set blob read only */
	spdk_blob_set_read_only(blob);
	spdk_blob_sync_md(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Shallow copy over a spdk_bs_dev with incorrect size */
	ext_dev = init_ext_dev(1, DEV_BUFFER_BLOCKLEN);
	rc = spdk_bs_blob_shallow_copy(bs, blob_ch, blobid, ext_dev,
				       blob_shallow_copy_status_cb, NULL,
				       blob_op_complete, NULL);
	CU_ASSERT(rc == 0);
	poll_threads();
	CU_ASSERT(g_bserrno == -EINVAL);
	ext_dev->destroy(ext_dev);

	/* Shallow copy over a spdk_bs_dev with incorrect block len */
	ext_dev = init_ext_dev(num_clusters * 1024 * 1024, DEV_BUFFER_BLOCKLEN * 2);
	rc = spdk_bs_blob_shallow_copy(bs, blob_ch, blobid, ext_dev,
				       blob_shallow_copy_status_cb, NULL,
				       blob_op_complete, NULL);
	CU_ASSERT(rc == 0);
	poll_threads();
	CU_ASSERT(g_bserrno == -EINVAL);
	ext_dev->destroy(ext_dev);

	/* Initialize ext_dev for the successuful shallow copy */
	ext_dev = init_ext_dev(num_clusters * 1024 * 1024, DEV_BUFFER_BLOCKLEN);
	bdev_ch = ext_dev->create_channel(ext_dev);
	SPDK_CU_ASSERT_FATAL(bdev_ch != NULL);
	ext_args.cb_fn = bs_dev_io_complete_cb;
	for (offset = 0; offset < 4 * io_units_per_cluster; offset++) {
		memset(buf2, 0xff, DEV_BUFFER_BLOCKLEN);
		ext_dev->write(ext_dev, bdev_ch, buf2, offset, 1, &ext_args);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
	}

	/* Correct shallow copy of blob over bdev */
	rc = spdk_bs_blob_shallow_copy(bs, blob_ch, blobid, ext_dev,
				       blob_shallow_copy_status_cb, NULL,
				       blob_op_complete, NULL);
	CU_ASSERT(rc == 0);
	poll_thread_times(0, 1);
	CU_ASSERT(g_copied_clusters_count == 1);
	poll_thread_times(0, 2);
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_copied_clusters_count == 2);

	/* Read from bdev */
	/* Only cluster 1 and 3 must be filled */
	/* Clusters 2 and 4 should not have been touched */
	for (offset = 0; offset < io_units_per_cluster; offset++) {
		memset(buf1, offset, DEV_BUFFER_BLOCKLEN);
		ext_dev->read(ext_dev, bdev_ch, buf2, offset, 1, &ext_args);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(memcmp(buf1, buf2, DEV_BUFFER_BLOCKLEN) == 0);
	}
	for (offset = io_units_per_cluster; offset < 2 * io_units_per_cluster; offset++) {
		memset(buf1, 0xff, DEV_BUFFER_BLOCKLEN);
		ext_dev->read(ext_dev, bdev_ch, buf2, offset, 1, &ext_args);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(memcmp(buf1, buf2, DEV_BUFFER_BLOCKLEN) == 0);
	}
	for (offset = 2 * io_units_per_cluster; offset < 3 * io_units_per_cluster; offset++) {
		memset(buf1, offset, DEV_BUFFER_BLOCKLEN);
		ext_dev->read(ext_dev, bdev_ch, buf2, offset, 1, &ext_args);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(memcmp(buf1, buf2, DEV_BUFFER_BLOCKLEN) == 0);
	}
	for (offset = 3 * io_units_per_cluster; offset < 4 * io_units_per_cluster; offset++) {
		memset(buf1, 0xff, DEV_BUFFER_BLOCKLEN);
		ext_dev->read(ext_dev, bdev_ch, buf2, offset, 1, &ext_args);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		CU_ASSERT(memcmp(buf1, buf2, DEV_BUFFER_BLOCKLEN) == 0);
	}

	/* Clean up */
	ext_dev->destroy_channel(ext_dev, bdev_ch);
	ext_dev->destroy(ext_dev);
	spdk_bs_free_io_channel(blob_ch);
	ut_blob_close_and_delete(bs, blob);
	poll_threads();
}

static void
blob_set_parent(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob_opts opts;
	struct ut_esnap_opts esnap_opts;
	struct spdk_blob *blob1, *blob2, *blob3, *blob4, *blob5;
	spdk_blob_id blobid1, blobid2, blobid3, blobid4, blobid5,
		     snapshotid1, snapshotid2, snapshotid3;
	uint32_t cluster_sz, block_sz;
	const uint32_t esnap_num_clusters = 4;
	uint64_t esnap_num_blocks;
	spdk_blob_id ids[2];
	size_t clone_count = 2;

	cluster_sz = spdk_bs_get_cluster_size(bs);
	block_sz = spdk_bs_get_io_unit_size(bs);
	esnap_num_blocks = cluster_sz * esnap_num_clusters / block_sz;

	/* Create a normal blob and make a couple of snapshots */
	ut_spdk_blob_opts_init(&opts);
	blob1 = ut_blob_create_and_open(bs, &opts);
	SPDK_CU_ASSERT_FATAL(blob1 != NULL);
	blobid1 = spdk_blob_get_id(blob1);
	spdk_bs_create_snapshot(bs, blobid1, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	SPDK_CU_ASSERT_FATAL(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blobid != SPDK_BLOBID_INVALID);
	snapshotid1 = g_blobid;
	spdk_bs_create_snapshot(bs, blobid1, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	SPDK_CU_ASSERT_FATAL(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blobid != SPDK_BLOBID_INVALID);
	snapshotid2 = g_blobid;

	/* Call set_parent with an invalid snapshotid */
	spdk_bs_blob_set_parent(bs, blobid1, SPDK_BLOBID_INVALID, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EINVAL);

	/* Call set_parent with blobid and snapshotid the same */
	spdk_bs_blob_set_parent(bs, blobid1, blobid1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EINVAL);

	/* Call set_parent with a blob and its parent snapshot */
	spdk_bs_blob_set_parent(bs, blobid1, snapshotid2, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EEXIST);

	/* Create an esnap clone blob */
	ut_spdk_blob_opts_init(&opts);
	ut_esnap_opts_init(block_sz, esnap_num_blocks, __func__, NULL, &esnap_opts);
	opts.esnap_id = &esnap_opts;
	opts.esnap_id_len = sizeof(esnap_opts);
	opts.num_clusters = esnap_num_clusters;
	blob2 = ut_blob_create_and_open(bs, &opts);
	SPDK_CU_ASSERT_FATAL(blob2 != NULL);
	blobid2 = spdk_blob_get_id(blob2);
	CU_ASSERT(spdk_blob_is_esnap_clone(blob2));

	/* Call set_parent with a non snapshot parent */
	spdk_bs_blob_set_parent(bs, blobid2, blobid1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EINVAL);

	/* Call set_parent with blob and snapshot of different size */
	spdk_bs_blob_set_parent(bs, blobid2, snapshotid1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EINVAL);

	/* Call set_parent correctly with a snapshot's clone blob */
	spdk_bs_blob_set_parent(bs, blobid1, snapshotid1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Check relations */
	CU_ASSERT(spdk_blob_is_clone(blob1));
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, blobid1) == snapshotid1);
	CU_ASSERT(spdk_blob_get_clones(bs, snapshotid1, ids, &clone_count) == 0);
	CU_ASSERT(clone_count == 2);
	CU_ASSERT(ids[1] == blobid1);

	/* Create another normal blob with size equal to esnap size and make a snapshot */
	ut_spdk_blob_opts_init(&opts);
	opts.num_clusters = esnap_num_clusters;
	opts.thin_provision = true;
	blob3 = ut_blob_create_and_open(bs, &opts);
	SPDK_CU_ASSERT_FATAL(blob3 != NULL);
	blobid3 = spdk_blob_get_id(blob3);
	spdk_bs_create_snapshot(bs, blobid3, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	SPDK_CU_ASSERT_FATAL(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blobid != SPDK_BLOBID_INVALID);
	snapshotid3 = g_blobid;

	/* Call set_parent correctly with an esnap's clone blob */
	spdk_bs_blob_set_parent(bs, blobid2, snapshotid3, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Check relations */
	CU_ASSERT(!spdk_blob_is_esnap_clone(blob2));
	CU_ASSERT(spdk_blob_is_clone(blob2));
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, blobid2) == snapshotid3);
	CU_ASSERT(spdk_blob_get_clones(bs, snapshotid3, ids, &clone_count) == 0);
	CU_ASSERT(clone_count == 2);
	CU_ASSERT(ids[1] == blobid2);

	/* Create a not thin-provisioned blob that is not a clone */
	ut_spdk_blob_opts_init(&opts);
	opts.thin_provision = false;
	blob4 = ut_blob_create_and_open(bs, &opts);
	SPDK_CU_ASSERT_FATAL(blob4 != NULL);
	blobid4 = spdk_blob_get_id(blob4);

	/* Call set_parent with a blob that isn't a clone and that isn't thin-provisioned */
	spdk_bs_blob_set_parent(bs, blobid4, snapshotid2, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EINVAL);

	/* Create a thin-provisioned blob that is not a clone */
	ut_spdk_blob_opts_init(&opts);
	opts.thin_provision = true;
	blob5 = ut_blob_create_and_open(bs, &opts);
	SPDK_CU_ASSERT_FATAL(blob5 != NULL);
	blobid5 = spdk_blob_get_id(blob5);

	/* Call set_parent correctly with a blob that isn't a clone */
	spdk_bs_blob_set_parent(bs, blobid5, snapshotid2, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Check relations */
	CU_ASSERT(spdk_blob_is_clone(blob5));
	CU_ASSERT(spdk_blob_get_parent_snapshot(bs, blobid5) == snapshotid2);
	CU_ASSERT(spdk_blob_get_clones(bs, snapshotid2, ids, &clone_count) == 0);
	CU_ASSERT(clone_count == 1);
	CU_ASSERT(ids[0] == blobid5);

	/* Clean up */
	ut_blob_close_and_delete(bs, blob5);
	ut_blob_close_and_delete(bs, blob4);
	ut_blob_close_and_delete(bs, blob3);
	ut_blob_close_and_delete(bs, blob2);
	ut_blob_close_and_delete(bs, blob1);
	spdk_bs_delete_blob(bs, snapshotid3, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	spdk_bs_delete_blob(bs, snapshotid2, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	spdk_bs_delete_blob(bs, snapshotid1, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
}

static void
blob_set_external_parent(void)
{
	struct spdk_blob_store *bs = g_bs;
	struct spdk_blob_opts opts;
	struct ut_esnap_opts esnap_opts, esnap_opts2;
	struct spdk_blob *blob1, *blob2, *blob3, *blob4;
	spdk_blob_id blobid1, blobid2, blobid3, blobid4, snapshotid;
	uint32_t cluster_sz, block_sz;
	const uint32_t esnap_num_clusters = 4;
	uint64_t esnap_num_blocks;
	struct spdk_bs_dev *esnap_dev1, *esnap_dev2, *esnap_dev3;
	const void *esnap_id;
	size_t esnap_id_len;
	int rc;

	cluster_sz = spdk_bs_get_cluster_size(bs);
	block_sz = spdk_bs_get_io_unit_size(bs);
	esnap_num_blocks = cluster_sz * esnap_num_clusters / block_sz;
	esnap_dev1 = init_dev();
	esnap_dev2 = init_dev();
	esnap_dev3 = init_dev();

	/* Create an esnap clone blob */
	ut_spdk_blob_opts_init(&opts);
	ut_esnap_opts_init(block_sz, esnap_num_blocks, __func__, NULL, &esnap_opts);
	opts.esnap_id = &esnap_opts;
	opts.esnap_id_len = sizeof(esnap_opts);
	opts.num_clusters = esnap_num_clusters;
	blob1 = ut_blob_create_and_open(bs, &opts);
	SPDK_CU_ASSERT_FATAL(blob1 != NULL);
	blobid1 = spdk_blob_get_id(blob1);
	CU_ASSERT(spdk_blob_is_esnap_clone(blob1));

	/* Call set_esternal_parent with blobid and esnapid the same */
	spdk_bs_blob_set_external_parent(bs, blobid1, esnap_dev1, &blobid1, sizeof(blobid1),
					 blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == -EINVAL);

	/* Call set_external_parent with esnap of incompatible size */
	esnap_dev1->blockcnt = esnap_num_blocks - 1;
	spdk_bs_blob_set_external_parent(bs, blobid1, esnap_dev1, opts.esnap_id, opts.esnap_id_len,
					 blob_op_complete, NULL);
	CU_ASSERT(g_bserrno == -EINVAL);

	/* Call set_external_parent with a blob and its parent esnap */
	esnap_dev1->blocklen = block_sz;
	esnap_dev1->blockcnt = esnap_num_blocks;
	spdk_bs_blob_set_external_parent(bs, blobid1, esnap_dev1, opts.esnap_id, opts.esnap_id_len,
					 blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EEXIST);

	/* Create a blob that is a clone of a snapshots */
	ut_spdk_blob_opts_init(&opts);
	blob2 = ut_blob_create_and_open(bs, &opts);
	SPDK_CU_ASSERT_FATAL(blob2 != NULL);
	blobid2 = spdk_blob_get_id(blob2);
	spdk_bs_create_snapshot(bs, blobid2, NULL, blob_op_with_id_complete, NULL);
	poll_threads();
	SPDK_CU_ASSERT_FATAL(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_blobid != SPDK_BLOBID_INVALID);
	snapshotid = g_blobid;

	/* Call set_parent correctly with a snapshot's clone blob */
	esnap_dev2->blocklen = block_sz;
	esnap_dev2->blockcnt = esnap_num_blocks;
	ut_esnap_opts_init(block_sz, esnap_num_blocks, __func__, NULL, &esnap_opts2);
	spdk_bs_blob_set_external_parent(bs, blobid2, esnap_dev2, &esnap_opts2, sizeof(esnap_opts2),
					 blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Check relations */
	rc = spdk_blob_get_esnap_id(blob2, &esnap_id, &esnap_id_len);
	CU_ASSERT(spdk_blob_is_esnap_clone(blob2));
	CU_ASSERT(!spdk_blob_is_clone(blob2));
	CU_ASSERT(rc == 0 && esnap_id_len == sizeof(esnap_opts2) &&
		  memcmp(esnap_id, &esnap_opts2, esnap_id_len) == 0);
	CU_ASSERT(blob2->parent_id == SPDK_BLOBID_EXTERNAL_SNAPSHOT);

	/* Create a not thin-provisioned blob that is not a clone */
	ut_spdk_blob_opts_init(&opts);
	opts.thin_provision = false;
	blob3 = ut_blob_create_and_open(bs, &opts);
	SPDK_CU_ASSERT_FATAL(blob3 != NULL);
	blobid3 = spdk_blob_get_id(blob3);

	/* Call set_external_parent with a blob that isn't a clone and that isn't thin-provisioned */
	spdk_bs_blob_set_external_parent(bs, blobid3, esnap_dev1, &esnap_opts, sizeof(esnap_opts),
					 blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == -EINVAL);

	/* Create a thin-provisioned blob that is not a clone */
	ut_spdk_blob_opts_init(&opts);
	opts.thin_provision = true;
	blob4 = ut_blob_create_and_open(bs, &opts);
	SPDK_CU_ASSERT_FATAL(blob4 != NULL);
	blobid4 = spdk_blob_get_id(blob4);

	/* Call set_external_parent correctly with a blob that isn't a clone */
	esnap_dev3->blocklen = block_sz;
	esnap_dev3->blockcnt = esnap_num_blocks;
	ut_esnap_opts_init(block_sz, esnap_num_blocks, __func__, NULL, &esnap_opts);
	spdk_bs_blob_set_external_parent(bs, blobid4, esnap_dev3, &esnap_opts, sizeof(esnap_opts),
					 blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);

	/* Check relations */
	rc = spdk_blob_get_esnap_id(blob4, &esnap_id, &esnap_id_len);
	CU_ASSERT(spdk_blob_is_esnap_clone(blob4));
	CU_ASSERT(!spdk_blob_is_clone(blob4));
	CU_ASSERT(rc == 0 && esnap_id_len == sizeof(esnap_opts) &&
		  memcmp(esnap_id, &esnap_opts, esnap_id_len) == 0);
	CU_ASSERT(blob4->parent_id == SPDK_BLOBID_EXTERNAL_SNAPSHOT);

	ut_blob_close_and_delete(bs, blob4);
	ut_blob_close_and_delete(bs, blob3);
	ut_blob_close_and_delete(bs, blob2);
	ut_blob_close_and_delete(bs, blob1);
	spdk_bs_delete_blob(bs, snapshotid, blob_op_complete, NULL);
	dev_destroy(esnap_dev1);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
}

static void
suite_bs_setup(void)
{
	struct spdk_bs_dev *dev;

	dev = init_dev();
	memset(g_dev_buffer, 0, DEV_BUFFER_SIZE);
	spdk_bs_init(dev, NULL, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_bs != NULL);
}

static void
suite_esnap_bs_setup(void)
{
	struct spdk_bs_dev	*dev;
	struct spdk_bs_opts	bs_opts;

	dev = init_dev();
	memset(g_dev_buffer, 0, DEV_BUFFER_SIZE);
	spdk_bs_opts_init(&bs_opts, sizeof(bs_opts));
	bs_opts.cluster_sz = 4 * g_phys_blocklen;
	bs_opts.esnap_bs_dev_create = ut_esnap_create;
	spdk_bs_init(dev, &bs_opts, bs_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	SPDK_CU_ASSERT_FATAL(g_bs != NULL);
}

static void
suite_bs_cleanup(void)
{
	if (g_bs != NULL) {
		spdk_bs_unload(g_bs, bs_op_complete, NULL);
		poll_threads();
		CU_ASSERT(g_bserrno == 0);
		g_bs = NULL;
	}
	memset(g_dev_buffer, 0, DEV_BUFFER_SIZE);
}

static struct spdk_blob *
ut_blob_create_and_open(struct spdk_blob_store *bs, struct spdk_blob_opts *blob_opts)
{
	struct spdk_blob *blob;
	struct spdk_blob_opts create_blob_opts;
	spdk_blob_id blobid;

	if (blob_opts == NULL) {
		ut_spdk_blob_opts_init(&create_blob_opts);
		blob_opts = &create_blob_opts;
	}

	spdk_bs_create_blob_ext(bs, blob_opts, blob_op_with_id_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blobid != SPDK_BLOBID_INVALID);
	blobid = g_blobid;
	g_blobid = -1;

	spdk_bs_open_blob(bs, blobid, blob_op_with_handle_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	CU_ASSERT(g_blob != NULL);
	blob = g_blob;

	g_blob = NULL;
	g_bserrno = -1;

	return blob;
}

static void
ut_blob_close_and_delete(struct spdk_blob_store *bs, struct spdk_blob *blob)
{
	spdk_blob_id blobid = spdk_blob_get_id(blob);

	spdk_blob_close(blob, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_blob = NULL;

	spdk_bs_delete_blob(bs, blobid, blob_op_complete, NULL);
	poll_threads();
	CU_ASSERT(g_bserrno == 0);
	g_bserrno = -1;
}

static void
suite_blob_setup(void)
{
	suite_bs_setup();
	CU_ASSERT(g_bs != NULL);

	g_blob = ut_blob_create_and_open(g_bs, NULL);
	CU_ASSERT(g_blob != NULL);
}

static void
suite_blob_cleanup(void)
{
	ut_blob_close_and_delete(g_bs, g_blob);
	CU_ASSERT(g_blob == NULL);

	suite_bs_cleanup();
	CU_ASSERT(g_bs == NULL);
}

static int
ut_setup_config_nocopy_noextent(void)
{
	g_dev_copy_enabled = false;
	g_use_extent_table = false;
	g_phys_blocklen = 4096;

	return 0;
}

static int
ut_setup_config_nocopy_extent(void)
{
	g_dev_copy_enabled = false;
	g_use_extent_table = true;
	g_phys_blocklen = 4096;

	return 0;
}

static int
ut_setup_config_nocopy_extent_16k_phys(void)
{
	g_dev_copy_enabled = false;
	g_use_extent_table = true;
	g_phys_blocklen = 16384;

	return 0;
}


static int
ut_setup_config_copy_noextent(void)
{
	g_dev_copy_enabled = true;
	g_use_extent_table = false;
	g_phys_blocklen = 4096;

	return 0;
}

static int
ut_setup_config_copy_extent(void)
{
	g_dev_copy_enabled = true;
	g_use_extent_table = true;
	g_phys_blocklen = 4096;

	return 0;
}

struct ut_config {
	const char *suffix;
	CU_InitializeFunc setup_cb;
};

int
main(int argc, char **argv)
{
	CU_pSuite		suite, suite_bs, suite_blob, suite_esnap_bs;
	unsigned int		i, num_failures;
	char			suite_name[4096];
	struct ut_config	*config;
	struct ut_config	configs[] = {
		{"nocopy_noextent", ut_setup_config_nocopy_noextent},
		{"nocopy_extent", ut_setup_config_nocopy_extent},
		{"nocopy_extent_16k_phys", ut_setup_config_nocopy_extent_16k_phys},
		{"copy_noextent", ut_setup_config_copy_noextent},
		{"copy_extent", ut_setup_config_copy_extent},
	};

	CU_initialize_registry();

	for (i = 0; i < SPDK_COUNTOF(configs); ++i) {
		config = &configs[i];

		snprintf(suite_name, sizeof(suite_name), "blob_%s", config->suffix);
		suite = CU_add_suite(suite_name, config->setup_cb, NULL);

		snprintf(suite_name, sizeof(suite_name), "blob_bs_%s", config->suffix);
		suite_bs = CU_add_suite_with_setup_and_teardown(suite_name, config->setup_cb, NULL,
				suite_bs_setup, suite_bs_cleanup);

		snprintf(suite_name, sizeof(suite_name), "blob_blob_%s", config->suffix);
		suite_blob = CU_add_suite_with_setup_and_teardown(suite_name, config->setup_cb, NULL,
				suite_blob_setup, suite_blob_cleanup);

		snprintf(suite_name, sizeof(suite_name), "blob_esnap_bs_%s", config->suffix);
		suite_esnap_bs = CU_add_suite_with_setup_and_teardown(suite_name, config->setup_cb, NULL,
				 suite_esnap_bs_setup,
				 suite_bs_cleanup);

		CU_ADD_TEST(suite, blob_init);
		CU_ADD_TEST(suite_bs, blob_open);
		CU_ADD_TEST(suite_bs, blob_create);
		CU_ADD_TEST(suite_bs, blob_create_loop);
		CU_ADD_TEST(suite_bs, blob_create_fail);
		CU_ADD_TEST(suite_bs, blob_create_internal);
		CU_ADD_TEST(suite_bs, blob_create_zero_extent);
		CU_ADD_TEST(suite, blob_thin_provision);
		CU_ADD_TEST(suite_bs, blob_snapshot);
		CU_ADD_TEST(suite_bs, blob_clone);
		CU_ADD_TEST(suite_bs, blob_inflate);
		CU_ADD_TEST(suite_bs, blob_delete);
		CU_ADD_TEST(suite_bs, blob_resize_test);
		CU_ADD_TEST(suite_bs, blob_resize_thin_test);
		CU_ADD_TEST(suite, blob_read_only);
		CU_ADD_TEST(suite_bs, channel_ops);
		CU_ADD_TEST(suite_bs, blob_super);
		CU_ADD_TEST(suite_blob, blob_write);
		CU_ADD_TEST(suite_blob, blob_read);
		CU_ADD_TEST(suite_blob, blob_rw_verify);
		CU_ADD_TEST(suite_bs, blob_rw_verify_iov);
		CU_ADD_TEST(suite_blob, blob_rw_verify_iov_nomem);
		CU_ADD_TEST(suite_blob, blob_rw_iov_read_only);
		CU_ADD_TEST(suite_bs, blob_unmap);
		CU_ADD_TEST(suite_bs, blob_iter);
		CU_ADD_TEST(suite_blob, blob_xattr);
		CU_ADD_TEST(suite_bs, blob_parse_md);
		CU_ADD_TEST(suite, bs_load);
		CU_ADD_TEST(suite_bs, bs_load_pending_removal);
		CU_ADD_TEST(suite, bs_load_custom_cluster_size);
		CU_ADD_TEST(suite, bs_load_after_failed_grow);
		CU_ADD_TEST(suite, bs_load_error);
		CU_ADD_TEST(suite_bs, bs_unload);
		CU_ADD_TEST(suite, bs_cluster_sz);
		CU_ADD_TEST(suite_bs, bs_usable_clusters);
		CU_ADD_TEST(suite, bs_resize_md);
		CU_ADD_TEST(suite, bs_destroy);
		CU_ADD_TEST(suite, bs_type);
		CU_ADD_TEST(suite, bs_super_block);
		CU_ADD_TEST(suite, bs_test_recover_cluster_count);
		CU_ADD_TEST(suite, bs_grow_live);
		CU_ADD_TEST(suite, bs_grow_live_no_space);
		CU_ADD_TEST(suite, bs_test_grow);
		CU_ADD_TEST(suite, blob_serialize_test);
		CU_ADD_TEST(suite_bs, blob_crc);
		CU_ADD_TEST(suite, super_block_crc);
		CU_ADD_TEST(suite_blob, blob_dirty_shutdown);
		CU_ADD_TEST(suite_bs, blob_flags);
		CU_ADD_TEST(suite_bs, bs_version);
		CU_ADD_TEST(suite_bs, blob_set_xattrs_test);
		CU_ADD_TEST(suite_bs, blob_thin_prov_alloc);
		CU_ADD_TEST(suite_bs, blob_insert_cluster_msg_test);
		CU_ADD_TEST(suite_bs, blob_thin_prov_rw);
		CU_ADD_TEST(suite, blob_thin_prov_write_count_io);
		CU_ADD_TEST(suite, blob_thin_prov_unmap_cluster);
		CU_ADD_TEST(suite_bs, blob_thin_prov_rle);
		CU_ADD_TEST(suite_bs, blob_thin_prov_rw_iov);
		CU_ADD_TEST(suite, bs_load_iter_test);
		CU_ADD_TEST(suite_bs, blob_snapshot_rw);
		CU_ADD_TEST(suite_bs, blob_snapshot_rw_iov);
		CU_ADD_TEST(suite, blob_relations);
		CU_ADD_TEST(suite, blob_relations2);
		CU_ADD_TEST(suite, blob_relations3);
		CU_ADD_TEST(suite, blobstore_clean_power_failure);
		CU_ADD_TEST(suite, blob_delete_snapshot_power_failure);
		CU_ADD_TEST(suite, blob_create_snapshot_power_failure);
		CU_ADD_TEST(suite_bs, blob_inflate_rw);
		CU_ADD_TEST(suite_bs, blob_inflate_unmap);
		CU_ADD_TEST(suite_bs, blob_snapshot_freeze_io);
		CU_ADD_TEST(suite_bs, blob_operation_split_rw);
		CU_ADD_TEST(suite_bs, blob_operation_split_rw_iov);
		CU_ADD_TEST(suite, blob_io_unit);
		CU_ADD_TEST(suite, blob_io_unit_compatibility);
		CU_ADD_TEST(suite_bs, blob_simultaneous_operations);
		CU_ADD_TEST(suite_bs, blob_persist_test);
		CU_ADD_TEST(suite_bs, blob_decouple_snapshot);
		CU_ADD_TEST(suite_bs, blob_seek_io_unit);
		CU_ADD_TEST(suite_esnap_bs, blob_esnap_create);
		CU_ADD_TEST(suite_bs, blob_nested_freezes);
		CU_ADD_TEST(suite, blob_ext_md_pages);
		CU_ADD_TEST(suite, blob_esnap_io_4096_4096);
		CU_ADD_TEST(suite, blob_esnap_io_512_512);
		CU_ADD_TEST(suite, blob_esnap_io_4096_512);
		CU_ADD_TEST(suite, blob_esnap_io_512_4096);
		CU_ADD_TEST(suite_esnap_bs, blob_esnap_thread_add_remove);
		CU_ADD_TEST(suite_esnap_bs, blob_esnap_clone_snapshot);
		CU_ADD_TEST(suite_esnap_bs, blob_esnap_clone_inflate);
		CU_ADD_TEST(suite_esnap_bs, blob_esnap_clone_decouple);
		CU_ADD_TEST(suite_esnap_bs, blob_esnap_clone_reload);
		CU_ADD_TEST(suite_esnap_bs, blob_esnap_hotplug);
		CU_ADD_TEST(suite_blob, blob_is_degraded);
		CU_ADD_TEST(suite_bs, blob_clone_resize);
		CU_ADD_TEST(suite, blob_esnap_clone_resize);
		CU_ADD_TEST(suite_bs, blob_shallow_copy);
		CU_ADD_TEST(suite_esnap_bs, blob_set_parent);
		CU_ADD_TEST(suite_esnap_bs, blob_set_external_parent);
	}

	allocate_threads(2);
	set_thread(0);

	g_dev_buffer = calloc(1, DEV_BUFFER_SIZE);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);

	free(g_dev_buffer);

	free_threads();

	return num_failures;
}
