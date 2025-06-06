/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "common/lib/ut_multithread.c"
#include "spdk_internal/cunit.h"
#include "spdk/nvmf.h"
#include "spdk_internal/mock.h"

#include "spdk/bdev_module.h"
#include "nvmf/subsystem.c"
#include "nvmf/transport.c"

SPDK_LOG_REGISTER_COMPONENT(nvmf)

DEFINE_STUB(spdk_bdev_module_claim_bdev,
	    int,
	    (struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
	     struct spdk_bdev_module *module), 0);

DEFINE_STUB_V(spdk_bdev_module_release_bdev,
	      (struct spdk_bdev *bdev));

DEFINE_STUB(spdk_bdev_desc_get_block_size, uint32_t,
	    (struct spdk_bdev_desc *desc), 512);

DEFINE_STUB(spdk_bdev_desc_get_md_size, uint32_t,
	    (struct spdk_bdev_desc *desc), 0);

DEFINE_STUB(spdk_bdev_desc_is_md_interleaved, bool,
	    (struct spdk_bdev_desc *desc), false);

DEFINE_STUB(spdk_bdev_io_type_supported, bool,
	    (struct spdk_bdev *bdev,
	     enum spdk_bdev_io_type io_type), false);

DEFINE_STUB_V(spdk_nvmf_send_discovery_log_notice,
	      (struct spdk_nvmf_tgt *tgt, const char *hostnqn));
DEFINE_STUB(spdk_nvmf_qpair_disconnect, int, (struct spdk_nvmf_qpair *qpair), 0);

DEFINE_STUB(spdk_nvmf_request_complete,
	    int,
	    (struct spdk_nvmf_request *req), 0);

DEFINE_STUB(nvmf_ctrlr_async_event_ana_change_notice,
	    int,
	    (struct spdk_nvmf_ctrlr *ctrlr), 0);

DEFINE_STUB(spdk_nvme_transport_id_trtype_str,
	    const char *,
	    (enum spdk_nvme_transport_type trtype), NULL);

DEFINE_STUB(spdk_bdev_is_zoned, bool,
	    (const struct spdk_bdev *bdev), false);

DEFINE_STUB(spdk_bdev_get_max_zone_append_size, uint32_t,
	    (const struct spdk_bdev *bdev), 0);

DEFINE_STUB(spdk_mempool_lookup, struct spdk_mempool *,
	    (const char *name), NULL);

DEFINE_STUB(spdk_nvme_transport_id_adrfam_str, const char *,
	    (enum spdk_nvmf_adrfam adrfam), NULL);

DEFINE_STUB(spdk_nvmf_qpair_get_listen_trid, int,
	    (struct spdk_nvmf_qpair *qpair,
	     struct spdk_nvme_transport_id *trid), 0);
DEFINE_STUB(spdk_key_dup, struct spdk_key *, (struct spdk_key *k), NULL);
DEFINE_STUB(spdk_key_get_name, const char *, (struct spdk_key *k), NULL);
DEFINE_STUB_V(spdk_keyring_put_key, (struct spdk_key *k));
DEFINE_STUB(nvmf_auth_is_supported, bool, (void), false);
DEFINE_STUB(nvmf_tgt_update_mdns_prr, int, (struct spdk_nvmf_tgt *tgt), 0);

DEFINE_STUB(spdk_bdev_get_module_name, const char *, (const struct spdk_bdev *bdev), "nvme");
DEFINE_STUB(spdk_bdev_get_module_ctx, void *, (struct spdk_bdev_desc *desc), NULL);
DEFINE_STUB(spdk_bdev_get_nvme_nsid, uint32_t, (struct spdk_bdev *bdev), 0);

static struct spdk_nvmf_transport g_transport = {};

struct spdk_nvmf_subsystem *
spdk_nvmf_tgt_find_subsystem(struct spdk_nvmf_tgt *tgt, const char *subnqn)
{
	return NULL;
}

struct spdk_nvmf_transport *
spdk_nvmf_tgt_get_transport(struct spdk_nvmf_tgt *tgt, const char *transport_name)
{
	if (strncmp(transport_name, SPDK_NVME_TRANSPORT_NAME_RDMA, SPDK_NVMF_TRSTRING_MAX_LEN)) {
		return &g_transport;
	}

	return NULL;
}

int
nvmf_poll_group_update_subsystem(struct spdk_nvmf_poll_group *group,
				 struct spdk_nvmf_subsystem *subsystem)
{
	return 0;
}

int
nvmf_poll_group_add_subsystem(struct spdk_nvmf_poll_group *group,
			      struct spdk_nvmf_subsystem *subsystem,
			      spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg)
{
	return 0;
}

void
nvmf_poll_group_remove_subsystem(struct spdk_nvmf_poll_group *group,
				 struct spdk_nvmf_subsystem *subsystem,
				 spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg)
{
}

void
nvmf_poll_group_pause_subsystem(struct spdk_nvmf_poll_group *group,
				struct spdk_nvmf_subsystem *subsystem,
				uint32_t nsid,
				spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg)
{
}

void
nvmf_poll_group_resume_subsystem(struct spdk_nvmf_poll_group *group,
				 struct spdk_nvmf_subsystem *subsystem,
				 spdk_nvmf_poll_group_mod_done cb_fn, void *cb_arg)
{
}

int
spdk_nvme_transport_id_parse_trtype(enum spdk_nvme_transport_type *trtype, const char *str)
{
	if (trtype == NULL || str == NULL) {
		return -EINVAL;
	}

	if (strcasecmp(str, "PCIe") == 0) {
		*trtype = SPDK_NVME_TRANSPORT_PCIE;
	} else if (strcasecmp(str, "RDMA") == 0) {
		*trtype = SPDK_NVME_TRANSPORT_RDMA;
	} else {
		return -ENOENT;
	}
	return 0;
}

int
spdk_nvme_transport_id_compare(const struct spdk_nvme_transport_id *trid1,
			       const struct spdk_nvme_transport_id *trid2)
{
	return 0;
}

int32_t
spdk_nvme_ctrlr_process_admin_completions(struct spdk_nvme_ctrlr *ctrlr)
{
	return -1;
}

int32_t
spdk_nvme_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	return -1;
}

int
spdk_nvme_detach(struct spdk_nvme_ctrlr *ctrlr)
{
	return -1;
}

void
nvmf_ctrlr_destruct(struct spdk_nvmf_ctrlr *ctrlr)
{
}

static struct spdk_nvmf_ctrlr *g_ns_changed_ctrlr = NULL;
static uint32_t g_ns_changed_nsid = 0;
void
nvmf_ctrlr_ns_changed(struct spdk_nvmf_ctrlr *ctrlr, uint32_t nsid)
{
	g_ns_changed_ctrlr = ctrlr;
	g_ns_changed_nsid = nsid;
}


static struct spdk_nvmf_ctrlr *g_async_event_ctrlr = NULL;
int
nvmf_ctrlr_async_event_ns_notice(struct spdk_nvmf_ctrlr *ctrlr)
{
	g_async_event_ctrlr = ctrlr;
	return 0;
}

static struct spdk_bdev g_bdevs[] = {
	{ .name = "bdev1" },
	{ .name = "bdev2" },
	{ .name = "bdev3", .ctratt.raw = 0x80000 },
};

struct spdk_bdev_desc {
	struct spdk_bdev	*bdev;
};

int
spdk_bdev_open_ext_v2(const char *bdev_name, bool write, spdk_bdev_event_cb_t event_cb,
		      void *event_ctx, struct spdk_bdev_open_opts *opts,
		      struct spdk_bdev_desc **_desc)
{
	struct spdk_bdev_desc *desc;
	size_t i;

	for (i = 0; i < sizeof(g_bdevs); i++) {
		if (strcmp(bdev_name, g_bdevs[i].name) == 0) {

			desc = calloc(1, sizeof(*desc));
			SPDK_CU_ASSERT_FATAL(desc != NULL);

			desc->bdev = &g_bdevs[i];
			*_desc = desc;
			return 0;
		}
	}

	return -EINVAL;
}

void
spdk_bdev_open_opts_init(struct spdk_bdev_open_opts *opts, size_t opts_size)
{
	memset(opts, 0, opts_size);
}

void
spdk_bdev_close(struct spdk_bdev_desc *desc)
{
	free(desc);
}

struct spdk_bdev *
spdk_bdev_desc_get_bdev(struct spdk_bdev_desc *desc)
{
	return desc->bdev;
}

const char *
spdk_bdev_get_name(const struct spdk_bdev *bdev)
{
	return "test";
}

const struct spdk_uuid *
spdk_bdev_get_uuid(const struct spdk_bdev *bdev)
{
	return &bdev->uuid;
}

union spdk_bdev_nvme_ctratt spdk_bdev_get_nvme_ctratt(struct spdk_bdev *bdev)
{
	return bdev->ctratt;
}

static void
test_spdk_nvmf_subsystem_add_ns(void)
{
	struct spdk_nvmf_tgt tgt = {};
	struct spdk_nvmf_subsystem subsystem = {
		.max_nsid = 1024,
		.ns = NULL,
		.tgt = &tgt,
	};
	struct spdk_nvmf_ns_opts ns_opts;
	uint32_t nsid;
	int rc;

	subsystem.ns = calloc(subsystem.max_nsid, sizeof(struct spdk_nvmf_subsystem_ns *));
	SPDK_CU_ASSERT_FATAL(subsystem.ns != NULL);
	subsystem.ana_group = calloc(subsystem.max_nsid, sizeof(uint32_t));
	SPDK_CU_ASSERT_FATAL(subsystem.ana_group != NULL);

	tgt.max_subsystems = 1024;
	RB_INIT(&tgt.subsystems);

	/* Request a specific NSID */
	spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));
	ns_opts.nsid = 5;
	nsid = spdk_nvmf_subsystem_add_ns_ext(&subsystem, "bdev2", &ns_opts, sizeof(ns_opts), NULL);
	CU_ASSERT(nsid == 5);
	CU_ASSERT(subsystem.max_nsid == 1024);
	SPDK_CU_ASSERT_FATAL(subsystem.ns[nsid - 1] != NULL);
	CU_ASSERT(subsystem.ns[nsid - 1]->bdev == &g_bdevs[1]);

	/* Request an NSID that is already in use */
	spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));
	ns_opts.nsid = 5;
	nsid = spdk_nvmf_subsystem_add_ns_ext(&subsystem, "bdev2", &ns_opts, sizeof(ns_opts), NULL);
	CU_ASSERT(nsid == 0);
	CU_ASSERT(subsystem.max_nsid == 1024);

	/* Request 0xFFFFFFFF (invalid NSID, reserved for broadcast) */
	spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));
	ns_opts.nsid = 0xFFFFFFFF;
	nsid = spdk_nvmf_subsystem_add_ns_ext(&subsystem, "bdev2", &ns_opts, sizeof(ns_opts), NULL);
	CU_ASSERT(nsid == 0);
	CU_ASSERT(subsystem.max_nsid == 1024);

	rc = spdk_nvmf_subsystem_remove_ns(&subsystem, 5);
	CU_ASSERT(rc == 0);

	free(subsystem.ns);
	free(subsystem.ana_group);
}

static void
test_spdk_nvmf_subsystem_add_fdp_ns(void)
{
	struct spdk_nvmf_tgt tgt = {};
	struct spdk_nvmf_subsystem subsystem = {
		.max_nsid = 1024,
		.ns = NULL,
		.tgt = &tgt,
	};
	struct spdk_nvmf_ns_opts ns_opts;
	uint32_t nsid;
	int rc;

	subsystem.ns = calloc(subsystem.max_nsid, sizeof(struct spdk_nvmf_subsystem_ns *));
	SPDK_CU_ASSERT_FATAL(subsystem.ns != NULL);
	subsystem.ana_group = calloc(subsystem.max_nsid, sizeof(uint32_t));
	SPDK_CU_ASSERT_FATAL(subsystem.ana_group != NULL);

	tgt.max_subsystems = 1024;
	RB_INIT(&tgt.subsystems);

	CU_ASSERT(subsystem.fdp_supported == false);

	/* Add a FDP supported namespace to the subsystem */
	spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));
	ns_opts.nsid = 3;
	nsid = spdk_nvmf_subsystem_add_ns_ext(&subsystem, "bdev3", &ns_opts, sizeof(ns_opts), NULL);
	CU_ASSERT(nsid == 3);
	CU_ASSERT(subsystem.max_nsid == 1024);
	SPDK_CU_ASSERT_FATAL(subsystem.ns[nsid - 1] != NULL);
	CU_ASSERT(subsystem.ns[nsid - 1]->bdev == &g_bdevs[2]);
	CU_ASSERT(subsystem.fdp_supported == true);

	/* Try to add a non FDP supported namespace to the subsystem */
	spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));
	ns_opts.nsid = 5;
	nsid = spdk_nvmf_subsystem_add_ns_ext(&subsystem, "bdev2", &ns_opts, sizeof(ns_opts), NULL);
	CU_ASSERT(nsid == 0);
	CU_ASSERT(subsystem.max_nsid == 1024);
	CU_ASSERT(subsystem.fdp_supported == true);

	/* Remove last FDP namespace from the subsystem */
	rc = spdk_nvmf_subsystem_remove_ns(&subsystem, 3);
	CU_ASSERT(rc == 0);
	CU_ASSERT(subsystem.fdp_supported == false);

	free(subsystem.ns);
	free(subsystem.ana_group);
}

static void
nvmf_test_create_subsystem(void)
{
	struct spdk_nvmf_tgt tgt = {};
	char nqn[256];
	struct spdk_nvmf_subsystem *subsystem;
	int rc;

	tgt.max_subsystems = 1024;
	tgt.subsystem_ids = spdk_bit_array_create(tgt.max_subsystems);
	RB_INIT(&tgt.subsystems);

	snprintf(nqn, sizeof(nqn), "nqn.2016-06.io.spdk:subsystem1");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);
	CU_ASSERT_STRING_EQUAL(subsystem->subnqn, nqn);
	rc = spdk_nvmf_subsystem_destroy(subsystem, NULL, NULL);
	CU_ASSERT(rc == 0);

	/* valid name with complex reverse domain */
	snprintf(nqn, sizeof(nqn), "nqn.2016-06.io.spdk-full--rev-domain.name:subsystem1");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);
	CU_ASSERT_STRING_EQUAL(subsystem->subnqn, nqn);
	rc = spdk_nvmf_subsystem_destroy(subsystem, NULL, NULL);
	CU_ASSERT(rc == 0);

	/* Valid name discovery controller */
	snprintf(nqn, sizeof(nqn), "nqn.2016-06.io.spdk:subsystem1");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);
	CU_ASSERT_STRING_EQUAL(subsystem->subnqn, nqn);
	rc = spdk_nvmf_subsystem_destroy(subsystem, NULL, NULL);
	CU_ASSERT(rc == 0);

	/* Invalid name, no user supplied string */
	snprintf(nqn, sizeof(nqn), "nqn.2016-06.io.spdk:");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem == NULL);

	/* Valid name, only contains top-level domain name */
	snprintf(nqn, sizeof(nqn), "nqn.2016-06.io.spdk:subsystem1");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);
	CU_ASSERT_STRING_EQUAL(subsystem->subnqn, nqn);
	rc = spdk_nvmf_subsystem_destroy(subsystem, NULL, NULL);
	CU_ASSERT(rc == 0);

	/* Invalid name, domain label > 63 characters */
	snprintf(nqn, sizeof(nqn),
		 "nqn.2016-06.io.abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz:sub");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem == NULL);

	/* Invalid name, domain label starts with digit */
	snprintf(nqn, sizeof(nqn), "nqn.2016-06.io.3spdk:sub");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem == NULL);

	/* Invalid name, domain label starts with - */
	snprintf(nqn, sizeof(nqn), "nqn.2016-06.io.-spdk:subsystem1");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem == NULL);

	/* Invalid name, domain label ends with - */
	snprintf(nqn, sizeof(nqn), "nqn.2016-06.io.spdk-:subsystem1");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem == NULL);

	/* Invalid name, domain label with multiple consecutive periods */
	snprintf(nqn, sizeof(nqn), "nqn.2016-06.io..spdk:subsystem1");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem == NULL);

	/* Longest valid name */
	snprintf(nqn, sizeof(nqn), "nqn.2016-06.io.spdk:");
	memset(nqn + strlen(nqn), 'a', 223 - strlen(nqn));
	nqn[223] = '\0';
	CU_ASSERT(strlen(nqn) == 223);
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);
	CU_ASSERT_STRING_EQUAL(subsystem->subnqn, nqn);
	rc = spdk_nvmf_subsystem_destroy(subsystem, NULL, NULL);
	CU_ASSERT(rc == 0);

	/* Invalid name, too long */
	snprintf(nqn, sizeof(nqn), "nqn.2016-06.io.spdk:");
	memset(nqn + strlen(nqn), 'a', 224 - strlen(nqn));
	nqn[224] = '\0';
	CU_ASSERT(strlen(nqn) == 224);
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	CU_ASSERT(subsystem == NULL);

	/* Valid name using uuid format */
	snprintf(nqn, sizeof(nqn), "nqn.2014-08.org.nvmexpress:uuid:ff9b6406-0fc8-4779-80ca-4dca14bda0d2");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);
	CU_ASSERT_STRING_EQUAL(subsystem->subnqn, nqn);
	rc = spdk_nvmf_subsystem_destroy(subsystem, NULL, NULL);
	CU_ASSERT(rc == 0);

	/* Invalid name user string contains an invalid utf-8 character */
	snprintf(nqn, sizeof(nqn), "nqn.2016-06.io.spdk:\xFFsubsystem1");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem == NULL);

	/* Valid name with non-ascii but valid utf-8 characters */
	snprintf(nqn, sizeof(nqn), "nqn.2016-06.io.spdk:\xe1\x8a\x88subsystem1\xca\x80");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);
	CU_ASSERT_STRING_EQUAL(subsystem->subnqn, nqn);
	rc = spdk_nvmf_subsystem_destroy(subsystem, NULL, NULL);
	CU_ASSERT(rc == 0);

	/* Invalid uuid (too long) */
	snprintf(nqn, sizeof(nqn),
		 "nqn.2014-08.org.nvmexpress:uuid:ff9b6406-0fc8-4779-80ca-4dca14bda0d2aaaa");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem == NULL);

	/* Invalid uuid (dashes placed incorrectly) */
	snprintf(nqn, sizeof(nqn), "nqn.2014-08.org.nvmexpress:uuid:ff9b64-060fc8-4779-80ca-4dca14bda0d2");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem == NULL);

	/* Invalid uuid (invalid characters in uuid) */
	snprintf(nqn, sizeof(nqn), "nqn.2014-08.org.nvmexpress:uuid:ff9hg406-0fc8-4779-80ca-4dca14bda0d2");
	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem == NULL);

	spdk_bit_array_free(&tgt.subsystem_ids);
}

static void
test_spdk_nvmf_subsystem_set_sn(void)
{
	struct spdk_nvmf_subsystem subsystem = {};

	/* Basic valid serial number */
	CU_ASSERT(spdk_nvmf_subsystem_set_sn(&subsystem, "abcd xyz") == 0);
	CU_ASSERT(strcmp(subsystem.sn, "abcd xyz") == 0);

	/* Exactly 20 characters (valid) */
	CU_ASSERT(spdk_nvmf_subsystem_set_sn(&subsystem, "12345678901234567890") == 0);
	CU_ASSERT(strcmp(subsystem.sn, "12345678901234567890") == 0);

	/* 21 characters (too long, invalid) */
	CU_ASSERT(spdk_nvmf_subsystem_set_sn(&subsystem, "123456789012345678901") < 0);

	/* Non-ASCII characters (invalid) */
	CU_ASSERT(spdk_nvmf_subsystem_set_sn(&subsystem, "abcd\txyz") < 0);
}

static void
test_spdk_nvmf_ns_visible(void)
{
	struct spdk_nvmf_subsystem subsystem = {};
	struct spdk_nvmf_ns ns1 = {
		.nsid = 1,
		.anagrpid = 1,
		.always_visible = false
	};
	struct spdk_nvmf_ns ns2 = {
		.nsid = 2,
		.anagrpid = 2,
		.always_visible = false
	};
	struct spdk_nvmf_ns *ns3;
	struct spdk_nvmf_ctrlr ctrlrA = {
		.subsys = &subsystem
	};
	struct spdk_nvmf_ctrlr ctrlrB = {
		.subsys = &subsystem
	};
	struct spdk_thread *thread;
	struct spdk_nvmf_tgt tgt = {};
	uint32_t nsid;
	int rc;

	thread = spdk_get_thread();
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	ctrlrA.thread = thread;
	ctrlrB.thread = thread;

	subsystem.max_nsid = 1024;
	subsystem.ns = calloc(subsystem.max_nsid, sizeof(subsystem.ns));
	SPDK_CU_ASSERT_FATAL(subsystem.ns != NULL);
	subsystem.ana_group = calloc(subsystem.max_nsid, sizeof(uint32_t));
	SPDK_CU_ASSERT_FATAL(subsystem.ana_group != NULL);
	TAILQ_INIT(&tgt.transports);
	subsystem.tgt = &tgt;

	subsystem.ns[1] = &ns1;
	subsystem.ns[2] = &ns2;
	ns3 = calloc(1, sizeof(*ns3));
	SPDK_CU_ASSERT_FATAL(ns3 != NULL);
	ns3->nsid = 3;
	ns3->anagrpid = 3;
	subsystem.ana_group[ns3->anagrpid - 1] = 1;
	subsystem.ns[3] = ns3;

	snprintf(ctrlrA.hostnqn, sizeof(ctrlrA.hostnqn), "nqn.2016-06.io.spdk:host1");
	ctrlrA.visible_ns = spdk_bit_array_create(subsystem.max_nsid);
	SPDK_CU_ASSERT_FATAL(ctrlrA.visible_ns != NULL);
	snprintf(ctrlrB.hostnqn, sizeof(ctrlrB.hostnqn), "nqn.2016-06.io.spdk:host2");
	ctrlrB.visible_ns = spdk_bit_array_create(subsystem.max_nsid);
	SPDK_CU_ASSERT_FATAL(ctrlrB.visible_ns != NULL);

	/* Add two controllers ctrlrA and ctrlrB */
	TAILQ_INIT(&subsystem.ctrlrs);
	TAILQ_INSERT_TAIL(&subsystem.ctrlrs, &ctrlrA, link);
	TAILQ_INSERT_TAIL(&subsystem.ctrlrs, &ctrlrB, link);

	/* Invalid host nqn */
	nsid = 1;
	rc = spdk_nvmf_ns_add_host(&subsystem, nsid, "", 0);
	CU_ASSERT(rc == -EINVAL);
	rc = spdk_nvmf_ns_add_host(&subsystem, nsid, NULL, 0);
	CU_ASSERT(rc == -EINVAL);
	rc = spdk_nvmf_ns_remove_host(&subsystem, nsid, NULL, 0);
	CU_ASSERT(rc == -EINVAL);

	/* Invalid nsid */
	nsid = 0;
	rc = spdk_nvmf_ns_add_host(&subsystem, nsid, ctrlrA.hostnqn, 0);
	CU_ASSERT(rc == -EINVAL);
	rc = spdk_nvmf_ns_remove_host(&subsystem, nsid, ctrlrA.hostnqn, 0);
	CU_ASSERT(rc == -EINVAL);

	/* Unallocated ns */
	nsid = 1;
	rc = spdk_nvmf_ns_add_host(&subsystem, nsid, ctrlrA.hostnqn, 0);
	CU_ASSERT(rc == -ENOENT);
	rc = spdk_nvmf_ns_remove_host(&subsystem, nsid, ctrlrA.hostnqn, 0);
	CU_ASSERT(rc == -ENOENT);

	/* Attach any is active => do not allow individual host control */
	ns1.always_visible = true;
	nsid = 2;
	rc = spdk_nvmf_ns_add_host(&subsystem, nsid, ctrlrA.hostnqn, 0);
	CU_ASSERT(rc == -EPERM);
	rc = spdk_nvmf_ns_remove_host(&subsystem, nsid, ctrlrA.hostnqn, 0);
	CU_ASSERT(rc == -EPERM);
	ns1.always_visible = false;

	/* Attach ctrlrA to namespace 2 hot + cold */
	nsid = 2;
	g_async_event_ctrlr = NULL;
	g_ns_changed_ctrlr = NULL;
	g_ns_changed_nsid = 0;
	rc = spdk_nvmf_ns_add_host(&subsystem, nsid, ctrlrA.hostnqn, 0);
	CU_ASSERT(rc == 0);
	CU_ASSERT(nvmf_ns_find_host(&ns1, ctrlrA.hostnqn) != NULL);
	CU_ASSERT(nvmf_ns_find_host(&ns1, ctrlrB.hostnqn) == NULL);
	CU_ASSERT(nvmf_ns_find_host(&ns2, ctrlrA.hostnqn) == NULL);
	CU_ASSERT(nvmf_ns_find_host(&ns2, ctrlrB.hostnqn) == NULL);
	CU_ASSERT(nvmf_ctrlr_ns_is_visible(&ctrlrA, nsid));
	CU_ASSERT(!nvmf_ctrlr_ns_is_visible(&ctrlrB, nsid));
	CU_ASSERT(!nvmf_ctrlr_ns_is_visible(&ctrlrA, nsid + 1));
	CU_ASSERT(!nvmf_ctrlr_ns_is_visible(&ctrlrB, nsid + 1));
	/* check last ns_changed */
	CU_ASSERT(g_ns_changed_ctrlr == &ctrlrA);
	CU_ASSERT(g_ns_changed_nsid == nsid);
	/* check async_event */
	poll_threads();
	CU_ASSERT(g_async_event_ctrlr == &ctrlrA);

	/* Attach ctrlrA to namespace 2 again => should not create any ns change/async event */
	g_async_event_ctrlr = NULL;
	g_ns_changed_ctrlr = NULL;
	g_ns_changed_nsid = 0;
	rc = spdk_nvmf_ns_add_host(&subsystem, nsid, ctrlrA.hostnqn, 0);
	CU_ASSERT(rc == 0);
	CU_ASSERT(nvmf_ns_find_host(&ns1, ctrlrA.hostnqn) != NULL);
	CU_ASSERT(nvmf_ns_find_host(&ns1, ctrlrB.hostnqn) == NULL);
	CU_ASSERT(nvmf_ns_find_host(&ns2, ctrlrA.hostnqn) == NULL);
	CU_ASSERT(nvmf_ns_find_host(&ns2, ctrlrB.hostnqn) == NULL);
	CU_ASSERT(nvmf_ctrlr_ns_is_visible(&ctrlrA, nsid));
	CU_ASSERT(!nvmf_ctrlr_ns_is_visible(&ctrlrB, nsid));
	CU_ASSERT(!nvmf_ctrlr_ns_is_visible(&ctrlrA, nsid + 1));
	CU_ASSERT(!nvmf_ctrlr_ns_is_visible(&ctrlrB, nsid + 1));
	/* check last ns_changed */
	CU_ASSERT(g_ns_changed_ctrlr == NULL);
	CU_ASSERT(g_ns_changed_nsid == 0);
	/* check async_event */
	poll_threads();
	CU_ASSERT(g_async_event_ctrlr == NULL);

	/* Detach ctrlrA from namespace 2 hot + cold */
	g_async_event_ctrlr = NULL;
	g_ns_changed_ctrlr = NULL;
	g_ns_changed_nsid = 0;
	rc = spdk_nvmf_ns_remove_host(&subsystem, nsid, ctrlrA.hostnqn, 0);
	CU_ASSERT(rc == 0);
	CU_ASSERT(nvmf_ns_find_host(&ns1, ctrlrA.hostnqn) == NULL);
	CU_ASSERT(nvmf_ns_find_host(&ns1, ctrlrB.hostnqn) == NULL);
	CU_ASSERT(nvmf_ns_find_host(&ns2, ctrlrA.hostnqn) == NULL);
	CU_ASSERT(nvmf_ns_find_host(&ns2, ctrlrB.hostnqn) == NULL);
	CU_ASSERT(!nvmf_ctrlr_ns_is_visible(&ctrlrA, nsid));
	CU_ASSERT(!nvmf_ctrlr_ns_is_visible(&ctrlrB, nsid));
	CU_ASSERT(!nvmf_ctrlr_ns_is_visible(&ctrlrA, nsid + 1));
	CU_ASSERT(!nvmf_ctrlr_ns_is_visible(&ctrlrB, nsid + 1));
	/* check last ns_changed */
	CU_ASSERT(g_ns_changed_ctrlr == &ctrlrA);
	CU_ASSERT(g_ns_changed_nsid == nsid);
	/* check async_event */
	poll_threads();
	CU_ASSERT(g_async_event_ctrlr == &ctrlrA);

	/* Detach ctrlrA from namespace 2 again hot + cold */
	g_async_event_ctrlr = NULL;
	g_ns_changed_ctrlr = NULL;
	g_ns_changed_nsid = 0;
	rc = spdk_nvmf_ns_remove_host(&subsystem, nsid, ctrlrA.hostnqn, 0);
	CU_ASSERT(rc == 0);
	CU_ASSERT(nvmf_ns_find_host(&ns1, ctrlrA.hostnqn) == NULL);
	CU_ASSERT(nvmf_ns_find_host(&ns1, ctrlrB.hostnqn) == NULL);
	CU_ASSERT(nvmf_ns_find_host(&ns2, ctrlrA.hostnqn) == NULL);
	CU_ASSERT(nvmf_ns_find_host(&ns2, ctrlrB.hostnqn) == NULL);
	CU_ASSERT(!nvmf_ctrlr_ns_is_visible(&ctrlrA, nsid));
	CU_ASSERT(!nvmf_ctrlr_ns_is_visible(&ctrlrB, nsid));
	CU_ASSERT(!nvmf_ctrlr_ns_is_visible(&ctrlrA, nsid + 1));
	CU_ASSERT(!nvmf_ctrlr_ns_is_visible(&ctrlrB, nsid + 1));
	/* check last ns_changed */
	CU_ASSERT(g_ns_changed_ctrlr == NULL);
	CU_ASSERT(g_ns_changed_nsid == 0);
	/* check async_event */
	poll_threads();
	CU_ASSERT(g_async_event_ctrlr == NULL);

	/* Attach ctrlrA to namespace 4 hot + cold => remove ns */
	nsid = 4;
	g_async_event_ctrlr = NULL;
	g_ns_changed_ctrlr = NULL;
	g_ns_changed_nsid = 0;
	rc = spdk_nvmf_ns_add_host(&subsystem, nsid, ctrlrA.hostnqn, 0);
	CU_ASSERT(rc == 0);
	CU_ASSERT(nvmf_ns_find_host(ns3, ctrlrA.hostnqn) != NULL);
	CU_ASSERT(nvmf_ns_find_host(ns3, ctrlrB.hostnqn) == NULL);
	CU_ASSERT(nvmf_ctrlr_ns_is_visible(&ctrlrA, nsid));
	CU_ASSERT(!nvmf_ctrlr_ns_is_visible(&ctrlrB, nsid));
	/* check last ns_changed */
	CU_ASSERT(g_ns_changed_ctrlr == &ctrlrA);
	CU_ASSERT(g_ns_changed_nsid == nsid);
	/* check async_event */
	poll_threads();
	CU_ASSERT(g_async_event_ctrlr == &ctrlrA);

	g_async_event_ctrlr = NULL;
	g_ns_changed_ctrlr = NULL;
	g_ns_changed_nsid = 0;
	rc = spdk_nvmf_subsystem_remove_ns(&subsystem, nsid);
	CU_ASSERT(rc == 0);
	CU_ASSERT(!nvmf_ctrlr_ns_is_visible(&ctrlrA, nsid));
	CU_ASSERT(!nvmf_ctrlr_ns_is_visible(&ctrlrB, nsid));
	/* check last ns_changed */
	CU_ASSERT(g_ns_changed_ctrlr == &ctrlrA);
	CU_ASSERT(g_ns_changed_nsid == nsid);

	free(ctrlrA.visible_ns);
	free(ctrlrB.visible_ns);
	free(subsystem.ana_group);
	free(subsystem.ns);
}

/*
 * Reservation Unit Test Configuration
 *       --------             --------    --------
 *      | Host A |           | Host B |  | Host C |
 *       --------             --------    --------
 *      /        \               |           |
 *  --------   --------       -------     -------
 * |Ctrlr1_A| |Ctrlr2_A|     |Ctrlr_B|   |Ctrlr_C|
 *  --------   --------       -------     -------
 *    \           \              /           /
 *     \           \            /           /
 *      \           \          /           /
 *      --------------------------------------
 *     |            NAMESPACE 1               |
 *      --------------------------------------
 */
static struct spdk_nvmf_subsystem g_subsystem;
static struct spdk_nvmf_ctrlr g_ctrlr1_A, g_ctrlr2_A, g_ctrlr_B, g_ctrlr_C;
static struct spdk_nvmf_ns g_ns;
struct spdk_nvmf_subsystem_pg_ns_info g_ns_info;

void
nvmf_ctrlr_async_event_reservation_notification(struct spdk_nvmf_ctrlr *ctrlr)
{
}

static void
ut_reservation_init(void)
{

	TAILQ_INIT(&g_subsystem.ctrlrs);

	memset(&g_ns, 0, sizeof(g_ns));
	TAILQ_INIT(&g_ns.registrants);
	g_ns.subsystem = &g_subsystem;
	g_ns.ptpl_file = NULL;
	g_ns.ptpl_activated = false;
	spdk_uuid_generate(&g_bdevs[0].uuid);
	g_ns.bdev = &g_bdevs[0];

	/* Host A has two controllers */
	spdk_uuid_generate(&g_ctrlr1_A.hostid);
	TAILQ_INIT(&g_ctrlr1_A.log_head);
	g_ctrlr1_A.subsys = &g_subsystem;
	g_ctrlr1_A.num_avail_log_pages = 0;
	TAILQ_INSERT_TAIL(&g_subsystem.ctrlrs, &g_ctrlr1_A, link);
	spdk_uuid_copy(&g_ctrlr2_A.hostid, &g_ctrlr1_A.hostid);
	TAILQ_INIT(&g_ctrlr2_A.log_head);
	g_ctrlr2_A.subsys = &g_subsystem;
	g_ctrlr2_A.num_avail_log_pages = 0;
	TAILQ_INSERT_TAIL(&g_subsystem.ctrlrs, &g_ctrlr2_A, link);

	/* Host B has 1 controller */
	spdk_uuid_generate(&g_ctrlr_B.hostid);
	TAILQ_INIT(&g_ctrlr_B.log_head);
	g_ctrlr_B.subsys = &g_subsystem;
	g_ctrlr_B.num_avail_log_pages = 0;
	TAILQ_INSERT_TAIL(&g_subsystem.ctrlrs, &g_ctrlr_B, link);

	/* Host C has 1 controller */
	spdk_uuid_generate(&g_ctrlr_C.hostid);
	TAILQ_INIT(&g_ctrlr_C.log_head);
	g_ctrlr_C.subsys = &g_subsystem;
	g_ctrlr_C.num_avail_log_pages = 0;
	TAILQ_INSERT_TAIL(&g_subsystem.ctrlrs, &g_ctrlr_C, link);
}

static void
ut_reservation_deinit(void)
{
	struct spdk_nvmf_registrant *reg, *tmp;
	struct spdk_nvmf_reservation_log *log, *log_tmp;
	struct spdk_nvmf_ctrlr *ctrlr, *ctrlr_tmp;

	TAILQ_FOREACH_SAFE(reg, &g_ns.registrants, link, tmp) {
		TAILQ_REMOVE(&g_ns.registrants, reg, link);
		free(reg);
	}
	TAILQ_FOREACH_SAFE(log, &g_ctrlr1_A.log_head, link, log_tmp) {
		TAILQ_REMOVE(&g_ctrlr1_A.log_head, log, link);
		free(log);
	}
	g_ctrlr1_A.num_avail_log_pages = 0;
	TAILQ_FOREACH_SAFE(log, &g_ctrlr2_A.log_head, link, log_tmp) {
		TAILQ_REMOVE(&g_ctrlr2_A.log_head, log, link);
		free(log);
	}
	g_ctrlr2_A.num_avail_log_pages = 0;
	TAILQ_FOREACH_SAFE(log, &g_ctrlr_B.log_head, link, log_tmp) {
		TAILQ_REMOVE(&g_ctrlr_B.log_head, log, link);
		free(log);
	}
	g_ctrlr_B.num_avail_log_pages = 0;
	TAILQ_FOREACH_SAFE(log, &g_ctrlr_C.log_head, link, log_tmp) {
		TAILQ_REMOVE(&g_ctrlr_C.log_head, log, link);
		free(log);
	}
	g_ctrlr_C.num_avail_log_pages = 0;

	TAILQ_FOREACH_SAFE(ctrlr, &g_subsystem.ctrlrs, link, ctrlr_tmp) {
		TAILQ_REMOVE(&g_subsystem.ctrlrs, ctrlr, link);
	}
}

static struct spdk_nvmf_request *
ut_reservation_build_req(uint32_t length)
{
	struct spdk_nvmf_request *req;

	req = calloc(1, sizeof(*req));
	assert(req != NULL);

	SPDK_IOV_ONE(req->iov, &req->iovcnt, calloc(1, length), length);
	assert(req->iov[0].iov_base != NULL);
	req->length = length;

	req->cmd = (union nvmf_h2c_msg *)calloc(1, sizeof(union nvmf_h2c_msg));
	assert(req->cmd != NULL);

	req->rsp = (union nvmf_c2h_msg *)calloc(1, sizeof(union nvmf_c2h_msg));
	assert(req->rsp != NULL);

	return req;
}

static void
ut_reservation_free_req(struct spdk_nvmf_request *req)
{
	free(req->cmd);
	free(req->rsp);
	free(req->iov[0].iov_base);
	free(req);
}

static void
ut_reservation_build_register_request(struct spdk_nvmf_request *req,
				      uint8_t rrega, uint8_t iekey,
				      uint8_t cptpl, uint64_t crkey,
				      uint64_t nrkey)
{
	struct spdk_nvme_reservation_register_data key;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	key.crkey = crkey;
	key.nrkey = nrkey;
	cmd->cdw10 = 0;
	cmd->cdw10_bits.resv_register.rrega = rrega;
	cmd->cdw10_bits.resv_register.iekey = iekey;
	cmd->cdw10_bits.resv_register.cptpl = cptpl;
	memcpy(req->iov[0].iov_base, &key, sizeof(key));
}

static void
ut_reservation_build_acquire_request(struct spdk_nvmf_request *req,
				     uint8_t racqa, uint8_t iekey,
				     uint8_t rtype, uint64_t crkey,
				     uint64_t prkey)
{
	struct spdk_nvme_reservation_acquire_data key;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	key.crkey = crkey;
	key.prkey = prkey;
	cmd->cdw10 = 0;
	cmd->cdw10_bits.resv_acquire.racqa = racqa;
	cmd->cdw10_bits.resv_acquire.iekey = iekey;
	cmd->cdw10_bits.resv_acquire.rtype = rtype;
	memcpy(req->iov[0].iov_base, &key, sizeof(key));
}

static void
ut_reservation_build_release_request(struct spdk_nvmf_request *req,
				     uint8_t rrela, uint8_t iekey,
				     uint8_t rtype, uint64_t crkey)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	cmd->cdw10 = 0;
	cmd->cdw10_bits.resv_release.rrela = rrela;
	cmd->cdw10_bits.resv_release.iekey = iekey;
	cmd->cdw10_bits.resv_release.rtype = rtype;
	memcpy(req->iov[0].iov_base, &crkey, sizeof(crkey));
}

/*
 * Construct four registrants for other test cases.
 *
 * g_ctrlr1_A register with key 0xa1.
 * g_ctrlr2_A register with key 0xa1.
 * g_ctrlr_B register with key 0xb1.
 * g_ctrlr_C register with key 0xc1.
 * */
static void
ut_reservation_build_registrants(void)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;
	struct spdk_nvmf_registrant *reg;
	uint32_t gen;

	req = ut_reservation_build_req(16);
	rsp = &req->rsp->nvme_cpl;
	SPDK_CU_ASSERT_FATAL(req != NULL);
	gen = g_ns.gen;

	/* TEST CASE: g_ctrlr1_A register with a new key */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_REGISTER_KEY,
					      0, 0, 0, 0xa1);
	nvmf_ns_reservation_register(&g_ns, &g_ctrlr1_A, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr1_A.hostid);
	SPDK_CU_ASSERT_FATAL(reg->rkey == 0xa1);
	SPDK_CU_ASSERT_FATAL(g_ns.gen == gen + 1);

	/* TEST CASE: g_ctrlr2_A register with a new key, because it has same
	 * Host Identifier with g_ctrlr1_A, so the register key should same.
	 */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_REGISTER_KEY,
					      0, 0, 0, 0xa2);
	nvmf_ns_reservation_register(&g_ns, &g_ctrlr2_A, req);
	/* Reservation conflict for other key than 0xa1 */
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_RESERVATION_CONFLICT);

	/* g_ctrlr_B register with a new key */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_REGISTER_KEY,
					      0, 0, 0, 0xb1);
	nvmf_ns_reservation_register(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr_B.hostid);
	SPDK_CU_ASSERT_FATAL(reg->rkey == 0xb1);
	SPDK_CU_ASSERT_FATAL(g_ns.gen == gen + 2);

	/* g_ctrlr_C register with a new key */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_REGISTER_KEY,
					      0, 0, 0, 0xc1);
	nvmf_ns_reservation_register(&g_ns, &g_ctrlr_C, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr_C.hostid);
	SPDK_CU_ASSERT_FATAL(reg->rkey == 0xc1);
	SPDK_CU_ASSERT_FATAL(g_ns.gen == gen + 3);

	ut_reservation_free_req(req);
}

static void
test_reservation_register(void)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;
	struct spdk_nvmf_registrant *reg;
	uint32_t gen;

	ut_reservation_init();

	req = ut_reservation_build_req(16);
	rsp = &req->rsp->nvme_cpl;
	SPDK_CU_ASSERT_FATAL(req != NULL);

	ut_reservation_build_registrants();

	/* TEST CASE: Replace g_ctrlr1_A with a new key */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_REPLACE_KEY,
					      0, 0, 0xa1, 0xa11);
	nvmf_ns_reservation_register(&g_ns, &g_ctrlr1_A, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr1_A.hostid);
	SPDK_CU_ASSERT_FATAL(reg->rkey == 0xa11);

	/* TEST CASE: Host A with g_ctrlr1_A get reservation with
	 * type SPDK_NVME_RESERVE_WRITE_EXCLUSIVE
	 */
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_ACQUIRE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE, 0xa11, 0x0);
	gen = g_ns.gen;
	nvmf_ns_reservation_acquire(&g_ns, &g_ctrlr1_A, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr1_A.hostid);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE);
	SPDK_CU_ASSERT_FATAL(g_ns.crkey == 0xa11);
	SPDK_CU_ASSERT_FATAL(g_ns.holder == reg);
	SPDK_CU_ASSERT_FATAL(g_ns.gen == gen);

	/* TEST CASE: g_ctrlr_C unregister with IEKEY enabled */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_UNREGISTER_KEY,
					      1, 0, 0, 0);
	nvmf_ns_reservation_register(&g_ns, &g_ctrlr_C, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr_C.hostid);
	SPDK_CU_ASSERT_FATAL(reg == NULL);

	/* TEST CASE: g_ctrlr_B unregister with correct key */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_UNREGISTER_KEY,
					      0, 0, 0xb1, 0);
	nvmf_ns_reservation_register(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr_B.hostid);
	SPDK_CU_ASSERT_FATAL(reg == NULL);

	/* TEST CASE: No registrant now, g_ctrlr_B replace new key with IEKEY disabled */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_REPLACE_KEY,
					      0, 0, 0, 0xb1);
	nvmf_ns_reservation_register(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc != SPDK_NVME_SC_SUCCESS);

	/* TEST CASE: No registrant now, g_ctrlr_B replace new key with IEKEY enabled */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_REPLACE_KEY,
					      1, 0, 0, 0xb1);
	nvmf_ns_reservation_register(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr_B.hostid);
	SPDK_CU_ASSERT_FATAL(reg != NULL);

	/* TEST CASE: g_ctrlr_B replace new key with IEKEY enabled and wrong crkey  */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_REPLACE_KEY,
					      1, 0, 0xff, 0xb2);
	nvmf_ns_reservation_register(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr_B.hostid);
	SPDK_CU_ASSERT_FATAL(reg != NULL);
	SPDK_CU_ASSERT_FATAL(reg->rkey == 0xb2);

	/* TEST CASE: g_ctrlr1_A unregister with correct key,
	 * reservation should be removed as well.
	 */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_UNREGISTER_KEY,
					      0, 0, 0xa11, 0);
	nvmf_ns_reservation_register(&g_ns, &g_ctrlr1_A, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr1_A.hostid);
	SPDK_CU_ASSERT_FATAL(reg == NULL);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == 0);
	SPDK_CU_ASSERT_FATAL(g_ns.crkey == 0);
	SPDK_CU_ASSERT_FATAL(g_ns.holder == NULL);

	ut_reservation_free_req(req);
	ut_reservation_deinit();
}

static void
test_reservation_register_with_ptpl(void)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;
	struct spdk_nvmf_registrant *reg;
	bool update_sgroup = false;
	int rc;
	struct spdk_nvmf_reservation_info info;

	ut_reservation_init();

	req = ut_reservation_build_req(16);
	rsp = &req->rsp->nvme_cpl;
	SPDK_CU_ASSERT_FATAL(req != NULL);

	/* TEST CASE: No persistent file, register with PTPL enabled will fail */
	g_ns.ptpl_file = NULL;
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_REGISTER_KEY, 0,
					      SPDK_NVME_RESERVE_PTPL_PERSIST_POWER_LOSS, 0, 0xa1);
	update_sgroup = nvmf_ns_reservation_register(&g_ns, &g_ctrlr1_A, req);
	SPDK_CU_ASSERT_FATAL(update_sgroup == false);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc != SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr1_A.hostid);
	SPDK_CU_ASSERT_FATAL(reg == NULL);

	/* TEST CASE: Enable PTPL */
	g_ns.ptpl_file = "/tmp/Ns1PR.cfg";
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_REGISTER_KEY, 0,
					      SPDK_NVME_RESERVE_PTPL_PERSIST_POWER_LOSS, 0, 0xa1);
	update_sgroup = nvmf_ns_reservation_register(&g_ns, &g_ctrlr1_A, req);
	SPDK_CU_ASSERT_FATAL(update_sgroup == true);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.ptpl_activated == true);
	rc = nvmf_ns_update_reservation_info(&g_ns);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr1_A.hostid);
	SPDK_CU_ASSERT_FATAL(reg != NULL);
	SPDK_CU_ASSERT_FATAL(!spdk_uuid_compare(&g_ctrlr1_A.hostid, &reg->hostid));
	/* Load reservation information from configuration file */
	memset(&info, 0, sizeof(info));
	rc = nvmf_ns_reservation_load(&g_ns, &info);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(info.ptpl_activated == true);

	/* TEST CASE: Disable PTPL */
	rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_REGISTER_KEY, 0,
					      SPDK_NVME_RESERVE_PTPL_CLEAR_POWER_ON, 0, 0xa1);
	update_sgroup = nvmf_ns_reservation_register(&g_ns, &g_ctrlr1_A, req);
	SPDK_CU_ASSERT_FATAL(update_sgroup == true);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.ptpl_activated == false);
	rc = nvmf_ns_update_reservation_info(&g_ns);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	rc = nvmf_ns_reservation_load(&g_ns, &info);
	SPDK_CU_ASSERT_FATAL(rc < 0);
	unlink(g_ns.ptpl_file);

	ut_reservation_free_req(req);
	ut_reservation_deinit();
}

static void
test_reservation_acquire_preempt_1(void)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;
	struct spdk_nvmf_registrant *reg;
	uint32_t gen;

	ut_reservation_init();

	req = ut_reservation_build_req(16);
	rsp = &req->rsp->nvme_cpl;
	SPDK_CU_ASSERT_FATAL(req != NULL);

	ut_reservation_build_registrants();

	gen = g_ns.gen;
	/* ACQUIRE: Host A with g_ctrlr1_A acquire reservation with
	 * type SPDK_NVME_RESERVE_WRITE_EXCLUSIVE.
	 */
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_ACQUIRE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY, 0xa1, 0x0);
	nvmf_ns_reservation_acquire(&g_ns, &g_ctrlr1_A, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr1_A.hostid);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY);
	SPDK_CU_ASSERT_FATAL(g_ns.crkey == 0xa1);
	SPDK_CU_ASSERT_FATAL(g_ns.holder == reg);
	SPDK_CU_ASSERT_FATAL(g_ns.gen == gen);

	/* TEST CASE: g_ctrlr1_A holds the reservation, g_ctrlr_B preempt g_ctrl1_A,
	 * g_ctrl1_A registrant is unregistered.
	 */
	gen = g_ns.gen;
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_PREEMPT, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS, 0xb1, 0xa1);
	nvmf_ns_reservation_acquire(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr1_A.hostid);
	SPDK_CU_ASSERT_FATAL(reg == NULL);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr_B.hostid);
	SPDK_CU_ASSERT_FATAL(reg != NULL);
	SPDK_CU_ASSERT_FATAL(g_ns.holder == reg);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr_C.hostid);
	SPDK_CU_ASSERT_FATAL(reg != NULL);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS);
	SPDK_CU_ASSERT_FATAL(g_ns.gen > gen);

	/* TEST CASE: g_ctrlr_B holds the reservation, g_ctrlr_C preempt g_ctrlr_B
	 * with valid key and PRKEY set to 0, all registrants other the host that issued
	 * the command are unregistered.
	 */
	gen = g_ns.gen;
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_PREEMPT, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS, 0xc1, 0x0);
	nvmf_ns_reservation_acquire(&g_ns, &g_ctrlr_C, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr2_A.hostid);
	SPDK_CU_ASSERT_FATAL(reg == NULL);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr_B.hostid);
	SPDK_CU_ASSERT_FATAL(reg == NULL);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr_C.hostid);
	SPDK_CU_ASSERT_FATAL(reg != NULL);
	SPDK_CU_ASSERT_FATAL(g_ns.holder == reg);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS);
	SPDK_CU_ASSERT_FATAL(g_ns.gen > gen);

	ut_reservation_free_req(req);
	ut_reservation_deinit();
}

static void
test_reservation_acquire_release_with_ptpl(void)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;
	struct spdk_nvmf_registrant *reg;
	bool update_sgroup = false;
	struct spdk_uuid holder_uuid;
	int rc;
	struct spdk_nvmf_reservation_info info;

	ut_reservation_init();

	req = ut_reservation_build_req(16);
	rsp = &req->rsp->nvme_cpl;
	SPDK_CU_ASSERT_FATAL(req != NULL);

	/* TEST CASE: Enable PTPL */
	g_ns.ptpl_file = "/tmp/Ns1PR.cfg";
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_REGISTER_KEY, 0,
					      SPDK_NVME_RESERVE_PTPL_PERSIST_POWER_LOSS, 0, 0xa1);
	update_sgroup = nvmf_ns_reservation_register(&g_ns, &g_ctrlr1_A, req);
	SPDK_CU_ASSERT_FATAL(update_sgroup == true);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.ptpl_activated == true);
	rc = nvmf_ns_update_reservation_info(&g_ns);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr1_A.hostid);
	SPDK_CU_ASSERT_FATAL(reg != NULL);
	SPDK_CU_ASSERT_FATAL(!spdk_uuid_compare(&g_ctrlr1_A.hostid, &reg->hostid));
	/* Load reservation information from configuration file */
	memset(&info, 0, sizeof(info));
	rc = nvmf_ns_reservation_load(&g_ns, &info);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(info.ptpl_activated == true);

	/* TEST CASE: Acquire the reservation */
	rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_ACQUIRE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY, 0xa1, 0x0);
	update_sgroup = nvmf_ns_reservation_acquire(&g_ns, &g_ctrlr1_A, req);
	SPDK_CU_ASSERT_FATAL(update_sgroup == true);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	rc = nvmf_ns_update_reservation_info(&g_ns);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	memset(&info, 0, sizeof(info));
	rc = nvmf_ns_reservation_load(&g_ns, &info);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(info.ptpl_activated == true);
	SPDK_CU_ASSERT_FATAL(info.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY);
	SPDK_CU_ASSERT_FATAL(info.crkey == 0xa1);
	spdk_uuid_parse(&holder_uuid, info.holder_uuid);
	SPDK_CU_ASSERT_FATAL(!spdk_uuid_compare(&g_ctrlr1_A.hostid, &holder_uuid));

	/* TEST CASE: Release the reservation */
	rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
	ut_reservation_build_release_request(req, SPDK_NVME_RESERVE_RELEASE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY, 0xa1);
	update_sgroup = nvmf_ns_reservation_release(&g_ns, &g_ctrlr1_A, req);
	SPDK_CU_ASSERT_FATAL(update_sgroup == true);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	rc = nvmf_ns_update_reservation_info(&g_ns);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	memset(&info, 0, sizeof(info));
	rc = nvmf_ns_reservation_load(&g_ns, &info);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(info.rtype == 0);
	SPDK_CU_ASSERT_FATAL(info.crkey == 0);
	SPDK_CU_ASSERT_FATAL(info.ptpl_activated == true);
	unlink(g_ns.ptpl_file);

	ut_reservation_free_req(req);
	ut_reservation_deinit();
}

static void
test_reservation_release(void)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;
	struct spdk_nvmf_registrant *reg;

	ut_reservation_init();

	req = ut_reservation_build_req(16);
	rsp = &req->rsp->nvme_cpl;
	SPDK_CU_ASSERT_FATAL(req != NULL);

	ut_reservation_build_registrants();

	/* ACQUIRE: Host A with g_ctrlr1_A get reservation with
	 * type SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS
	 */
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_ACQUIRE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS, 0xa1, 0x0);
	nvmf_ns_reservation_acquire(&g_ns, &g_ctrlr1_A, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr1_A.hostid);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS);
	SPDK_CU_ASSERT_FATAL(g_ns.holder == reg);

	/* Test Case: Host B release the reservation */
	ut_reservation_build_release_request(req, SPDK_NVME_RESERVE_RELEASE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS, 0xb1);
	nvmf_ns_reservation_release(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == 0);
	SPDK_CU_ASSERT_FATAL(g_ns.crkey == 0);
	SPDK_CU_ASSERT_FATAL(g_ns.holder == NULL);

	/* Test Case: Host C clear the registrants */
	ut_reservation_build_release_request(req, SPDK_NVME_RESERVE_CLEAR, 0,
					     0, 0xc1);
	nvmf_ns_reservation_release(&g_ns, &g_ctrlr_C, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr1_A.hostid);
	SPDK_CU_ASSERT_FATAL(reg == NULL);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr2_A.hostid);
	SPDK_CU_ASSERT_FATAL(reg == NULL);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr_B.hostid);
	SPDK_CU_ASSERT_FATAL(reg == NULL);
	reg = nvmf_ns_reservation_get_registrant(&g_ns, &g_ctrlr_C.hostid);
	SPDK_CU_ASSERT_FATAL(reg == NULL);

	ut_reservation_free_req(req);
	ut_reservation_deinit();
}

void
nvmf_ctrlr_reservation_notice_log(struct spdk_nvmf_ctrlr *ctrlr,
				  struct spdk_nvmf_ns *ns,
				  enum spdk_nvme_reservation_notification_log_page_type type)
{
	ctrlr->num_avail_log_pages++;
}

static void
test_reservation_unregister_notification(void)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;

	ut_reservation_init();

	req = ut_reservation_build_req(16);
	SPDK_CU_ASSERT_FATAL(req != NULL);
	rsp = &req->rsp->nvme_cpl;

	ut_reservation_build_registrants();

	/* ACQUIRE: Host B with g_ctrlr_B get reservation with
	 * type SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY
	 */
	rsp->status.sc = 0xff;
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_ACQUIRE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY, 0xb1, 0x0);
	nvmf_ns_reservation_acquire(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY);

	/* Test Case : g_ctrlr_B holds the reservation, g_ctrlr_B unregister the registration.
	 * Reservation release notification sends to g_ctrlr1_A/g_ctrlr2_A/g_ctrlr_C only for
	 * SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY or SPDK_NVME_RESERVE_EXCLUSIVE_ACCESS_REG_ONLY
	 * type.
	 */
	rsp->status.sc = 0xff;
	g_ctrlr1_A.num_avail_log_pages = 0;
	g_ctrlr2_A.num_avail_log_pages = 0;
	g_ctrlr_B.num_avail_log_pages = 5;
	g_ctrlr_C.num_avail_log_pages = 0;
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_UNREGISTER_KEY,
					      0, 0, 0xb1, 0);
	nvmf_ns_reservation_register(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == 0);
	SPDK_CU_ASSERT_FATAL(1 == g_ctrlr1_A.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(1 == g_ctrlr2_A.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(5 == g_ctrlr_B.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(1 == g_ctrlr_C.num_avail_log_pages);

	ut_reservation_free_req(req);
	ut_reservation_deinit();
}

static void
test_reservation_release_notification(void)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;

	ut_reservation_init();

	req = ut_reservation_build_req(16);
	SPDK_CU_ASSERT_FATAL(req != NULL);
	rsp = &req->rsp->nvme_cpl;

	ut_reservation_build_registrants();

	/* ACQUIRE: Host B with g_ctrlr_B get reservation with
	 * type SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY
	 */
	rsp->status.sc = 0xff;
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_ACQUIRE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY, 0xb1, 0x0);
	nvmf_ns_reservation_acquire(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY);

	/* Test Case : g_ctrlr_B holds the reservation, g_ctrlr_B release the reservation.
	 * Reservation release notification sends to g_ctrlr1_A/g_ctrlr2_A/g_ctrlr_C.
	 */
	rsp->status.sc = 0xff;
	g_ctrlr1_A.num_avail_log_pages = 0;
	g_ctrlr2_A.num_avail_log_pages = 0;
	g_ctrlr_B.num_avail_log_pages = 5;
	g_ctrlr_C.num_avail_log_pages = 0;
	ut_reservation_build_release_request(req, SPDK_NVME_RESERVE_RELEASE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY, 0xb1);
	nvmf_ns_reservation_release(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == 0);
	SPDK_CU_ASSERT_FATAL(1 == g_ctrlr1_A.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(1 == g_ctrlr2_A.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(5 == g_ctrlr_B.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(1 == g_ctrlr_C.num_avail_log_pages);

	ut_reservation_free_req(req);
	ut_reservation_deinit();
}

static void
test_reservation_release_notification_write_exclusive(void)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;

	ut_reservation_init();

	req = ut_reservation_build_req(16);
	SPDK_CU_ASSERT_FATAL(req != NULL);
	rsp = &req->rsp->nvme_cpl;

	ut_reservation_build_registrants();

	/* ACQUIRE: Host B with g_ctrlr_B get reservation with
	 * type SPDK_NVME_RESERVE_WRITE_EXCLUSIVE
	 */
	rsp->status.sc = 0xff;
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_ACQUIRE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE, 0xb1, 0x0);
	nvmf_ns_reservation_acquire(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE);

	/* Test Case : g_ctrlr_B holds the reservation, g_ctrlr_B release the reservation.
	 * Because the reservation type is SPDK_NVME_RESERVE_WRITE_EXCLUSIVE,
	 * no reservation notification occurs.
	 */
	rsp->status.sc = 0xff;
	g_ctrlr1_A.num_avail_log_pages = 5;
	g_ctrlr2_A.num_avail_log_pages = 5;
	g_ctrlr_B.num_avail_log_pages = 5;
	g_ctrlr_C.num_avail_log_pages = 5;
	ut_reservation_build_release_request(req, SPDK_NVME_RESERVE_RELEASE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE, 0xb1);
	nvmf_ns_reservation_release(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == 0);
	SPDK_CU_ASSERT_FATAL(5 == g_ctrlr1_A.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(5 == g_ctrlr2_A.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(5 == g_ctrlr_B.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(5 == g_ctrlr_C.num_avail_log_pages);

	ut_reservation_free_req(req);
	ut_reservation_deinit();
}

static void
test_reservation_clear_notification(void)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;

	ut_reservation_init();

	req = ut_reservation_build_req(16);
	SPDK_CU_ASSERT_FATAL(req != NULL);
	rsp = &req->rsp->nvme_cpl;

	ut_reservation_build_registrants();

	/* ACQUIRE: Host B with g_ctrlr_B get reservation with
	 * type SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY
	 */
	rsp->status.sc = 0xff;
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_ACQUIRE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY, 0xb1, 0x0);
	nvmf_ns_reservation_acquire(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY);

	/* Test Case : g_ctrlr_B holds the reservation, g_ctrlr_B clear the reservation.
	 * Reservation Preempted notification sends to g_ctrlr1_A/g_ctrlr2_A/g_ctrlr_C.
	 */
	rsp->status.sc = 0xff;
	g_ctrlr1_A.num_avail_log_pages = 0;
	g_ctrlr2_A.num_avail_log_pages = 0;
	g_ctrlr_B.num_avail_log_pages = 5;
	g_ctrlr_C.num_avail_log_pages = 0;
	ut_reservation_build_release_request(req, SPDK_NVME_RESERVE_CLEAR, 0,
					     0, 0xb1);
	nvmf_ns_reservation_release(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == 0);
	SPDK_CU_ASSERT_FATAL(1 == g_ctrlr1_A.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(1 == g_ctrlr2_A.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(5 == g_ctrlr_B.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(1 == g_ctrlr_C.num_avail_log_pages);

	ut_reservation_free_req(req);
	ut_reservation_deinit();
}

static void
test_reservation_preempt_notification(void)
{
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;

	ut_reservation_init();

	req = ut_reservation_build_req(16);
	SPDK_CU_ASSERT_FATAL(req != NULL);
	rsp = &req->rsp->nvme_cpl;

	ut_reservation_build_registrants();

	/* ACQUIRE: Host B with g_ctrlr_B get reservation with
	 * type SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY
	 */
	rsp->status.sc = 0xff;
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_ACQUIRE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY, 0xb1, 0x0);
	nvmf_ns_reservation_acquire(&g_ns, &g_ctrlr_B, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY);

	/* Test Case : g_ctrlr_B holds the reservation, g_ctrlr_C preempt g_ctrlr_B,
	 * g_ctrlr_B registrant is unregistered, and reservation is preempted.
	 * Registration Preempted notification sends to g_ctrlr_B.
	 * Reservation Preempted notification sends to g_ctrlr1_A/g_ctrlr2_A.
	 */
	rsp->status.sc = 0xff;
	g_ctrlr1_A.num_avail_log_pages = 0;
	g_ctrlr2_A.num_avail_log_pages = 0;
	g_ctrlr_B.num_avail_log_pages = 0;
	g_ctrlr_C.num_avail_log_pages = 5;
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_PREEMPT, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS, 0xc1, 0xb1);
	nvmf_ns_reservation_acquire(&g_ns, &g_ctrlr_C, req);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS);
	SPDK_CU_ASSERT_FATAL(1 == g_ctrlr1_A.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(1 == g_ctrlr2_A.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(1 == g_ctrlr_B.num_avail_log_pages);
	SPDK_CU_ASSERT_FATAL(5 == g_ctrlr_C.num_avail_log_pages);

	ut_reservation_free_req(req);
	ut_reservation_deinit();
}

static int
nvmf_tgt_create_poll_group(void *io_device, void *ctx_buf)
{
	return 0;
}

static void
nvmf_tgt_destroy_poll_group(void *io_device, void *ctx_buf)
{
}

static void
test_spdk_nvmf_ns_event(void)
{
	struct spdk_nvmf_tgt tgt = {};
	struct spdk_nvmf_subsystem subsystem = {
		.max_nsid = 1024,
		.ns = NULL,
		.tgt = &tgt,
		.state_changes = TAILQ_HEAD_INITIALIZER(subsystem.state_changes),
	};
	struct spdk_nvmf_ctrlr ctrlr = {
		.subsys = &subsystem
	};
	struct spdk_nvmf_ns_opts ns_opts;
	uint32_t nsid;
	struct spdk_bdev *bdev;
	struct spdk_thread *thread;

	ctrlr.visible_ns = spdk_bit_array_create(1);
	spdk_bit_array_set(ctrlr.visible_ns, 0);

	thread = spdk_get_thread();
	SPDK_CU_ASSERT_FATAL(thread != NULL);

	subsystem.ns = calloc(subsystem.max_nsid, sizeof(struct spdk_nvmf_subsystem_ns *));
	SPDK_CU_ASSERT_FATAL(subsystem.ns != NULL);
	subsystem.ana_group = calloc(subsystem.max_nsid, sizeof(uint32_t));
	SPDK_CU_ASSERT_FATAL(subsystem.ana_group != NULL);

	tgt.max_subsystems = 1024;
	tgt.subsystem_ids = spdk_bit_array_create(tgt.max_subsystems);
	RB_INIT(&tgt.subsystems);

	spdk_io_device_register(&tgt,
				nvmf_tgt_create_poll_group,
				nvmf_tgt_destroy_poll_group,
				sizeof(struct spdk_nvmf_poll_group),
				NULL);

	/* Add one namespace */
	spdk_nvmf_ns_opts_get_defaults(&ns_opts, sizeof(ns_opts));
	nsid = spdk_nvmf_subsystem_add_ns_ext(&subsystem, "bdev1", &ns_opts, sizeof(ns_opts), NULL);
	CU_ASSERT(nsid == 1);
	CU_ASSERT(NULL != subsystem.ns[0]);
	CU_ASSERT(subsystem.ns[nsid - 1]->bdev == &g_bdevs[nsid - 1]);

	bdev = subsystem.ns[nsid - 1]->bdev;

	/* Add one controller */
	TAILQ_INIT(&subsystem.ctrlrs);
	TAILQ_INSERT_TAIL(&subsystem.ctrlrs, &ctrlr, link);

	/* Namespace resize event */
	subsystem.state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	g_ns_changed_nsid = 0xFFFFFFFF;
	g_ns_changed_ctrlr = NULL;
	nvmf_ns_event(SPDK_BDEV_EVENT_RESIZE, bdev, subsystem.ns[0]);
	CU_ASSERT(SPDK_NVMF_SUBSYSTEM_PAUSING == subsystem.state);

	poll_threads();
	CU_ASSERT(1 == g_ns_changed_nsid);
	CU_ASSERT(&ctrlr == g_ns_changed_ctrlr);
	CU_ASSERT(SPDK_NVMF_SUBSYSTEM_ACTIVE == subsystem.state);

	/* Namespace remove event */
	subsystem.state = SPDK_NVMF_SUBSYSTEM_ACTIVE;
	g_ns_changed_nsid = 0xFFFFFFFF;
	g_ns_changed_ctrlr = NULL;
	nvmf_ns_event(SPDK_BDEV_EVENT_REMOVE, bdev, subsystem.ns[0]);
	CU_ASSERT(SPDK_NVMF_SUBSYSTEM_PAUSING == subsystem.state);
	CU_ASSERT(0xFFFFFFFF == g_ns_changed_nsid);
	CU_ASSERT(NULL == g_ns_changed_ctrlr);

	poll_threads();
	CU_ASSERT(1 == g_ns_changed_nsid);
	CU_ASSERT(&ctrlr == g_ns_changed_ctrlr);
	CU_ASSERT(NULL == subsystem.ns[0]);
	CU_ASSERT(SPDK_NVMF_SUBSYSTEM_ACTIVE == subsystem.state);

	spdk_io_device_unregister(&tgt, NULL);

	poll_threads();

	free(subsystem.ns);
	free(subsystem.ana_group);
	spdk_bit_array_free(&ctrlr.visible_ns);
	spdk_bit_array_free(&tgt.subsystem_ids);
}

static void
test_nvmf_ns_reservation_add_remove_registrant(void)
{
	struct spdk_nvmf_ns ns = {};
	struct spdk_nvmf_ctrlr ctrlr = {};
	struct spdk_nvmf_registrant *reg = NULL;
	int rc;

	TAILQ_INIT(&ns.registrants);
	spdk_uuid_generate(&ctrlr.hostid);

	rc = nvmf_ns_reservation_add_registrant(&ns, &ctrlr, 0xa11);
	CU_ASSERT(rc == 0);
	reg = TAILQ_FIRST(&ns.registrants);
	SPDK_CU_ASSERT_FATAL(reg != NULL);
	CU_ASSERT(ns.gen == 1);
	CU_ASSERT(reg->rkey == 0xa11);
	CU_ASSERT(!strncmp((uint8_t *)&reg->hostid, (uint8_t *)&ctrlr.hostid, sizeof(ctrlr.hostid)));

	nvmf_ns_reservation_remove_registrant(&ns, reg);
	CU_ASSERT(TAILQ_EMPTY(&ns.registrants));
	CU_ASSERT(ns.gen == 2);
}

static void
test_nvmf_subsystem_destroy_cb(void *cb_arg)
{
}

static void
test_nvmf_subsystem_add_ctrlr(void)
{
	int rc;
	struct spdk_nvmf_ctrlr ctrlr = {};
	struct spdk_nvmf_tgt tgt = {};
	char nqn[256] = "nqn.2016-06.io.spdk:subsystem1";
	struct spdk_nvmf_subsystem *subsystem = NULL;

	tgt.max_subsystems = 1024;
	tgt.subsystem_ids = spdk_bit_array_create(tgt.max_subsystems);
	RB_INIT(&tgt.subsystems);

	subsystem = spdk_nvmf_subsystem_create(&tgt, nqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);
	ctrlr.subsys = subsystem;

	ctrlr.dynamic_ctrlr = true;
	rc = nvmf_subsystem_add_ctrlr(subsystem, &ctrlr);
	CU_ASSERT(rc == 0);
	CU_ASSERT(!TAILQ_EMPTY(&subsystem->ctrlrs));
	CU_ASSERT(ctrlr.cntlid == 1);
	CU_ASSERT(nvmf_subsystem_get_ctrlr(subsystem, 1) == &ctrlr);

	nvmf_subsystem_remove_ctrlr(subsystem, &ctrlr);
	CU_ASSERT(TAILQ_EMPTY(&subsystem->ctrlrs));
	rc = spdk_nvmf_subsystem_destroy(subsystem, test_nvmf_subsystem_destroy_cb, NULL);
	CU_ASSERT(rc == 0);
	spdk_bit_array_free(&tgt.subsystem_ids);
}

static void
_add_transport_cb(void *arg, int status)
{
	CU_ASSERT(status == 0);
}

static int
transport_subsystem_add_host_err(struct spdk_nvmf_transport *transport,
				 const struct spdk_nvmf_subsystem *subsystem,
				 const char *hostnqn,
				 const struct spdk_json_val *transport_specific)
{
	return -1;
}

void
spdk_nvmf_tgt_add_transport(struct spdk_nvmf_tgt *tgt,
			    struct spdk_nvmf_transport *transport,
			    spdk_nvmf_tgt_add_transport_done_fn cb_fn,
			    void *cb_arg)
{
	TAILQ_INSERT_TAIL(&tgt->transports, transport, link);
}

static struct spdk_nvmf_transport *
transport_create(struct spdk_nvmf_transport_opts *opts)
{
	return &g_transport;
}

static void
test_spdk_nvmf_subsystem_add_host(void)
{
	struct spdk_nvmf_tgt tgt = {};
	struct spdk_nvmf_subsystem *subsystem = NULL;
	int rc;
	const char hostnqn[] = "nqn.2016-06.io.spdk:host1";
	const char subsystemnqn[] = "nqn.2016-06.io.spdk:subsystem1";
	struct spdk_nvmf_transport_opts opts = {
		.opts_size = sizeof(struct spdk_nvmf_transport_opts),
		.io_unit_size = 8192,
		.kas = NVMF_DEFAULT_KAS,
		.min_kato = NVMF_DEFAULT_MIN_KATO,
	};
	const struct spdk_nvmf_transport_ops test_ops = {
		.name = "transport_ut",
		.create = transport_create,
		.subsystem_add_host = transport_subsystem_add_host_err,
	};
	struct spdk_nvmf_transport *transport;

	tgt.max_subsystems = 1024;
	tgt.subsystem_ids = spdk_bit_array_create(tgt.max_subsystems);
	RB_INIT(&tgt.subsystems);

	subsystem = spdk_nvmf_subsystem_create(&tgt, subsystemnqn, SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);
	CU_ASSERT_STRING_EQUAL(subsystem->subnqn, subsystemnqn);

	rc = spdk_nvmf_subsystem_add_host(subsystem, hostnqn, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(!TAILQ_EMPTY(&subsystem->hosts));

	/* Add existing nqn, this function isn't allowed to be called if the nqn was previously added. */
	rc = spdk_nvmf_subsystem_add_host(subsystem, hostnqn, NULL);
	CU_ASSERT(rc == -EINVAL);

	rc = spdk_nvmf_subsystem_remove_host(subsystem, hostnqn);
	CU_ASSERT(rc == 0);
	CU_ASSERT(TAILQ_EMPTY(&subsystem->hosts));

	/* No available nqn */
	rc = spdk_nvmf_subsystem_remove_host(subsystem, hostnqn);
	CU_ASSERT(rc == -ENOENT);

	/* Ensure hostnqn list remains empty after transport callback fails */
	spdk_nvmf_transport_register(&test_ops);
	transport = spdk_nvmf_transport_create("transport_ut", &opts);
	SPDK_CU_ASSERT_FATAL(transport != NULL);

	TAILQ_INIT(&tgt.transports);
	spdk_nvmf_tgt_add_transport(&tgt, transport, _add_transport_cb, 0);

	rc = spdk_nvmf_subsystem_add_host(subsystem, hostnqn, NULL);
	CU_ASSERT(rc != 0);
	CU_ASSERT(TAILQ_EMPTY(&subsystem->hosts));

	spdk_nvmf_subsystem_destroy(subsystem, NULL, NULL);
	spdk_bit_array_free(&tgt.subsystem_ids);
}

static void
test_nvmf_ns_reservation_report(void)
{
	struct spdk_nvmf_ns ns = {};
	struct spdk_nvmf_ctrlr ctrlr = {};
	struct spdk_nvmf_request req = {};
	union nvmf_h2c_msg cmd = {};
	union nvmf_c2h_msg rsp = {};
	struct spdk_nvme_registered_ctrlr_extended_data *ctrlr_data;
	struct spdk_nvme_reservation_status_extended_data *status_data;
	struct spdk_nvmf_registrant *reg;
	void *data;

	data = calloc(1, sizeof(*status_data) + sizeof(*ctrlr_data) * 2);
	reg = calloc(2, sizeof(struct spdk_nvmf_registrant));
	SPDK_CU_ASSERT_FATAL(data != NULL && reg != NULL);

	req.length = sizeof(*status_data) + sizeof(*ctrlr_data) * 2;
	SPDK_IOV_ONE(req.iov, &req.iovcnt, data, req.length);

	req.cmd = &cmd;
	req.rsp = &rsp;
	ns.gen = 1;
	ns.rtype = SPDK_NVME_RESERVE_WRITE_EXCLUSIVE;
	ns.ptpl_activated = true;
	cmd.nvme_cmd.cdw11_bits.resv_report.eds = true;
	cmd.nvme_cmd.cdw10 = 100;
	reg[0].rkey = 0xa;
	reg[1].rkey = 0xb;
	spdk_uuid_generate(&reg[0].hostid);
	spdk_uuid_generate(&reg[1].hostid);
	TAILQ_INIT(&ns.registrants);
	TAILQ_INSERT_TAIL(&ns.registrants, &reg[0], link);
	TAILQ_INSERT_TAIL(&ns.registrants, &reg[1], link);

	nvmf_ns_reservation_report(&ns, &ctrlr, &req);
	CU_ASSERT(req.rsp->nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);
	CU_ASSERT(req.rsp->nvme_cpl.status.sc == SPDK_NVME_SC_SUCCESS);
	/* Get ctrlr data and status data pointers */
	ctrlr_data = (void *)((char *)req.iov[0].iov_base + sizeof(*status_data));
	status_data = (void *)req.iov[0].iov_base;
	SPDK_CU_ASSERT_FATAL(status_data != NULL && ctrlr_data != NULL);
	CU_ASSERT(status_data->data.gen == 1);
	CU_ASSERT(status_data->data.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE);
	CU_ASSERT(status_data->data.ptpls == true);
	CU_ASSERT(status_data->data.regctl == 2);
	CU_ASSERT(ctrlr_data->cntlid == 0xffff);
	CU_ASSERT(ctrlr_data->rcsts.status == false);
	CU_ASSERT(ctrlr_data->rkey ==  0xa);
	CU_ASSERT(!spdk_uuid_compare((struct spdk_uuid *)ctrlr_data->hostid, &reg[0].hostid));
	/* Check second ctrlr data */
	ctrlr_data++;
	CU_ASSERT(ctrlr_data->cntlid == 0xffff);
	CU_ASSERT(ctrlr_data->rcsts.status == false);
	CU_ASSERT(ctrlr_data->rkey ==  0xb);
	CU_ASSERT(!spdk_uuid_compare((struct spdk_uuid *)ctrlr_data->hostid, &reg[1].hostid));

	/* extended controller data structure */
	spdk_iov_memset(req.iov, req.iovcnt, 0);
	memset(req.rsp, 0, sizeof(*req.rsp));
	cmd.nvme_cmd.cdw11_bits.resv_report.eds = false;

	nvmf_ns_reservation_report(&ns, &ctrlr, &req);
	CU_ASSERT(req.rsp->nvme_cpl.status.sc == SPDK_NVME_SC_HOSTID_INCONSISTENT_FORMAT);
	CU_ASSERT(req.rsp->nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);

	/* Transfer length invalid */
	spdk_iov_memset(req.iov, req.iovcnt, 0);
	memset(req.rsp, 0, sizeof(*req.rsp));
	cmd.nvme_cmd.cdw11_bits.resv_report.eds = true;
	cmd.nvme_cmd.cdw10 = 0;

	nvmf_ns_reservation_report(&ns, &ctrlr, &req);
	CU_ASSERT(req.rsp->nvme_cpl.status.sc == SPDK_NVME_SC_INTERNAL_DEVICE_ERROR);
	CU_ASSERT(req.rsp->nvme_cpl.status.sct == SPDK_NVME_SCT_GENERIC);

	free(req.iov[0].iov_base);
	free(reg);
}

static void
test_nvmf_nqn_is_valid(void)
{
	bool rc;
	char uuid[SPDK_NVMF_UUID_STRING_LEN + 1] = {};
	char nqn[SPDK_NVMF_NQN_MAX_LEN + 1] = {};
	struct spdk_uuid s_uuid = {};

	spdk_uuid_generate(&s_uuid);
	spdk_uuid_fmt_lower(uuid, sizeof(uuid), &s_uuid);

	/* discovery nqn */
	snprintf(nqn, sizeof(nqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

	rc = nvmf_nqn_is_valid(nqn);
	CU_ASSERT(rc == true);

	/* nqn with uuid */
	memset(nqn, 0xff, sizeof(nqn));
	snprintf(nqn, sizeof(nqn), "%s%s", SPDK_NVMF_NQN_UUID_PRE, uuid);

	rc = nvmf_nqn_is_valid(nqn);
	CU_ASSERT(rc == true);

	/* Check nqn valid reverse domain */
	memset(nqn, 0xff, sizeof(nqn));
	snprintf(nqn, sizeof(nqn), "%s", "nqn.2016-06.io.spdk:cnode1");

	rc = nvmf_nqn_is_valid(nqn);
	CU_ASSERT(rc == true);

	/* Invalid nqn length */
	memset(nqn, 0xff, sizeof(nqn));
	snprintf(nqn, sizeof(nqn), "%s", "nqn.");

	rc = nvmf_nqn_is_valid(nqn);
	CU_ASSERT(rc == false);

	/* Copy uuid to the nqn string, but omit the last character to make it invalid */
	memset(nqn, 0, SPDK_NVMF_NQN_MAX_LEN + 1);
	snprintf(nqn, sizeof(nqn), "%s", SPDK_NVMF_NQN_UUID_PRE);
	memcpy(&nqn[SPDK_NVMF_NQN_UUID_PRE_LEN], uuid, SPDK_NVMF_UUID_STRING_LEN - 1);

	rc = nvmf_nqn_is_valid(nqn);
	CU_ASSERT(rc == false);

	/* Invalid domain */
	memset(nqn, 0xff, SPDK_NVMF_NQN_MAX_LEN + 1);
	snprintf(nqn, sizeof(nqn), "%s", "nqn.2016-06.io...spdk:cnode1");

	rc = nvmf_nqn_is_valid(nqn);
	CU_ASSERT(rc == false);
}

static void
test_nvmf_ns_reservation_restore(void)
{
	struct spdk_nvmf_ns ns = {};
	struct spdk_nvmf_reservation_info info = {};
	struct spdk_bdev bdev = {};
	struct spdk_uuid s_uuid = {};
	struct spdk_nvmf_registrant *reg0, *reg1;
	char uuid[SPDK_UUID_STRING_LEN] = {};
	int rc;

	ns.bdev = &bdev;
	TAILQ_INIT(&ns.registrants);
	info.ptpl_activated = true;
	info.num_regs = 2;
	info.rtype = SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS;
	info.registrants[0].rkey = 0xb;
	info.registrants[1].rkey = 0xc;

	/* Generate and prepare uuids, make sure bdev and info uuid are the same */
	spdk_uuid_generate(&s_uuid);
	spdk_uuid_fmt_lower(uuid, sizeof(uuid), &s_uuid);
	snprintf(info.holder_uuid, SPDK_UUID_STRING_LEN, "%s", uuid);
	snprintf(info.bdev_uuid, SPDK_UUID_STRING_LEN, "%s", uuid);
	snprintf(info.registrants[0].host_uuid, SPDK_UUID_STRING_LEN, "%s", uuid);
	spdk_uuid_copy(&bdev.uuid, &s_uuid);
	spdk_uuid_generate(&s_uuid);
	spdk_uuid_fmt_lower(uuid, sizeof(uuid), &s_uuid);
	snprintf(info.registrants[1].host_uuid, SPDK_UUID_STRING_LEN, "%s", uuid);

	/* info->rkey not exist in registrants */
	info.crkey = 0xa;

	rc = nvmf_ns_reservation_restore(&ns, &info);
	CU_ASSERT(rc == -EINVAL);

	/* info->rkey exists in registrants */
	info.crkey = 0xb;

	rc = nvmf_ns_reservation_restore(&ns, &info);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ns.crkey == 0xb);
	CU_ASSERT(ns.rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_ALL_REGS);
	CU_ASSERT(ns.ptpl_activated == true);
	/* Check two registrant`s rkey */
	reg0 = TAILQ_FIRST(&ns.registrants);
	reg1 = TAILQ_NEXT(reg0, link);
	CU_ASSERT(ns.holder == reg0);
	CU_ASSERT(reg0->rkey == 0xb);
	CU_ASSERT(reg1->rkey == 0xc);

	rc = nvmf_ns_reservation_clear_all_registrants(&ns);
	CU_ASSERT(rc == 2);
	CU_ASSERT(TAILQ_EMPTY(&ns.registrants));

	/* Existing bdev UUID is different with configuration */
	spdk_uuid_generate(&s_uuid);
	spdk_uuid_fmt_lower(uuid, sizeof(uuid), &s_uuid);
	snprintf(info.bdev_uuid, SPDK_UUID_STRING_LEN, "%s", uuid);
	spdk_uuid_generate(&s_uuid);
	spdk_uuid_copy(&bdev.uuid, &s_uuid);

	rc = nvmf_ns_reservation_restore(&ns, &info);
	CU_ASSERT(rc == -EINVAL);

	/* Check restore without reservation */
	spdk_uuid_fmt_lower(info.bdev_uuid, sizeof(info.bdev_uuid), &bdev.uuid);
	info.rtype = 0;
	info.crkey = 0;
	memset(info.holder_uuid, 0, SPDK_UUID_STRING_LEN);

	rc = nvmf_ns_reservation_restore(&ns, &info);
	CU_ASSERT(rc == 0);
	CU_ASSERT(ns.crkey == 0);
	CU_ASSERT(ns.rtype == 0);
	CU_ASSERT(ns.ptpl_activated == true);
	CU_ASSERT(ns.holder == NULL);
	reg0 = TAILQ_FIRST(&ns.registrants);
	reg1 = TAILQ_NEXT(reg0, link);
	CU_ASSERT(reg0->rkey == 0xb);
	CU_ASSERT(reg1->rkey == 0xc);

	rc = nvmf_ns_reservation_clear_all_registrants(&ns);
	CU_ASSERT(rc == 2);
	CU_ASSERT(TAILQ_EMPTY(&ns.registrants));
}

static void
ut_nvmf_subsystem_paused(struct spdk_nvmf_subsystem *subsystem, void *ctx, int status)
{
	CU_ASSERT_EQUAL(status, 0);
	CU_ASSERT_EQUAL(subsystem->state, SPDK_NVMF_SUBSYSTEM_PAUSED);
}

static void
test_nvmf_subsystem_state_change(void)
{
	struct spdk_nvmf_tgt tgt = {};
	struct spdk_nvmf_subsystem *subsystem, *discovery_subsystem;
	int rc;

	tgt.max_subsystems = 1024;
	tgt.subsystem_ids = spdk_bit_array_create(tgt.max_subsystems);
	RB_INIT(&tgt.subsystems);

	discovery_subsystem = spdk_nvmf_subsystem_create(&tgt, SPDK_NVMF_DISCOVERY_NQN,
			      SPDK_NVMF_SUBTYPE_DISCOVERY_CURRENT, 0);
	SPDK_CU_ASSERT_FATAL(discovery_subsystem != NULL);
	subsystem = spdk_nvmf_subsystem_create(&tgt, "nqn.2016-06.io.spdk:subsystem1",
					       SPDK_NVMF_SUBTYPE_NVME, 0);
	SPDK_CU_ASSERT_FATAL(subsystem != NULL);

	spdk_io_device_register(&tgt,
				nvmf_tgt_create_poll_group,
				nvmf_tgt_destroy_poll_group,
				sizeof(struct spdk_nvmf_poll_group),
				NULL);

	rc = spdk_nvmf_subsystem_start(discovery_subsystem, NULL, NULL);
	CU_ASSERT(rc == 0);
	poll_threads();
	CU_ASSERT(discovery_subsystem->state == SPDK_NVMF_SUBSYSTEM_ACTIVE);
	rc = spdk_nvmf_subsystem_start(subsystem, NULL, NULL);
	CU_ASSERT(rc == 0);
	poll_threads();
	CU_ASSERT(subsystem->state == SPDK_NVMF_SUBSYSTEM_ACTIVE);

	rc = spdk_nvmf_subsystem_pause(subsystem, SPDK_NVME_GLOBAL_NS_TAG,
				       ut_nvmf_subsystem_paused, NULL);
	CU_ASSERT(rc == 0);
	rc = spdk_nvmf_subsystem_stop(subsystem, NULL, NULL);
	CU_ASSERT(rc == 0);
	poll_threads();
	CU_ASSERT(subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE);

	rc = spdk_nvmf_subsystem_stop(discovery_subsystem, NULL, NULL);
	CU_ASSERT(rc == 0);
	poll_threads();
	CU_ASSERT(discovery_subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE);
	rc = spdk_nvmf_subsystem_stop(subsystem, NULL, NULL);
	CU_ASSERT(rc == 0);
	poll_threads();
	CU_ASSERT(subsystem->state == SPDK_NVMF_SUBSYSTEM_INACTIVE);

	rc = spdk_nvmf_subsystem_destroy(subsystem, NULL, NULL);
	CU_ASSERT(rc == 0);
	rc = spdk_nvmf_subsystem_destroy(discovery_subsystem, NULL, NULL);
	CU_ASSERT(rc == 0);

	spdk_io_device_unregister(&tgt, NULL);
	poll_threads();

	spdk_bit_array_free(&tgt.subsystem_ids);
}

static bool
ut_is_ptpl_capable(const struct spdk_nvmf_ns *ns)
{
	return true;
}

static struct spdk_nvmf_reservation_info g_resv_info;

static int
ut_update_reservation(const struct spdk_nvmf_ns *ns, const struct spdk_nvmf_reservation_info *info)
{
	g_resv_info = *info;

	return 0;
}

static int
ut_load_reservation(const struct spdk_nvmf_ns *ns, struct spdk_nvmf_reservation_info *info)
{
	*info = g_resv_info;

	return 0;
}

static void
test_nvmf_reservation_custom_ops(void)
{
	struct spdk_nvmf_ns_reservation_ops ops = {
		.is_ptpl_capable = ut_is_ptpl_capable,
		.update = ut_update_reservation,
		.load = ut_load_reservation,
	};
	struct spdk_nvmf_request *req;
	struct spdk_nvme_cpl *rsp;
	struct spdk_nvmf_registrant *reg;
	bool update_sgroup = false;
	struct spdk_nvmf_tgt tgt = {};
	struct spdk_nvmf_subsystem subsystem = {
		.max_nsid = 4,
		.tgt = &tgt,
	};
	uint32_t nsid;
	struct spdk_nvmf_ns *ns;
	int rc;

	subsystem.ns = calloc(subsystem.max_nsid, sizeof(struct spdk_nvmf_subsystem_ns *));
	SPDK_CU_ASSERT_FATAL(subsystem.ns != NULL);
	subsystem.ana_group = calloc(subsystem.max_nsid, sizeof(uint32_t));
	SPDK_CU_ASSERT_FATAL(subsystem.ana_group != NULL);

	spdk_nvmf_set_custom_ns_reservation_ops(&ops);

	ut_reservation_init();

	req = ut_reservation_build_req(16);
	rsp = &req->rsp->nvme_cpl;
	SPDK_CU_ASSERT_FATAL(req != NULL);

	/* Add a registrant and activate ptpl */
	ut_reservation_build_register_request(req, SPDK_NVME_RESERVE_REGISTER_KEY, 0,
					      SPDK_NVME_RESERVE_PTPL_PERSIST_POWER_LOSS, 0, 0xa1);
	update_sgroup = nvmf_ns_reservation_register(&g_ns, &g_ctrlr1_A, req);
	SPDK_CU_ASSERT_FATAL(update_sgroup == true);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	SPDK_CU_ASSERT_FATAL(g_ns.ptpl_activated == true);
	rc = nvmf_ns_update_reservation_info(&g_ns);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	/* Acquire a reservation */
	rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
	ut_reservation_build_acquire_request(req, SPDK_NVME_RESERVE_ACQUIRE, 0,
					     SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY, 0xa1, 0x0);
	update_sgroup = nvmf_ns_reservation_acquire(&g_ns, &g_ctrlr1_A, req);
	SPDK_CU_ASSERT_FATAL(update_sgroup == true);
	SPDK_CU_ASSERT_FATAL(rsp->status.sc == SPDK_NVME_SC_SUCCESS);
	rc = nvmf_ns_update_reservation_info(&g_ns);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	/* Add the namespace using a different subsystem.
	 * Reservation information should be restored. */
	nsid = spdk_nvmf_subsystem_add_ns_ext(&subsystem, g_ns.bdev->name, NULL, 0, NULL);
	CU_ASSERT(nsid == 1);

	ns = _nvmf_subsystem_get_ns(&subsystem, nsid);
	SPDK_CU_ASSERT_FATAL(ns != NULL);
	CU_ASSERT(ns->crkey == 0xa1);
	CU_ASSERT(ns->rtype == SPDK_NVME_RESERVE_WRITE_EXCLUSIVE_REG_ONLY);
	CU_ASSERT(ns->ptpl_activated == true);

	reg = nvmf_ns_reservation_get_registrant(ns, &g_ctrlr1_A.hostid);
	SPDK_CU_ASSERT_FATAL(reg != NULL);
	SPDK_CU_ASSERT_FATAL(!spdk_uuid_compare(&g_ctrlr1_A.hostid, &reg->hostid));
	CU_ASSERT(reg == ns->holder);

	rc = spdk_nvmf_subsystem_remove_ns(&subsystem, nsid);
	CU_ASSERT(rc == 0);

	free(subsystem.ns);
	free(subsystem.ana_group);
	ut_reservation_free_req(req);
	ut_reservation_deinit();
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("nvmf", NULL, NULL);

	CU_ADD_TEST(suite, nvmf_test_create_subsystem);
	CU_ADD_TEST(suite, test_spdk_nvmf_subsystem_add_ns);
	CU_ADD_TEST(suite, test_spdk_nvmf_subsystem_add_fdp_ns);
	CU_ADD_TEST(suite, test_spdk_nvmf_subsystem_set_sn);
	CU_ADD_TEST(suite, test_spdk_nvmf_ns_visible);
	CU_ADD_TEST(suite, test_reservation_register);
	CU_ADD_TEST(suite, test_reservation_register_with_ptpl);
	CU_ADD_TEST(suite, test_reservation_acquire_preempt_1);
	CU_ADD_TEST(suite, test_reservation_acquire_release_with_ptpl);
	CU_ADD_TEST(suite, test_reservation_release);
	CU_ADD_TEST(suite, test_reservation_unregister_notification);
	CU_ADD_TEST(suite, test_reservation_release_notification);
	CU_ADD_TEST(suite, test_reservation_release_notification_write_exclusive);
	CU_ADD_TEST(suite, test_reservation_clear_notification);
	CU_ADD_TEST(suite, test_reservation_preempt_notification);
	CU_ADD_TEST(suite, test_spdk_nvmf_ns_event);
	CU_ADD_TEST(suite, test_nvmf_ns_reservation_add_remove_registrant);
	CU_ADD_TEST(suite, test_nvmf_subsystem_add_ctrlr);
	CU_ADD_TEST(suite, test_spdk_nvmf_subsystem_add_host);
	CU_ADD_TEST(suite, test_nvmf_ns_reservation_report);
	CU_ADD_TEST(suite, test_nvmf_nqn_is_valid);
	CU_ADD_TEST(suite, test_nvmf_ns_reservation_restore);
	CU_ADD_TEST(suite, test_nvmf_subsystem_state_change);
	CU_ADD_TEST(suite, test_nvmf_reservation_custom_ops);

	allocate_threads(1);
	set_thread(0);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();

	free_threads();

	return num_failures;
}
