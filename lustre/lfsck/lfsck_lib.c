/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License version 2 for more details.  A copy is
 * included in the COPYING file that accompanied this code.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2012, 2013, Intel Corporation.
 */
/*
 * lustre/lfsck/lfsck_lib.c
 *
 * Author: Fan, Yong <fan.yong@intel.com>
 */

#define DEBUG_SUBSYSTEM S_LFSCK

#include <libcfs/list.h>
#include <lu_object.h>
#include <dt_object.h>
#include <md_object.h>
#include <lustre_fld.h>
#include <lustre_lib.h>
#include <lustre_net.h>
#include <lustre_lfsck.h>
#include <lustre/lustre_lfsck_user.h>

#include "lfsck_internal.h"

/* define lfsck thread key */
LU_KEY_INIT(lfsck, struct lfsck_thread_info);

static void lfsck_key_fini(const struct lu_context *ctx,
			   struct lu_context_key *key, void *data)
{
	struct lfsck_thread_info *info = data;

	lu_buf_free(&info->lti_linkea_buf);
	OBD_FREE_PTR(info);
}

LU_CONTEXT_KEY_DEFINE(lfsck, LCT_MD_THREAD | LCT_DT_THREAD);
LU_KEY_INIT_GENERIC(lfsck);

static CFS_LIST_HEAD(lfsck_instance_list);
static struct list_head lfsck_ost_orphan_list;
static struct list_head lfsck_mdt_orphan_list;
static DEFINE_SPINLOCK(lfsck_instance_lock);

static const char *lfsck_status_names[] = {
	[LS_INIT]		= "init",
	[LS_SCANNING_PHASE1]	= "scanning-phase1",
	[LS_SCANNING_PHASE2]	= "scanning-phase2",
	[LS_COMPLETED]		= "completed",
	[LS_FAILED]		= "failed",
	[LS_STOPPED]		= "stopped",
	[LS_PAUSED]		= "paused",
	[LS_CRASHED]		= "crashed",
	[LS_PARTIAL]		= "partial",
	[LS_CO_FAILED]		= "co-failed",
	[LS_CO_STOPPED] 	= "co-stopped",
	[LS_CO_PAUSED]		= "co-paused"
};

const char *lfsck_flags_names[] = {
	"scanned-once",
	"inconsistent",
	"upgrade",
	"incomplete",
	"crashed_lastid",
	NULL
};

const char *lfsck_param_names[] = {
	NULL,
	"failout",
	"dryrun",
	NULL
};

const char *lfsck_status2names(enum lfsck_status status)
{
	if (unlikely(status < 0 || status >= LS_MAX))
		return "unknown";

	return lfsck_status_names[status];
}

static int lfsck_tgt_descs_init(struct lfsck_tgt_descs *ltds)
{
	spin_lock_init(&ltds->ltd_lock);
	init_rwsem(&ltds->ltd_rw_sem);
	INIT_LIST_HEAD(&ltds->ltd_orphan);
	ltds->ltd_tgts_bitmap = CFS_ALLOCATE_BITMAP(BITS_PER_LONG);
	if (ltds->ltd_tgts_bitmap == NULL)
		return -ENOMEM;

	return 0;
}

static void lfsck_tgt_descs_fini(struct lfsck_tgt_descs *ltds)
{
	struct lfsck_tgt_desc	*ltd;
	struct lfsck_tgt_desc	*next;
	int			 idx;

	down_write(&ltds->ltd_rw_sem);

	list_for_each_entry_safe(ltd, next, &ltds->ltd_orphan,
				 ltd_orphan_list) {
		list_del_init(&ltd->ltd_orphan_list);
		lfsck_tgt_put(ltd);
	}

	if (unlikely(ltds->ltd_tgts_bitmap == NULL)) {
		up_write(&ltds->ltd_rw_sem);

		return;
	}

	cfs_foreach_bit(ltds->ltd_tgts_bitmap, idx) {
		ltd = LTD_TGT(ltds, idx);
		if (likely(ltd != NULL)) {
			LASSERT(list_empty(&ltd->ltd_layout_list));

			ltds->ltd_tgtnr--;
			cfs_bitmap_clear(ltds->ltd_tgts_bitmap, idx);
			LTD_TGT(ltds, idx) = NULL;
			lfsck_tgt_put(ltd);
		}
	}

	LASSERTF(ltds->ltd_tgtnr == 0, "tgt count unmatched: %d\n",
		 ltds->ltd_tgtnr);

	for (idx = 0; idx < TGT_PTRS; idx++) {
		if (ltds->ltd_tgts_idx[idx] != NULL) {
			OBD_FREE_PTR(ltds->ltd_tgts_idx[idx]);
			ltds->ltd_tgts_idx[idx] = NULL;
		}
	}

	CFS_FREE_BITMAP(ltds->ltd_tgts_bitmap);
	ltds->ltd_tgts_bitmap = NULL;
	up_write(&ltds->ltd_rw_sem);
}

static int __lfsck_add_target(const struct lu_env *env,
			      struct lfsck_instance *lfsck,
			      struct lfsck_tgt_desc *ltd,
			      bool for_ost, bool locked)
{
	struct lfsck_tgt_descs *ltds;
	__u32			index = ltd->ltd_index;
	int			rc    = 0;
	ENTRY;

	if (for_ost)
		ltds = &lfsck->li_ost_descs;
	else
		ltds = &lfsck->li_mdt_descs;

	if (!locked)
		down_write(&ltds->ltd_rw_sem);

	LASSERT(ltds->ltd_tgts_bitmap != NULL);

	if (index >= ltds->ltd_tgts_bitmap->size) {
		__u32 newsize = max((__u32)ltds->ltd_tgts_bitmap->size,
				    (__u32)BITS_PER_LONG);
		cfs_bitmap_t *old_bitmap = ltds->ltd_tgts_bitmap;
		cfs_bitmap_t *new_bitmap;

		while (newsize < index + 1)
			newsize <<= 1;

		new_bitmap = CFS_ALLOCATE_BITMAP(newsize);
		if (new_bitmap == NULL)
			GOTO(unlock, rc = -ENOMEM);

		if (ltds->ltd_tgtnr > 0)
			cfs_bitmap_copy(new_bitmap, old_bitmap);
		ltds->ltd_tgts_bitmap = new_bitmap;
		CFS_FREE_BITMAP(old_bitmap);
	}

	if (cfs_bitmap_check(ltds->ltd_tgts_bitmap, index)) {
		CERROR("%s: the device %s (%u) is registered already\n",
		       lfsck_lfsck2name(lfsck),
		       ltd->ltd_tgt->dd_lu_dev.ld_obd->obd_name, index);
		GOTO(unlock, rc = -EEXIST);
	}

	if (ltds->ltd_tgts_idx[index / TGT_PTRS_PER_BLOCK] == NULL) {
		OBD_ALLOC_PTR(ltds->ltd_tgts_idx[index / TGT_PTRS_PER_BLOCK]);
		if (ltds->ltd_tgts_idx[index / TGT_PTRS_PER_BLOCK] == NULL)
			GOTO(unlock, rc = -ENOMEM);
	}

	LTD_TGT(ltds, index) = ltd;
	cfs_bitmap_set(ltds->ltd_tgts_bitmap, index);
	ltds->ltd_tgtnr++;

	GOTO(unlock, rc = 0);

unlock:
	if (!locked)
		up_write(&ltds->ltd_rw_sem);

	return rc;
}

static int lfsck_add_target_from_orphan(const struct lu_env *env,
					struct lfsck_instance *lfsck)
{
	struct lfsck_tgt_descs	*ltds    = &lfsck->li_ost_descs;
	struct lfsck_tgt_desc	*ltd;
	struct lfsck_tgt_desc	*next;
	struct list_head	*head    = &lfsck_ost_orphan_list;
	int			 rc;
	bool			 for_ost = true;

again:
	spin_lock(&lfsck_instance_lock);
	list_for_each_entry_safe(ltd, next, head, ltd_orphan_list) {
		if (ltd->ltd_key == lfsck->li_bottom) {
			list_del_init(&ltd->ltd_orphan_list);
			list_add_tail(&ltd->ltd_orphan_list,
				      &ltds->ltd_orphan);
		}
	}
	spin_unlock(&lfsck_instance_lock);

	down_write(&ltds->ltd_rw_sem);
	while (!list_empty(&ltds->ltd_orphan)) {
		ltd = list_entry(ltds->ltd_orphan.next,
				 struct lfsck_tgt_desc,
				 ltd_orphan_list);
		list_del_init(&ltd->ltd_orphan_list);
		rc = __lfsck_add_target(env, lfsck, ltd, for_ost, true);
		/* Do not hold the semaphore for too long time. */
		up_write(&ltds->ltd_rw_sem);
		if (rc != 0)
			return rc;

		down_write(&ltds->ltd_rw_sem);
	}
	up_write(&ltds->ltd_rw_sem);

	if (for_ost) {
		ltds = &lfsck->li_mdt_descs;
		head = &lfsck_mdt_orphan_list;
		for_ost = false;
		goto again;
	}

	return 0;
}

static inline struct lfsck_component *
__lfsck_component_find(struct lfsck_instance *lfsck, __u16 type, cfs_list_t *list)
{
	struct lfsck_component *com;

	cfs_list_for_each_entry(com, list, lc_link) {
		if (com->lc_type == type)
			return com;
	}
	return NULL;
}

static struct lfsck_component *
lfsck_component_find(struct lfsck_instance *lfsck, __u16 type)
{
	struct lfsck_component *com;

	spin_lock(&lfsck->li_lock);
	com = __lfsck_component_find(lfsck, type, &lfsck->li_list_scan);
	if (com != NULL)
		goto unlock;

	com = __lfsck_component_find(lfsck, type,
				     &lfsck->li_list_double_scan);
	if (com != NULL)
		goto unlock;

	com = __lfsck_component_find(lfsck, type, &lfsck->li_list_idle);

unlock:
	if (com != NULL)
		lfsck_component_get(com);
	spin_unlock(&lfsck->li_lock);
	return com;
}

void lfsck_component_cleanup(const struct lu_env *env,
			     struct lfsck_component *com)
{
	if (!cfs_list_empty(&com->lc_link))
		cfs_list_del_init(&com->lc_link);
	if (!cfs_list_empty(&com->lc_link_dir))
		cfs_list_del_init(&com->lc_link_dir);

	lfsck_component_put(env, com);
}

void lfsck_instance_cleanup(const struct lu_env *env,
			    struct lfsck_instance *lfsck)
{
	struct ptlrpc_thread	*thread = &lfsck->li_thread;
	struct lfsck_component	*com;
	ENTRY;

	LASSERT(list_empty(&lfsck->li_link));
	LASSERT(thread_is_init(thread) || thread_is_stopped(thread));

	lfsck_tgt_descs_fini(&lfsck->li_ost_descs);
	lfsck_tgt_descs_fini(&lfsck->li_mdt_descs);

	if (lfsck->li_obj_oit != NULL) {
		lu_object_put_nocache(env, &lfsck->li_obj_oit->do_lu);
		lfsck->li_obj_oit = NULL;
	}

	LASSERT(lfsck->li_obj_dir == NULL);

	while (!cfs_list_empty(&lfsck->li_list_scan)) {
		com = cfs_list_entry(lfsck->li_list_scan.next,
				     struct lfsck_component,
				     lc_link);
		lfsck_component_cleanup(env, com);
	}

	LASSERT(cfs_list_empty(&lfsck->li_list_dir));

	while (!cfs_list_empty(&lfsck->li_list_double_scan)) {
		com = cfs_list_entry(lfsck->li_list_double_scan.next,
				     struct lfsck_component,
				     lc_link);
		lfsck_component_cleanup(env, com);
	}

	while (!cfs_list_empty(&lfsck->li_list_idle)) {
		com = cfs_list_entry(lfsck->li_list_idle.next,
				     struct lfsck_component,
				     lc_link);
		lfsck_component_cleanup(env, com);
	}

	if (lfsck->li_bookmark_obj != NULL) {
		lu_object_put_nocache(env, &lfsck->li_bookmark_obj->do_lu);
		lfsck->li_bookmark_obj = NULL;
	}

	if (lfsck->li_los != NULL) {
		local_oid_storage_fini(env, lfsck->li_los);
		lfsck->li_los = NULL;
	}

	OBD_FREE_PTR(lfsck);
}

static inline struct lfsck_instance *
__lfsck_instance_find(struct dt_device *key, bool ref, bool unlink)
{
	struct lfsck_instance *lfsck;

	cfs_list_for_each_entry(lfsck, &lfsck_instance_list, li_link) {
		if (lfsck->li_bottom == key) {
			if (ref)
				lfsck_instance_get(lfsck);
			if (unlink)
				list_del_init(&lfsck->li_link);

			return lfsck;
		}
	}

	return NULL;
}

static inline struct lfsck_instance *lfsck_instance_find(struct dt_device *key,
							 bool ref, bool unlink)
{
	struct lfsck_instance *lfsck;

	spin_lock(&lfsck_instance_lock);
	lfsck = __lfsck_instance_find(key, ref, unlink);
	spin_unlock(&lfsck_instance_lock);

	return lfsck;
}

static inline int lfsck_instance_add(struct lfsck_instance *lfsck)
{
	struct lfsck_instance *tmp;

	spin_lock(&lfsck_instance_lock);
	cfs_list_for_each_entry(tmp, &lfsck_instance_list, li_link) {
		if (lfsck->li_bottom == tmp->li_bottom) {
			spin_unlock(&lfsck_instance_lock);
			return -EEXIST;
		}
	}

	cfs_list_add_tail(&lfsck->li_link, &lfsck_instance_list);
	spin_unlock(&lfsck_instance_lock);
	return 0;
}

int lfsck_bits_dump(char **buf, int *len, int bits, const char *names[],
		    const char *prefix)
{
	int save = *len;
	int flag;
	int rc;
	int i;

	rc = snprintf(*buf, *len, "%s:%c", prefix, bits != 0 ? ' ' : '\n');
	if (rc <= 0)
		return -ENOSPC;

	*buf += rc;
	*len -= rc;
	for (i = 0, flag = 1; bits != 0; i++, flag = 1 << i) {
		if (flag & bits) {
			bits &= ~flag;
			if (names[i] != NULL) {
				rc = snprintf(*buf, *len, "%s%c", names[i],
					      bits != 0 ? ',' : '\n');
				if (rc <= 0)
					return -ENOSPC;

				*buf += rc;
				*len -= rc;
			}
		}
	}
	return save - *len;
}

int lfsck_time_dump(char **buf, int *len, __u64 time, const char *prefix)
{
	int rc;

	if (time != 0)
		rc = snprintf(*buf, *len, "%s: "LPU64" seconds\n", prefix,
			      cfs_time_current_sec() - time);
	else
		rc = snprintf(*buf, *len, "%s: N/A\n", prefix);
	if (rc <= 0)
		return -ENOSPC;

	*buf += rc;
	*len -= rc;
	return rc;
}

int lfsck_pos_dump(char **buf, int *len, struct lfsck_position *pos,
		   const char *prefix)
{
	int rc;

	if (fid_is_zero(&pos->lp_dir_parent)) {
		if (pos->lp_oit_cookie == 0)
			rc = snprintf(*buf, *len, "%s: N/A, N/A, N/A\n",
				      prefix);
		else
			rc = snprintf(*buf, *len, "%s: "LPU64", N/A, N/A\n",
				      prefix, pos->lp_oit_cookie);
	} else {
		rc = snprintf(*buf, *len, "%s: "LPU64", "DFID", "LPU64"\n",
			      prefix, pos->lp_oit_cookie,
			      PFID(&pos->lp_dir_parent), pos->lp_dir_cookie);
	}
	if (rc <= 0)
		return -ENOSPC;

	*buf += rc;
	*len -= rc;
	return rc;
}

void lfsck_pos_fill(const struct lu_env *env, struct lfsck_instance *lfsck,
		    struct lfsck_position *pos, bool init)
{
	const struct dt_it_ops *iops = &lfsck->li_obj_oit->do_index_ops->dio_it;

	if (unlikely(lfsck->li_di_oit == NULL)) {
		memset(pos, 0, sizeof(*pos));
		return;
	}

	pos->lp_oit_cookie = iops->store(env, lfsck->li_di_oit);
	if (!lfsck->li_current_oit_processed && !init)
		pos->lp_oit_cookie--;

	LASSERT(pos->lp_oit_cookie > 0);

	if (lfsck->li_di_dir != NULL) {
		struct dt_object *dto = lfsck->li_obj_dir;

		pos->lp_dir_cookie = dto->do_index_ops->dio_it.store(env,
							lfsck->li_di_dir);

		if (pos->lp_dir_cookie >= MDS_DIR_END_OFF) {
			fid_zero(&pos->lp_dir_parent);
			pos->lp_dir_cookie = 0;
		} else {
			pos->lp_dir_parent = *lfsck_dto2fid(dto);
		}
	} else {
		fid_zero(&pos->lp_dir_parent);
		pos->lp_dir_cookie = 0;
	}
}

static void __lfsck_set_speed(struct lfsck_instance *lfsck, __u32 limit)
{
	lfsck->li_bookmark_ram.lb_speed_limit = limit;
	if (limit != LFSCK_SPEED_NO_LIMIT) {
		if (limit > HZ) {
			lfsck->li_sleep_rate = limit / HZ;
			lfsck->li_sleep_jif = 1;
		} else {
			lfsck->li_sleep_rate = 1;
			lfsck->li_sleep_jif = HZ / limit;
		}
	} else {
		lfsck->li_sleep_jif = 0;
		lfsck->li_sleep_rate = 0;
	}
}

void lfsck_control_speed(struct lfsck_instance *lfsck)
{
	struct ptlrpc_thread *thread = &lfsck->li_thread;
	struct l_wait_info    lwi;

	if (lfsck->li_sleep_jif > 0 &&
	    lfsck->li_new_scanned >= lfsck->li_sleep_rate) {
		lwi = LWI_TIMEOUT_INTR(lfsck->li_sleep_jif, NULL,
				       LWI_ON_SIGNAL_NOOP, NULL);

		l_wait_event(thread->t_ctl_waitq,
			     !thread_is_running(thread),
			     &lwi);
		lfsck->li_new_scanned = 0;
	}
}

void lfsck_control_speed_by_self(struct lfsck_component *com)
{
	struct lfsck_instance	*lfsck  = com->lc_lfsck;
	struct ptlrpc_thread	*thread = &lfsck->li_thread;
	struct l_wait_info	 lwi;

	if (lfsck->li_sleep_jif > 0 &&
	    com->lc_new_scanned >= lfsck->li_sleep_rate) {
		lwi = LWI_TIMEOUT_INTR(lfsck->li_sleep_jif, NULL,
				       LWI_ON_SIGNAL_NOOP, NULL);

		l_wait_event(thread->t_ctl_waitq,
			     !thread_is_running(thread),
			     &lwi);
		com->lc_new_scanned = 0;
	}
}

static int lfsck_parent_fid(const struct lu_env *env, struct dt_object *obj,
			    struct lu_fid *fid)
{
	if (unlikely(!S_ISDIR(lfsck_object_type(obj)) ||
		     !dt_try_as_dir(env, obj)))
		return -ENOTDIR;

	return dt_lookup(env, obj, (struct dt_rec *)fid,
			 (const struct dt_key *)"..", BYPASS_CAPA);
}

static int lfsck_needs_scan_dir(const struct lu_env *env,
				struct lfsck_instance *lfsck,
				struct dt_object *obj)
{
	struct lu_fid *fid   = &lfsck_env_info(env)->lti_fid;
	int	       depth = 0;
	int	       rc;

	if (!lfsck->li_master || !S_ISDIR(lfsck_object_type(obj)) ||
	    cfs_list_empty(&lfsck->li_list_dir))
	       RETURN(0);

	while (1) {
		/* XXX: Currently, we do not scan the "/REMOTE_PARENT_DIR",
		 *	which is the agent directory to manage the objects
		 *	which name entries reside on remote MDTs. Related
		 *	consistency verification will be processed in LFSCK
		 *	phase III. */
		if (lu_fid_eq(lfsck_dto2fid(obj), &lfsck->li_global_root_fid)) {
			if (depth > 0)
				lfsck_object_put(env, obj);
			return 1;
		}

		/* .lustre doesn't contain "real" user objects, no need lfsck */
		if (fid_is_dot_lustre(lfsck_dto2fid(obj))) {
			if (depth > 0)
				lfsck_object_put(env, obj);
			return 0;
		}

		dt_read_lock(env, obj, MOR_TGT_CHILD);
		if (unlikely(lfsck_is_dead_obj(obj))) {
			dt_read_unlock(env, obj);
			if (depth > 0)
				lfsck_object_put(env, obj);
			return 0;
		}

		rc = dt_xattr_get(env, obj,
				  lfsck_buf_get(env, NULL, 0), XATTR_NAME_LINK,
				  BYPASS_CAPA);
		dt_read_unlock(env, obj);
		if (rc >= 0) {
			if (depth > 0)
				lfsck_object_put(env, obj);
			return 1;
		}

		if (rc < 0 && rc != -ENODATA) {
			if (depth > 0)
				lfsck_object_put(env, obj);
			return rc;
		}

		rc = lfsck_parent_fid(env, obj, fid);
		if (depth > 0)
			lfsck_object_put(env, obj);
		if (rc != 0)
			return rc;

		if (unlikely(lu_fid_eq(fid, &lfsck->li_local_root_fid)))
			return 0;

		obj = lfsck_object_find(env, lfsck, fid);
		if (obj == NULL)
			return 0;
		else if (IS_ERR(obj))
			return PTR_ERR(obj);

		if (!dt_object_exists(obj)) {
			lfsck_object_put(env, obj);
			return 0;
		}

		/* Currently, only client visible directory can be remote. */
		if (dt_object_remote(obj)) {
			lfsck_object_put(env, obj);
			return 1;
		}

		depth++;
	}
	return 0;
}

struct lfsck_thread_args *lfsck_thread_args_init(struct lfsck_instance *lfsck,
						 struct lfsck_component *com)
{
	struct lfsck_thread_args *lta;
	int			  rc;

	OBD_ALLOC_PTR(lta);
	if (lta == NULL)
		return ERR_PTR(-ENOMEM);

	rc = lu_env_init(&lta->lta_env, LCT_MD_THREAD | LCT_DT_THREAD);
	if (rc != 0) {
		OBD_FREE_PTR(lta);
		return ERR_PTR(rc);
	}

	lta->lta_lfsck = lfsck_instance_get(lfsck);
	if (com != NULL)
		lta->lta_com = lfsck_component_get(com);

	return lta;
}

void lfsck_thread_args_fini(struct lfsck_thread_args *lta)
{
	if (lta->lta_com != NULL)
		lfsck_component_put(&lta->lta_env, lta->lta_com);
	lfsck_instance_put(&lta->lta_env, lta->lta_lfsck);
	lu_env_fini(&lta->lta_env);
	OBD_FREE_PTR(lta);
}

/* LFSCK wrap functions */

void lfsck_fail(const struct lu_env *env, struct lfsck_instance *lfsck,
		bool new_checked)
{
	struct lfsck_component *com;

	cfs_list_for_each_entry(com, &lfsck->li_list_scan, lc_link) {
		com->lc_ops->lfsck_fail(env, com, new_checked);
	}
}

int lfsck_checkpoint(const struct lu_env *env, struct lfsck_instance *lfsck)
{
	struct lfsck_component *com;
	int			rc  = 0;
	int			rc1 = 0;

	if (likely(cfs_time_beforeq(cfs_time_current(),
				    lfsck->li_time_next_checkpoint)))
		return 0;

	lfsck_pos_fill(env, lfsck, &lfsck->li_pos_current, false);
	cfs_list_for_each_entry(com, &lfsck->li_list_scan, lc_link) {
		rc = com->lc_ops->lfsck_checkpoint(env, com, false);
		if (rc != 0)
			rc1 = rc;
	}

	lfsck->li_time_last_checkpoint = cfs_time_current();
	lfsck->li_time_next_checkpoint = lfsck->li_time_last_checkpoint +
				cfs_time_seconds(LFSCK_CHECKPOINT_INTERVAL);
	return rc1 != 0 ? rc1 : rc;
}

int lfsck_prep(const struct lu_env *env, struct lfsck_instance *lfsck)
{
	struct dt_object       *obj	= NULL;
	struct lfsck_component *com;
	struct lfsck_component *next;
	struct lfsck_position  *pos	= NULL;
	const struct dt_it_ops *iops	=
				&lfsck->li_obj_oit->do_index_ops->dio_it;
	struct dt_it	       *di;
	int			rc;
	ENTRY;

	LASSERT(lfsck->li_obj_dir == NULL);
	LASSERT(lfsck->li_di_dir == NULL);

	lfsck->li_current_oit_processed = 0;
	cfs_list_for_each_entry_safe(com, next, &lfsck->li_list_scan, lc_link) {
		com->lc_new_checked = 0;
		if (lfsck->li_bookmark_ram.lb_param & LPF_DRYRUN)
			com->lc_journal = 0;

		rc = com->lc_ops->lfsck_prep(env, com);
		if (rc != 0)
			GOTO(out, rc);

		if ((pos == NULL) ||
		    (!lfsck_pos_is_zero(&com->lc_pos_start) &&
		     lfsck_pos_is_eq(pos, &com->lc_pos_start) > 0))
			pos = &com->lc_pos_start;
	}

	/* Init otable-based iterator. */
	if (pos == NULL) {
		rc = iops->load(env, lfsck->li_di_oit, 0);
		if (rc > 0) {
			lfsck->li_oit_over = 1;
			rc = 0;
		}

		GOTO(out, rc);
	}

	rc = iops->load(env, lfsck->li_di_oit, pos->lp_oit_cookie);
	if (rc < 0)
		GOTO(out, rc);
	else if (rc > 0)
		lfsck->li_oit_over = 1;

	if (!lfsck->li_master || fid_is_zero(&pos->lp_dir_parent))
		GOTO(out, rc = 0);

	/* Find the directory for namespace-based traverse. */
	obj = lfsck_object_find(env, lfsck, &pos->lp_dir_parent);
	if (obj == NULL)
		GOTO(out, rc = 0);
	else if (IS_ERR(obj))
		RETURN(PTR_ERR(obj));

	/* XXX: Currently, skip remote object, the consistency for
	 *	remote object will be processed in LFSCK phase III. */
	if (!dt_object_exists(obj) || dt_object_remote(obj) ||
	    unlikely(!S_ISDIR(lfsck_object_type(obj))))
		GOTO(out, rc = 0);

	if (unlikely(!dt_try_as_dir(env, obj)))
		GOTO(out, rc = -ENOTDIR);

	/* Init the namespace-based directory traverse. */
	iops = &obj->do_index_ops->dio_it;
	di = iops->init(env, obj, lfsck->li_args_dir, BYPASS_CAPA);
	if (IS_ERR(di))
		GOTO(out, rc = PTR_ERR(di));

	LASSERT(pos->lp_dir_cookie < MDS_DIR_END_OFF);

	rc = iops->load(env, di, pos->lp_dir_cookie);
	if ((rc == 0) || (rc > 0 && pos->lp_dir_cookie > 0))
		rc = iops->next(env, di);
	else if (rc > 0)
		rc = 0;

	if (rc != 0) {
		iops->put(env, di);
		iops->fini(env, di);
		GOTO(out, rc);
	}

	lfsck->li_obj_dir = lfsck_object_get(obj);
	lfsck->li_cookie_dir = iops->store(env, di);
	spin_lock(&lfsck->li_lock);
	lfsck->li_di_dir = di;
	spin_unlock(&lfsck->li_lock);

	GOTO(out, rc = 0);

out:
	if (obj != NULL)
		lfsck_object_put(env, obj);

	if (rc < 0) {
		cfs_list_for_each_entry_safe(com, next, &lfsck->li_list_scan,
					     lc_link)
			com->lc_ops->lfsck_post(env, com, rc, true);

		return rc;
	}

	rc = 0;
	lfsck_pos_fill(env, lfsck, &lfsck->li_pos_current, true);
	cfs_list_for_each_entry(com, &lfsck->li_list_scan, lc_link) {
		rc = com->lc_ops->lfsck_checkpoint(env, com, true);
		if (rc != 0)
			break;
	}

	lfsck->li_time_last_checkpoint = cfs_time_current();
	lfsck->li_time_next_checkpoint = lfsck->li_time_last_checkpoint +
				cfs_time_seconds(LFSCK_CHECKPOINT_INTERVAL);
	return rc;
}

int lfsck_exec_oit(const struct lu_env *env, struct lfsck_instance *lfsck,
		   struct dt_object *obj)
{
	struct lfsck_component *com;
	const struct dt_it_ops *iops;
	struct dt_it	       *di;
	int			rc;
	ENTRY;

	LASSERT(lfsck->li_obj_dir == NULL);

	cfs_list_for_each_entry(com, &lfsck->li_list_scan, lc_link) {
		rc = com->lc_ops->lfsck_exec_oit(env, com, obj);
		if (rc != 0)
			RETURN(rc);
	}

	rc = lfsck_needs_scan_dir(env, lfsck, obj);
	if (rc <= 0)
		GOTO(out, rc);

	if (unlikely(!dt_try_as_dir(env, obj)))
		GOTO(out, rc = -ENOTDIR);

	iops = &obj->do_index_ops->dio_it;
	di = iops->init(env, obj, lfsck->li_args_dir, BYPASS_CAPA);
	if (IS_ERR(di))
		GOTO(out, rc = PTR_ERR(di));

	rc = iops->load(env, di, 0);
	if (rc == 0)
		rc = iops->next(env, di);
	else if (rc > 0)
		rc = 0;

	if (rc != 0) {
		iops->put(env, di);
		iops->fini(env, di);
		GOTO(out, rc);
	}

	lfsck->li_obj_dir = lfsck_object_get(obj);
	lfsck->li_cookie_dir = iops->store(env, di);
	spin_lock(&lfsck->li_lock);
	lfsck->li_di_dir = di;
	spin_unlock(&lfsck->li_lock);

	GOTO(out, rc = 0);

out:
	if (rc < 0)
		lfsck_fail(env, lfsck, false);
	return (rc > 0 ? 0 : rc);
}

int lfsck_exec_dir(const struct lu_env *env, struct lfsck_instance *lfsck,
		   struct dt_object *obj, struct lu_dirent *ent)
{
	struct lfsck_component *com;
	int			rc;

	cfs_list_for_each_entry(com, &lfsck->li_list_scan, lc_link) {
		rc = com->lc_ops->lfsck_exec_dir(env, com, obj, ent);
		if (rc != 0)
			return rc;
	}
	return 0;
}

int lfsck_post(const struct lu_env *env, struct lfsck_instance *lfsck,
	       int result)
{
	struct lfsck_component *com;
	struct lfsck_component *next;
	int			rc  = 0;
	int			rc1 = 0;

	lfsck_pos_fill(env, lfsck, &lfsck->li_pos_current, false);
	cfs_list_for_each_entry_safe(com, next, &lfsck->li_list_scan, lc_link) {
		rc = com->lc_ops->lfsck_post(env, com, result, false);
		if (rc != 0)
			rc1 = rc;
	}

	lfsck->li_time_last_checkpoint = cfs_time_current();
	lfsck->li_time_next_checkpoint = lfsck->li_time_last_checkpoint +
				cfs_time_seconds(LFSCK_CHECKPOINT_INTERVAL);

	/* Ignore some component post failure to make other can go ahead. */
	return result;
}

int lfsck_double_scan(const struct lu_env *env, struct lfsck_instance *lfsck)
{
	struct lfsck_component *com;
	struct lfsck_component *next;
	struct l_wait_info	lwi = { 0 };
	int			rc  = 0;
	int			rc1 = 0;

	cfs_list_for_each_entry_safe(com, next, &lfsck->li_list_double_scan,
				     lc_link) {
		if (lfsck->li_bookmark_ram.lb_param & LPF_DRYRUN)
			com->lc_journal = 0;

		rc = com->lc_ops->lfsck_double_scan(env, com);
		if (rc != 0)
			rc1 = rc;
	}

	l_wait_event(lfsck->li_thread.t_ctl_waitq,
		     atomic_read(&lfsck->li_double_scan_count) == 0,
		     &lwi);

	return (rc1 != 0 ? rc1 : rc);
}

void lfsck_quit(const struct lu_env *env, struct lfsck_instance *lfsck)
{
	struct lfsck_component *com;
	struct lfsck_component *next;

	list_for_each_entry_safe(com, next, &lfsck->li_list_scan,
				 lc_link) {
		if (com->lc_ops->lfsck_quit != NULL)
			com->lc_ops->lfsck_quit(env, com);
	}

	list_for_each_entry_safe(com, next, &lfsck->li_list_double_scan,
				 lc_link) {
		if (com->lc_ops->lfsck_quit != NULL)
			com->lc_ops->lfsck_quit(env, com);
	}
}

/* external interfaces */

int lfsck_get_speed(struct dt_device *key, void *buf, int len)
{
	struct lu_env		env;
	struct lfsck_instance  *lfsck;
	int			rc;
	ENTRY;

	rc = lu_env_init(&env, LCT_MD_THREAD | LCT_DT_THREAD);
	if (rc != 0)
		RETURN(rc);

	lfsck = lfsck_instance_find(key, true, false);
	if (likely(lfsck != NULL)) {
		rc = snprintf(buf, len, "%u\n",
			      lfsck->li_bookmark_ram.lb_speed_limit);
		lfsck_instance_put(&env, lfsck);
	} else {
		rc = -ENODEV;
	}

	lu_env_fini(&env);

	RETURN(rc);
}
EXPORT_SYMBOL(lfsck_get_speed);

int lfsck_set_speed(struct dt_device *key, int val)
{
	struct lu_env		env;
	struct lfsck_instance  *lfsck;
	int			rc;
	ENTRY;

	rc = lu_env_init(&env, LCT_MD_THREAD | LCT_DT_THREAD);
	if (rc != 0)
		RETURN(rc);

	lfsck = lfsck_instance_find(key, true, false);
	if (likely(lfsck != NULL)) {
		mutex_lock(&lfsck->li_mutex);
		__lfsck_set_speed(lfsck, val);
		rc = lfsck_bookmark_store(&env, lfsck);
		mutex_unlock(&lfsck->li_mutex);
		lfsck_instance_put(&env, lfsck);
	} else {
		rc = -ENODEV;
	}

	lu_env_fini(&env);

	RETURN(rc);
}
EXPORT_SYMBOL(lfsck_set_speed);

int lfsck_get_windows(struct dt_device *key, void *buf, int len)
{
	struct lu_env		env;
	struct lfsck_instance  *lfsck;
	int			rc;
	ENTRY;

	rc = lu_env_init(&env, LCT_MD_THREAD | LCT_DT_THREAD);
	if (rc != 0)
		RETURN(rc);

	lfsck = lfsck_instance_find(key, true, false);
	if (likely(lfsck != NULL)) {
		rc = snprintf(buf, len, "%u\n",
			      lfsck->li_bookmark_ram.lb_async_windows);
		lfsck_instance_put(&env, lfsck);
	} else {
		rc = -ENODEV;
	}

	lu_env_fini(&env);

	RETURN(rc);
}
EXPORT_SYMBOL(lfsck_get_windows);

int lfsck_set_windows(struct dt_device *key, int val)
{
	struct lu_env		env;
	struct lfsck_instance  *lfsck;
	int			rc;
	ENTRY;

	rc = lu_env_init(&env, LCT_MD_THREAD | LCT_DT_THREAD);
	if (rc != 0)
		RETURN(rc);

	lfsck = lfsck_instance_find(key, true, false);
	if (likely(lfsck != NULL)) {
		if (val > LFSCK_ASYNC_WIN_MAX) {
			CERROR("%s: Too large async windows size, which "
			       "may cause memory issues. The valid range "
			       "is [0 - %u]. If you do not want to restrict "
			       "the windows size for async requests pipeline, "
			       "just set it as 0.\n",
			       lfsck_lfsck2name(lfsck), LFSCK_ASYNC_WIN_MAX);
			rc = -EINVAL;
		} else if (lfsck->li_bookmark_ram.lb_async_windows != val) {
			mutex_lock(&lfsck->li_mutex);
			lfsck->li_bookmark_ram.lb_async_windows = val;
			rc = lfsck_bookmark_store(&env, lfsck);
			mutex_unlock(&lfsck->li_mutex);
		}
		lfsck_instance_put(&env, lfsck);
	} else {
		rc = -ENODEV;
	}

	lu_env_fini(&env);

	RETURN(rc);
}
EXPORT_SYMBOL(lfsck_set_windows);

int lfsck_dump(struct dt_device *key, void *buf, int len, enum lfsck_type type)
{
	struct lu_env		env;
	struct lfsck_instance  *lfsck;
	struct lfsck_component *com;
	int			rc;
	ENTRY;

	rc = lu_env_init(&env, LCT_MD_THREAD | LCT_DT_THREAD);
	if (rc != 0)
		RETURN(rc);

	lfsck = lfsck_instance_find(key, true, false);
	if (likely(lfsck != NULL)) {
		com = lfsck_component_find(lfsck, type);
		if (likely(com != NULL)) {
			rc = com->lc_ops->lfsck_dump(&env, com, buf, len);
			lfsck_component_put(&env, com);
		} else {
			rc = -ENOTSUPP;
		}

		lfsck_instance_put(&env, lfsck);
	} else {
		rc = -ENODEV;
	}

	lu_env_fini(&env);

	RETURN(rc);
}
EXPORT_SYMBOL(lfsck_dump);

int lfsck_start(const struct lu_env *env, struct dt_device *key,
		struct lfsck_start_param *lsp)
{
	struct lfsck_start		*start  = lsp->lsp_start;
	struct lfsck_instance		*lfsck;
	struct lfsck_bookmark		*bk;
	struct ptlrpc_thread		*thread;
	struct lfsck_component		*com;
	struct l_wait_info		 lwi    = { 0 };
	struct lfsck_thread_args	*lta;
	bool				 dirty  = false;
	long				 rc     = 0;
	__u16				 valid  = 0;
	__u16				 flags  = 0;
	__u16				 type   = 1;
	ENTRY;

	lfsck = lfsck_instance_find(key, true, false);
	if (unlikely(lfsck == NULL))
		RETURN(-ENODEV);

	/* start == NULL means auto trigger paused LFSCK. */
	if ((start == NULL) &&
	    (cfs_list_empty(&lfsck->li_list_scan) ||
	     OBD_FAIL_CHECK(OBD_FAIL_LFSCK_NO_AUTO)))
		GOTO(put, rc = 0);

	bk = &lfsck->li_bookmark_ram;
	thread = &lfsck->li_thread;
	mutex_lock(&lfsck->li_mutex);
	spin_lock(&lfsck->li_lock);
	if (!thread_is_init(thread) && !thread_is_stopped(thread)) {
		rc = -EALREADY;
		while (start->ls_active != 0) {
			if (type & start->ls_active) {
				com = __lfsck_component_find(lfsck, type,
							&lfsck->li_list_scan);
				if (com == NULL)
					com = __lfsck_component_find(lfsck,
						type,
						&lfsck->li_list_double_scan);
				if (com == NULL) {
					rc = -EBUSY;
					break;
				} else {
					start->ls_active &= ~type;
				}
			}
			type <<= 1;
		}
		spin_unlock(&lfsck->li_lock);
		GOTO(out, rc);
	}
	spin_unlock(&lfsck->li_lock);

	lfsck->li_namespace = lsp->lsp_namespace;
	lfsck->li_status = 0;
	lfsck->li_oit_over = 0;
	lfsck->li_drop_dryrun = 0;
	lfsck->li_new_scanned = 0;

	/* For auto trigger. */
	if (start == NULL)
		goto trigger;

	start->ls_version = bk->lb_version;
	if (start->ls_valid & LSV_SPEED_LIMIT) {
		__lfsck_set_speed(lfsck, start->ls_speed_limit);
		dirty = true;
	}

	if (start->ls_valid & LSV_ASYNC_WINDOWS &&
	    bk->lb_async_windows != start->ls_async_windows) {
		bk->lb_async_windows = start->ls_async_windows;
		dirty = true;
	}

	if (start->ls_valid & LSV_ERROR_HANDLE) {
		valid |= DOIV_ERROR_HANDLE;
		if (start->ls_flags & LPF_FAILOUT)
			flags |= DOIF_FAILOUT;

		if ((start->ls_flags & LPF_FAILOUT) &&
		    !(bk->lb_param & LPF_FAILOUT)) {
			bk->lb_param |= LPF_FAILOUT;
			dirty = true;
		} else if (!(start->ls_flags & LPF_FAILOUT) &&
			   (bk->lb_param & LPF_FAILOUT)) {
			bk->lb_param &= ~LPF_FAILOUT;
			dirty = true;
		}
	}

	if (start->ls_valid & LSV_DRYRUN) {
		valid |= DOIV_DRYRUN;
		if (start->ls_flags & LPF_DRYRUN)
			flags |= DOIF_DRYRUN;

		if ((start->ls_flags & LPF_DRYRUN) &&
		    !(bk->lb_param & LPF_DRYRUN)) {
			bk->lb_param |= LPF_DRYRUN;
			dirty = true;
		} else if (!(start->ls_flags & LPF_DRYRUN) &&
			   (bk->lb_param & LPF_DRYRUN)) {
			bk->lb_param &= ~LPF_DRYRUN;
			lfsck->li_drop_dryrun = 1;
			dirty = true;
		}
	}

	if (dirty) {
		rc = lfsck_bookmark_store(env, lfsck);
		if (rc != 0)
			GOTO(out, rc);
	}

	if (start->ls_flags & LPF_RESET)
		flags |= DOIF_RESET;

	if (start->ls_active != 0) {
		struct lfsck_component *next;

		if (start->ls_active == LFSCK_TYPES_ALL)
			start->ls_active = LFSCK_TYPES_SUPPORTED;

		if (start->ls_active & ~LFSCK_TYPES_SUPPORTED) {
			start->ls_active &= ~LFSCK_TYPES_SUPPORTED;
			GOTO(out, rc = -ENOTSUPP);
		}

		cfs_list_for_each_entry_safe(com, next,
					     &lfsck->li_list_scan, lc_link) {
			if (!(com->lc_type & start->ls_active)) {
				rc = com->lc_ops->lfsck_post(env, com, 0,
							     false);
				if (rc != 0)
					GOTO(out, rc);
			}
		}

		while (start->ls_active != 0) {
			if (type & start->ls_active) {
				com = __lfsck_component_find(lfsck, type,
							&lfsck->li_list_idle);
				if (com != NULL) {
					/* The component status will be updated
					 * when its prep() is called later by
					 * the LFSCK main engine. */
					cfs_list_del_init(&com->lc_link);
					cfs_list_add_tail(&com->lc_link,
							  &lfsck->li_list_scan);
				}
				start->ls_active &= ~type;
			}
			type <<= 1;
		}
	}

	cfs_list_for_each_entry(com, &lfsck->li_list_scan, lc_link) {
		start->ls_active |= com->lc_type;
		if (flags & DOIF_RESET) {
			rc = com->lc_ops->lfsck_reset(env, com, false);
			if (rc != 0)
				GOTO(out, rc);
		}
	}

trigger:
	lfsck->li_args_dir = LUDA_64BITHASH | LUDA_VERIFY;
	if (bk->lb_param & LPF_DRYRUN) {
		lfsck->li_args_dir |= LUDA_VERIFY_DRYRUN;
		valid |= DOIV_DRYRUN;
		flags |= DOIF_DRYRUN;
	}

	if (bk->lb_param & LPF_FAILOUT) {
		valid |= DOIV_ERROR_HANDLE;
		flags |= DOIF_FAILOUT;
	}

	if (!cfs_list_empty(&lfsck->li_list_scan))
		flags |= DOIF_OUTUSED;

	lfsck->li_args_oit = (flags << DT_OTABLE_IT_FLAGS_SHIFT) | valid;
	thread_set_flags(thread, 0);
	lta = lfsck_thread_args_init(lfsck, NULL);
	if (IS_ERR(lta))
		GOTO(out, rc = PTR_ERR(lta));

	rc = PTR_ERR(kthread_run(lfsck_master_engine, lta, "lfsck"));
	if (IS_ERR_VALUE(rc)) {
		CERROR("%s: cannot start LFSCK thread: rc = %ld\n",
		       lfsck_lfsck2name(lfsck), rc);
		lfsck_thread_args_fini(lta);
	} else {
		rc = 0;
		l_wait_event(thread->t_ctl_waitq,
			     thread_is_running(thread) ||
			     thread_is_stopped(thread),
			     &lwi);
	}

	GOTO(out, rc);

out:
	mutex_unlock(&lfsck->li_mutex);
put:
	lfsck_instance_put(env, lfsck);
	return (rc < 0 ? rc : 0);
}
EXPORT_SYMBOL(lfsck_start);

int lfsck_stop(const struct lu_env *env, struct dt_device *key,
	       struct lfsck_stop *stop)
{
	struct lfsck_instance	*lfsck;
	struct ptlrpc_thread	*thread;
	struct l_wait_info	 lwi    = { 0 };
	int			 rc	= 0;
	ENTRY;

	lfsck = lfsck_instance_find(key, true, false);
	if (unlikely(lfsck == NULL))
		RETURN(-ENODEV);

	thread = &lfsck->li_thread;
	mutex_lock(&lfsck->li_mutex);
	spin_lock(&lfsck->li_lock);
	if (thread_is_init(thread) || thread_is_stopped(thread)) {
		spin_unlock(&lfsck->li_lock);
		GOTO(out, rc = -EALREADY);
	}

	if (stop != NULL)
		lfsck->li_status = stop->ls_status;
	else
		lfsck->li_status = LS_STOPPED;

	thread_set_flags(thread, SVC_STOPPING);
	spin_unlock(&lfsck->li_lock);

	wake_up_all(&thread->t_ctl_waitq);
	l_wait_event(thread->t_ctl_waitq,
		     thread_is_stopped(thread),
		     &lwi);

	GOTO(out, rc = 0);

out:
	mutex_unlock(&lfsck->li_mutex);
	lfsck_instance_put(env, lfsck);

	return rc;
}
EXPORT_SYMBOL(lfsck_stop);

int lfsck_in_notify(const struct lu_env *env, struct dt_device *key,
		    struct lfsck_request *lr)
{
	struct lfsck_instance  *lfsck;
	struct lfsck_component *com;
	int			rc;
	ENTRY;

	switch (lr->lr_event) {
	case LE_STOP:
	case LE_PHASE1_DONE:
	case LE_PHASE2_DONE:
		break;
	default:
		RETURN(-EOPNOTSUPP);
	}

	lfsck = lfsck_instance_find(key, true, false);
	if (unlikely(lfsck == NULL))
		RETURN(-ENODEV);

	com = lfsck_component_find(lfsck, lr->lr_active);
	if (likely(com != NULL)) {
		rc = com->lc_ops->lfsck_in_notify(env, com, lr);
		lfsck_component_put(env, com);
	} else {
		rc = -ENOTSUPP;
	}

	lfsck_instance_put(env, lfsck);

	RETURN(rc);
}
EXPORT_SYMBOL(lfsck_in_notify);

int lfsck_query(const struct lu_env *env, struct dt_device *key,
		struct lfsck_request *lr)
{
	struct lfsck_instance  *lfsck;
	struct lfsck_component *com;
	int			rc;
	ENTRY;

	lfsck = lfsck_instance_find(key, true, false);
	if (unlikely(lfsck == NULL))
		RETURN(-ENODEV);

	com = lfsck_component_find(lfsck, lr->lr_active);
	if (likely(com != NULL)) {
		rc = com->lc_ops->lfsck_query(env, com);
		lfsck_component_put(env, com);
	} else {
		rc = -ENOTSUPP;
	}

	lfsck_instance_put(env, lfsck);

	RETURN(rc);
}
EXPORT_SYMBOL(lfsck_query);

int lfsck_register(const struct lu_env *env, struct dt_device *key,
		   struct dt_device *next, lfsck_out_notify notify,
		   void *notify_data, bool master)
{
	struct lfsck_instance	*lfsck;
	struct dt_object	*root  = NULL;
	struct dt_object	*obj;
	struct lu_fid		*fid   = &lfsck_env_info(env)->lti_fid;
	int			 rc;
	ENTRY;

	lfsck = lfsck_instance_find(key, false, false);
	if (unlikely(lfsck != NULL))
		RETURN(-EEXIST);

	OBD_ALLOC_PTR(lfsck);
	if (lfsck == NULL)
		RETURN(-ENOMEM);

	mutex_init(&lfsck->li_mutex);
	spin_lock_init(&lfsck->li_lock);
	CFS_INIT_LIST_HEAD(&lfsck->li_link);
	CFS_INIT_LIST_HEAD(&lfsck->li_list_scan);
	CFS_INIT_LIST_HEAD(&lfsck->li_list_dir);
	CFS_INIT_LIST_HEAD(&lfsck->li_list_double_scan);
	CFS_INIT_LIST_HEAD(&lfsck->li_list_idle);
	atomic_set(&lfsck->li_ref, 1);
	atomic_set(&lfsck->li_double_scan_count, 0);
	init_waitqueue_head(&lfsck->li_thread.t_ctl_waitq);
	lfsck->li_out_notify = notify;
	lfsck->li_out_notify_data = notify_data;
	lfsck->li_next = next;
	lfsck->li_bottom = key;

	rc = lfsck_tgt_descs_init(&lfsck->li_ost_descs);
	if (rc != 0)
		GOTO(out, rc);

	rc = lfsck_tgt_descs_init(&lfsck->li_mdt_descs);
	if (rc != 0)
		GOTO(out, rc);

	fid->f_seq = FID_SEQ_LOCAL_NAME;
	fid->f_oid = 1;
	fid->f_ver = 0;
	rc = local_oid_storage_init(env, lfsck->li_bottom, fid, &lfsck->li_los);
	if (rc != 0)
		GOTO(out, rc);

	rc = dt_root_get(env, key, fid);
	if (rc != 0)
		GOTO(out, rc);

	root = dt_locate(env, lfsck->li_bottom, fid);
	if (IS_ERR(root))
		GOTO(out, rc = PTR_ERR(root));

	if (unlikely(!dt_try_as_dir(env, root)))
		GOTO(out, rc = -ENOTDIR);

	lfsck->li_local_root_fid = *fid;
	if (master) {
		lfsck->li_master = 1;
		if (lfsck_dev_idx(lfsck->li_bottom) == 0) {
			rc = dt_lookup(env, root,
				(struct dt_rec *)(&lfsck->li_global_root_fid),
				(const struct dt_key *)"ROOT", BYPASS_CAPA);
			if (rc != 0)
				GOTO(out, rc);
		}
	}

	fid->f_seq = FID_SEQ_LOCAL_FILE;
	fid->f_oid = OTABLE_IT_OID;
	fid->f_ver = 0;
	obj = dt_locate(env, lfsck->li_bottom, fid);
	if (IS_ERR(obj))
		GOTO(out, rc = PTR_ERR(obj));

	lfsck->li_obj_oit = obj;
	rc = obj->do_ops->do_index_try(env, obj, &dt_otable_features);
	if (rc != 0) {
		if (rc == -ENOTSUPP)
			GOTO(add, rc = 0);

		GOTO(out, rc);
	}

	rc = lfsck_bookmark_setup(env, lfsck);
	if (rc != 0)
		GOTO(out, rc);

	if (master) {
		rc = lfsck_namespace_setup(env, lfsck);
		if (rc < 0)
			GOTO(out, rc);
	}

	rc = lfsck_layout_setup(env, lfsck);
	if (rc < 0)
		GOTO(out, rc);

	/* XXX: more LFSCK components initialization to be added here. */

add:
	rc = lfsck_instance_add(lfsck);
	if (rc == 0)
		rc = lfsck_add_target_from_orphan(env, lfsck);
out:
	if (root != NULL && !IS_ERR(root))
		lu_object_put(env, &root->do_lu);
	if (rc != 0)
		lfsck_instance_cleanup(env, lfsck);
	return rc;
}
EXPORT_SYMBOL(lfsck_register);

void lfsck_degister(const struct lu_env *env, struct dt_device *key)
{
	struct lfsck_instance *lfsck;

	lfsck = lfsck_instance_find(key, false, true);
	if (lfsck != NULL)
		lfsck_instance_put(env, lfsck);
}
EXPORT_SYMBOL(lfsck_degister);

int lfsck_add_target(const struct lu_env *env, struct dt_device *key,
		     struct dt_device *tgt, struct obd_export *exp,
		     __u32 index, bool for_ost)
{
	struct lfsck_instance	*lfsck;
	struct lfsck_tgt_desc	*ltd;
	int			 rc;
	ENTRY;

	OBD_ALLOC_PTR(ltd);
	if (ltd == NULL)
		RETURN(-ENOMEM);

	ltd->ltd_tgt = tgt;
	ltd->ltd_key = key;
	ltd->ltd_exp = exp;
	INIT_LIST_HEAD(&ltd->ltd_orphan_list);
	INIT_LIST_HEAD(&ltd->ltd_layout_list);
	atomic_set(&ltd->ltd_ref, 1);
	ltd->ltd_index = index;

	spin_lock(&lfsck_instance_lock);
	lfsck = __lfsck_instance_find(key, true, false);
	if (lfsck == NULL) {
		if (for_ost)
			list_add_tail(&ltd->ltd_orphan_list,
				      &lfsck_ost_orphan_list);
		else
			list_add_tail(&ltd->ltd_orphan_list,
				      &lfsck_mdt_orphan_list);
		spin_unlock(&lfsck_instance_lock);

		RETURN(0);
	}
	spin_unlock(&lfsck_instance_lock);

	rc = __lfsck_add_target(env, lfsck, ltd, for_ost, false);
	if (rc != 0)
		lfsck_tgt_put(ltd);

	lfsck_instance_put(env, lfsck);

	RETURN(rc);
}
EXPORT_SYMBOL(lfsck_add_target);

void lfsck_del_target(const struct lu_env *env, struct dt_device *key,
		      struct dt_device *tgt, __u32 index, bool for_ost)
{
	struct lfsck_instance	*lfsck;
	struct lfsck_tgt_descs	*ltds;
	struct lfsck_tgt_desc	*ltd;
	struct list_head	*head;
	bool			 found = false;

	if (for_ost)
		head = &lfsck_ost_orphan_list;
	else
		head = &lfsck_mdt_orphan_list;

	spin_lock(&lfsck_instance_lock);
	list_for_each_entry(ltd, head, ltd_orphan_list) {
		if (ltd->ltd_tgt == tgt) {
			list_del_init(&ltd->ltd_orphan_list);
			spin_unlock(&lfsck_instance_lock);
			lfsck_tgt_put(ltd);

			return;
		}
	}

	lfsck = __lfsck_instance_find(key, true, false);
	spin_unlock(&lfsck_instance_lock);
	if (unlikely(lfsck == NULL))
		return;

	if (for_ost)
		ltds = &lfsck->li_ost_descs;
	else
		ltds = &lfsck->li_mdt_descs;

	down_write(&ltds->ltd_rw_sem);

	LASSERT(ltds->ltd_tgts_bitmap != NULL);

	if (unlikely(index >= ltds->ltd_tgts_bitmap->size))
		goto unlock;

	ltd = LTD_TGT(ltds, index);
	if (unlikely(ltd == NULL))
		goto unlock;

	found = true;
	if (!list_empty(&ltd->ltd_layout_list)) {
		spin_lock(&ltds->ltd_lock);
		list_del_init(&ltd->ltd_layout_list);
		spin_unlock(&ltds->ltd_lock);
	}

	LASSERT(ltds->ltd_tgtnr > 0);

	ltds->ltd_tgtnr--;
	cfs_bitmap_clear(ltds->ltd_tgts_bitmap, index);
	LTD_TGT(ltds, index) = NULL;
	lfsck_tgt_put(ltd);

unlock:
	if (!found) {
		if (for_ost)
			head = &lfsck->li_ost_descs.ltd_orphan;
		else
			head = &lfsck->li_ost_descs.ltd_orphan;

		list_for_each_entry(ltd, head, ltd_orphan_list) {
			if (ltd->ltd_tgt == tgt) {
				list_del_init(&ltd->ltd_orphan_list);
				lfsck_tgt_put(ltd);
				break;
			}
		}
	}

	up_write(&ltds->ltd_rw_sem);
	lfsck_instance_put(env, lfsck);
}
EXPORT_SYMBOL(lfsck_del_target);

static int __init lfsck_init(void)
{
	int rc;

	INIT_LIST_HEAD(&lfsck_ost_orphan_list);
	INIT_LIST_HEAD(&lfsck_mdt_orphan_list);
	lfsck_key_init_generic(&lfsck_thread_key, NULL);
	rc = lu_context_key_register(&lfsck_thread_key);
	if (rc == 0) {
		tgt_register_lfsck_start(lfsck_start);
		tgt_register_lfsck_in_notify(lfsck_in_notify);
		tgt_register_lfsck_query(lfsck_query);
	}

	return rc;
}

static void __exit lfsck_exit(void)
{
	struct lfsck_tgt_desc *ltd;
	struct lfsck_tgt_desc *next;

	LASSERT(cfs_list_empty(&lfsck_instance_list));

	list_for_each_entry_safe(ltd, next, &lfsck_ost_orphan_list,
				 ltd_orphan_list) {
		list_del_init(&ltd->ltd_orphan_list);
		lfsck_tgt_put(ltd);
	}

	list_for_each_entry_safe(ltd, next, &lfsck_mdt_orphan_list,
				 ltd_orphan_list) {
		list_del_init(&ltd->ltd_orphan_list);
		lfsck_tgt_put(ltd);
	}

	lu_context_key_degister(&lfsck_thread_key);
}

MODULE_AUTHOR("Intel Corporation <http://www.intel.com/>");
MODULE_DESCRIPTION("LFSCK");
MODULE_LICENSE("GPL");

cfs_module(lfsck, LUSTRE_VERSION_STRING, lfsck_init, lfsck_exit);
