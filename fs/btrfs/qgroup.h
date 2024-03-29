/*
 * Copyright (C) 2014 Facebook.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#ifndef __BTRFS_QGROUP__
#define __BTRFS_QGROUP__

#include "ulist.h"
#include "delayed-ref.h"

/*
 * Btrfs qgroup overview
 *
 * Btrfs qgroup splits into 3 main part:
 * 1) Reserve
 *    Reserve metadata/data space for incoming operations
 *    Affect how qgroup limit works
 *
 * 2) Trace
 *    Tell btrfs qgroup to trace dirty extents.
 *
 *    Dirty extents including:
 *    - Newly allocated extents
 *    - Extents going to be deleted (in this trans)
 *    - Extents whose owner is going to be modified
 *
 *    This is the main part affects whether qgroup numbers will stay
 *    consistent.
 *    Btrfs qgroup can trace clean extents and won't cause any problem,
 *    but it will consume extra CPU time, it should be avoided if possible.
 *
 * 3) Account
 *    Btrfs qgroup will updates its numbers, based on dirty extents traced
 *    in previous step.
 *
 *    Normally at qgroup rescan and transaction commit time.
 */

/*
 * Record a dirty extent, and info qgroup to update quota on it
 * TODO: Use kmem cache to alloc it.
 */
struct btrfs_qgroup_extent_record {
	struct rb_node node;
	u64 bytenr;
	u64 num_bytes;
	struct ulist *old_roots;
};

/*
 * Qgroup reservation types:
 * DATA:
 *	space reserved for data
 *
 * META_PERTRANS:
 * 	Space reserved for metadata (per-transaction)
 * 	Due to the fact that qgroup data is only updated at transaction commit
 * 	time, reserved space for metadata must be kept until transaction
 * 	commitment.
 * 	Any metadata reserved used in btrfs_start_transaction() should be this
 * 	type.
 *
 * META_PREALLOC:
 *	There are cases where metadata space is reserved before starting
 *	transaction, and then btrfs_join_transaction() to get a trans handler.
 *	Any metadata reserved for such usage should be this type.
 *	And after join_transaction() part (or all) of such reservation should
 *	be converted into META_PERTRANS.
 */
enum btrfs_qgroup_rsv_type {
	BTRFS_QGROUP_RSV_DATA = 0,
	BTRFS_QGROUP_RSV_META_PERTRANS,
	BTRFS_QGROUP_RSV_META_PREALLOC,
	BTRFS_QGROUP_RSV_LAST,
};

/*
 * Represents how many bytes we have reserved for this qgroup.
 *
 * Each type should have different reservation behavior.
 * E.g, data follows its io_tree flag modification, while
 * *currently* meta is just reserve-and-clear during transcation.
 *
 * TODO: Add new type for reservation which can survive transaction commit.
 * Currect metadata reservation behavior is not suitable for such case.
 */
struct btrfs_qgroup_rsv {
	u64 values[BTRFS_QGROUP_RSV_LAST];
};

/*
 * one struct for each qgroup, organized in fs_info->qgroup_tree.
 */
struct btrfs_qgroup {
	u64 qgroupid;

	/*
	 * state
	 */
	u64 rfer;	/* referenced */
	u64 rfer_cmpr;	/* referenced compressed */
	u64 excl;	/* exclusive */
	u64 excl_cmpr;	/* exclusive compressed */

	/*
	 * limits
	 */
	u64 lim_flags;	/* which limits are set */
	u64 max_rfer;
	u64 max_excl;
	u64 rsv_rfer;
	u64 rsv_excl;

	/*
	 * reservation tracking
	 */
	struct btrfs_qgroup_rsv rsv;

	/*
	 * lists
	 */
	struct list_head groups;  /* groups this group is member of */
	struct list_head members; /* groups that are members of this group */
	struct list_head dirty;   /* dirty groups */
	struct rb_node node;	  /* tree of qgroups */

	/*
	 * temp variables for accounting operations
	 * Refer to qgroup_shared_accounting() for details.
	 */
	u64 old_refcnt;
	u64 new_refcnt;
};

/*
 * For qgroup event trace points only
 */
#define QGROUP_RESERVE		(1<<0)
#define QGROUP_RELEASE		(1<<1)
#define QGROUP_FREE		(1<<2)

int btrfs_quota_enable(struct btrfs_trans_handle *trans,
		       struct btrfs_fs_info *fs_info);
int btrfs_quota_disable(struct btrfs_trans_handle *trans,
			struct btrfs_fs_info *fs_info);
int btrfs_qgroup_rescan(struct btrfs_fs_info *fs_info);
void btrfs_qgroup_rescan_resume(struct btrfs_fs_info *fs_info);
int btrfs_qgroup_wait_for_completion(struct btrfs_fs_info *fs_info,
				     bool interruptible);
int btrfs_add_qgroup_relation(struct btrfs_trans_handle *trans,
			      struct btrfs_fs_info *fs_info, u64 src, u64 dst);
int btrfs_del_qgroup_relation(struct btrfs_trans_handle *trans,
			      struct btrfs_fs_info *fs_info, u64 src, u64 dst);
int btrfs_create_qgroup(struct btrfs_trans_handle *trans,
			struct btrfs_fs_info *fs_info, u64 qgroupid);
int btrfs_remove_qgroup(struct btrfs_trans_handle *trans,
			      struct btrfs_fs_info *fs_info, u64 qgroupid);
int btrfs_limit_qgroup(struct btrfs_trans_handle *trans,
		       struct btrfs_fs_info *fs_info, u64 qgroupid,
		       struct btrfs_qgroup_limit *limit);
int btrfs_read_qgroup_config(struct btrfs_fs_info *fs_info);
void btrfs_free_qgroup_config(struct btrfs_fs_info *fs_info);
struct btrfs_delayed_extent_op;

/*
 * Inform qgroup to trace one dirty extent, its info is recorded in @record.
 * So qgroup can account it at transaction committing time.
 *
 * No lock version, caller must acquire delayed ref lock and allocated memory,
 * then call btrfs_qgroup_trace_extent_post() after exiting lock context.
 *
 * Return 0 for success insert
 * Return >0 for existing record, caller can free @record safely.
 * Error is not possible
 */
int btrfs_qgroup_trace_extent_nolock(
		struct btrfs_fs_info *fs_info,
		struct btrfs_delayed_ref_root *delayed_refs,
		struct btrfs_qgroup_extent_record *record);

/*
 * Post handler after qgroup_trace_extent_nolock().
 *
 * NOTE: Current qgroup does the expensive backref walk at transaction
 * committing time with TRANS_STATE_COMMIT_DOING, this blocks incoming
 * new transaction.
 * This is designed to allow btrfs_find_all_roots() to get correct new_roots
 * result.
 *
 * However for old_roots there is no need to do backref walk at that time,
 * since we search commit roots to walk backref and result will always be
 * correct.
 *
 * Due to the nature of no lock version, we can't do backref there.
 * So we must call btrfs_qgroup_trace_extent_post() after exiting
 * spinlock context.
 *
 * TODO: If we can fix and prove btrfs_find_all_roots() can get correct result
 * using current root, then we can move all expensive backref walk out of
 * transaction committing, but not now as qgroup accounting will be wrong again.
 */
int btrfs_qgroup_trace_extent_post(struct btrfs_fs_info *fs_info,
				   struct btrfs_qgroup_extent_record *qrecord);

/*
 * Inform qgroup to trace one dirty extent, specified by @bytenr and
 * @num_bytes.
 * So qgroup can account it at commit trans time.
 *
 * Better encapsulated version, with memory allocation and backref walk for
 * commit roots.
 * So this can sleep.
 *
 * Return 0 if the operation is done.
 * Return <0 for error, like memory allocation failure or invalid parameter
 * (NULL trans)
 */
int btrfs_qgroup_trace_extent(struct btrfs_trans_handle *trans,
		struct btrfs_fs_info *fs_info, u64 bytenr, u64 num_bytes,
		gfp_t gfp_flag);

/*
 * Inform qgroup to trace all leaf items of data
 *
 * Return 0 for success
 * Return <0 for error(ENOMEM)
 */
int btrfs_qgroup_trace_leaf_items(struct btrfs_trans_handle *trans,
				  struct btrfs_fs_info *fs_info,
				  struct extent_buffer *eb);
/*
 * Inform qgroup to trace a whole subtree, including all its child tree
 * blocks and data.
 * The root tree block is specified by @root_eb.
 *
 * Normally used by relocation(tree block swap) and subvolume deletion.
 *
 * Return 0 for success
 * Return <0 for error(ENOMEM or tree search error)
 */
int btrfs_qgroup_trace_subtree(struct btrfs_trans_handle *trans,
			       struct btrfs_root *root,
			       struct extent_buffer *root_eb,
			       u64 root_gen, int root_level);
int
btrfs_qgroup_account_extent(struct btrfs_trans_handle *trans,
			    struct btrfs_fs_info *fs_info,
			    u64 bytenr, u64 num_bytes,
			    struct ulist *old_roots, struct ulist *new_roots);
int btrfs_qgroup_account_extents(struct btrfs_trans_handle *trans);
int btrfs_run_qgroups(struct btrfs_trans_handle *trans,
		      struct btrfs_fs_info *fs_info);
int btrfs_qgroup_inherit(struct btrfs_trans_handle *trans,
			 struct btrfs_fs_info *fs_info, u64 srcid, u64 objectid,
			 struct btrfs_qgroup_inherit *inherit);
void btrfs_qgroup_free_refroot(struct btrfs_fs_info *fs_info,
			       u64 ref_root, u64 num_bytes,
			       enum btrfs_qgroup_rsv_type type);
static inline void btrfs_qgroup_free_delayed_ref(struct btrfs_fs_info *fs_info,
						 u64 ref_root, u64 num_bytes)
{
	trace_btrfs_qgroup_free_delayed_ref(fs_info, ref_root, num_bytes);
	btrfs_qgroup_free_refroot(fs_info, ref_root, num_bytes,
				  BTRFS_QGROUP_RSV_DATA);
}

#ifdef CONFIG_BTRFS_FS_RUN_SANITY_TESTS
int btrfs_verify_qgroup_counts(struct btrfs_fs_info *fs_info, u64 qgroupid,
			       u64 rfer, u64 excl);
#endif

/* New io_tree based accurate qgroup reserve API */
int btrfs_qgroup_reserve_data(struct inode *inode,
			struct extent_changeset **reserved, u64 start, u64 len);
int btrfs_qgroup_release_data(struct inode *inode, u64 start, u64 len);
int btrfs_qgroup_free_data(struct inode *inode,
			struct extent_changeset *reserved, u64 start, u64 len);

int __btrfs_qgroup_reserve_meta(struct btrfs_root *root, int num_bytes,
				enum btrfs_qgroup_rsv_type type, bool enforce);
/* Reserve metadata space for pertrans and prealloc type*/
static inline int btrfs_qgroup_reserve_meta_pertrans(struct btrfs_root *root,
				int num_bytes, bool enforce)
{
	return __btrfs_qgroup_reserve_meta(root, num_bytes,
			BTRFS_QGROUP_RSV_META_PERTRANS, enforce);
}
static inline int btrfs_qgroup_reserve_meta_prealloc(struct btrfs_root *root,
				int num_bytes, bool enforce)
{
	return __btrfs_qgroup_reserve_meta(root, num_bytes,
			BTRFS_QGROUP_RSV_META_PREALLOC, enforce);
}

void __btrfs_qgroup_free_meta(struct btrfs_root *root, int num_bytes,
			     enum btrfs_qgroup_rsv_type type);

/* Free per-transaction meta reservation for error handler */
static inline void btrfs_qgroup_free_meta_pertrans(struct btrfs_root *root,
						   int num_bytes)
{
	__btrfs_qgroup_free_meta(root, num_bytes,
			BTRFS_QGROUP_RSV_META_PERTRANS);
}

/* Pre-allocated meta reservation can be freed at need */
static inline void btrfs_qgroup_free_meta_prealloc(struct btrfs_root *root,
						   int num_bytes)
{
	__btrfs_qgroup_free_meta(root, num_bytes,
			BTRFS_QGROUP_RSV_META_PREALLOC);
}

/*
 * Per-transaction meta reservation should be all freed at transaction commit
 * time
 */
void btrfs_qgroup_free_meta_all_pertrans(struct btrfs_root *root);

/*
 * Convert @num_bytes of META_PREALLOCATED reservation to META_PERTRANS.
 *
 * This is called when preallocated meta reservation needs to be used.
 * Normally after btrfs_join_transaction() call.
 */
void btrfs_qgroup_convert_reserved_meta(struct btrfs_root *root, int num_bytes);

void btrfs_qgroup_check_reserved_leak(struct inode *inode);
#endif /* __BTRFS_QGROUP__ */
