/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2001, 2002 Cluster File Systems, Inc.
 *
 * This code is issued under the GNU General Public License.
 * See the file COPYING in this distribution
 */

#ifndef __OBD_H
#define __OBD_H

#define IOC_OSC_TYPE         'h'
#define IOC_OSC_MIN_NR       20
#define IOC_OSC_SET_ACTIVE   _IOWR(IOC_OSC_TYPE, 21, struct obd_device *)
#define IOC_OSC_CTL_RECOVERY _IOWR(IOC_OSC_TYPE, 22, struct obd_device *)
#define IOC_OSC_MAX_NR       50

#define IOC_MDC_TYPE         'i'
#define IOC_MDC_MIN_NR       20
#define IOC_MDC_LOOKUP       _IOWR(IOC_MDC_TYPE, 20, struct obd_device *)
/* Moved to lustre_user.h
#define IOC_MDC_GETSTRIPE    _IOWR(IOC_MDC_TYPE, 21, struct lov_mds_md *) */
#define IOC_MDC_MAX_NR       50

#ifdef __KERNEL__
# include <linux/fs.h>
# include <linux/list.h>
# include <linux/sched.h> /* for struct task_struct, for current.h */
# include <asm/current.h> /* for smp_lock.h */
# include <linux/smp_lock.h>
# include <linux/proc_fs.h>
# include <linux/mount.h>
#endif

#define OBD_MDS_DEVICENAME         "mds"
#define OBD_MDT_DEVICENAME         "mdt"
#define OBD_MDC_DEVICENAME         "mdc"
#define OBD_LMV_DEVICENAME         "lmv"
#define OBD_LOV_DEVICENAME         "lov"
#define OBD_OST_DEVICENAME         "ost"
#define OBD_OSC_DEVICENAME         "osc"

#define OBD_LDLM_DEVICENAME        "ldlm"
#define OBD_CACHE_DEVICENAME       "cobd"
#define OBD_CMOBD_DEVICENAME       "cmobd"
#define OBD_CONF_DEVICENAME        "confobd"

#define OBD_SANOSC_DEVICENAME      "sanosc"
#define OBD_SANOST_DEVICENAME      "sanost"

#define OBD_ECHO_DEVICENAME        "obdecho"
#define OBD_ECHO_CLIENT_DEVICENAME "echo_client"

#define OBD_FILTER_DEVICENAME      "obdfilter"
#define OBD_FILTER_SAN_DEVICENAME  "sanobdfilter"

#define OBD_MGMTCLI_DEVICENAME     "mgmt_cli"
#define OBD_PTLBD_SV_DEVICENAME    "ptlbd_server"
#define OBD_PTLBD_CL_DEVICENAME    "ptlbd_client"

#include <linux/lvfs.h>
#include <linux/lustre_lib.h>
#include <linux/lustre_idl.h>
#include <linux/lustre_export.h>
#include <linux/lustre_sec.h>

/* this is really local to the OSC */
struct loi_oap_pages {
        struct list_head        lop_pending;
        int                     lop_num_pending;
        struct list_head        lop_urgent;
        struct list_head        lop_pending_group;
};

struct lov_oinfo {                 /* per-stripe data structure */
        __u64 loi_id;              /* object ID on the target OST */
        __u64 loi_gr;              /* object group on the target OST */
        int loi_ost_idx;           /* OST stripe index in lov_tgt_desc->tgts */
        int loi_ost_gen;           /* generation of this loi_ost_idx */

        /* used by the osc to keep track of what objects to build into rpcs */
        struct loi_oap_pages loi_read_lop;
        struct loi_oap_pages loi_write_lop;
        /* _cli_ is poorly named, it should be _ready_ */
        struct list_head loi_cli_item;
        struct list_head loi_write_item;
        struct list_head loi_read_item;

        unsigned loi_kms_valid:1;
        __u64 loi_kms;             /* known minimum size */
        __u64 loi_rss;             /* recently seen size */
        __u64 loi_mtime;           /* recently seen mtime */
        __u64 loi_blocks;          /* recently seen blocks */
};

static inline void loi_init(struct lov_oinfo *loi)
{
        INIT_LIST_HEAD(&loi->loi_read_lop.lop_pending);
        INIT_LIST_HEAD(&loi->loi_read_lop.lop_urgent);
        INIT_LIST_HEAD(&loi->loi_read_lop.lop_pending_group);
        INIT_LIST_HEAD(&loi->loi_write_lop.lop_pending);
        INIT_LIST_HEAD(&loi->loi_write_lop.lop_urgent);
        INIT_LIST_HEAD(&loi->loi_write_lop.lop_pending_group);
        INIT_LIST_HEAD(&loi->loi_cli_item);
        INIT_LIST_HEAD(&loi->loi_write_item);
        INIT_LIST_HEAD(&loi->loi_read_item);
}

struct lov_stripe_md {
        /* Public members. */
        __u64 lsm_object_id;        /* lov object id */
        __u64 lsm_object_gr;        /* lov object id */
        __u64 lsm_maxbytes;         /* maximum possible file size */
        unsigned long lsm_xfersize; /* optimal transfer size */

        /* LOV-private members start here -- only for use in lov/. */
        __u32 lsm_magic;
        __u32 lsm_stripe_size;      /* size of the stripe */
        __u32 lsm_pattern;          /* striping pattern (RAID0, RAID1) */
        unsigned lsm_stripe_count;  /* number of objects being striped over */
        struct lov_oinfo lsm_oinfo[0];
};

static inline void dump_lsm(int level, struct lov_stripe_md *lsm)
{
        int i;
        CDEBUG(level, "objid "LPX64"/"LPU64", maxbytes "LPX64", magic 0x%08X, "
               "stripe_size %u, stripe_count %u\n",
               lsm->lsm_object_id, lsm->lsm_object_gr, lsm->lsm_maxbytes,
               lsm->lsm_magic, lsm->lsm_stripe_size, lsm->lsm_stripe_count);
        for (i = 0; i < lsm->lsm_stripe_count; i++)
                CDEBUG(level, "idx %u ostidx %u/%u object "LPU64"/"LPU64"\n",
                       i, lsm->lsm_oinfo[i].loi_ost_idx,
                       lsm->lsm_oinfo[i].loi_ost_gen,
                       lsm->lsm_oinfo[i].loi_id, lsm->lsm_oinfo[i].loi_gr);
}

struct obd_type {
        struct list_head typ_chain;
        struct obd_ops *typ_ops;
        struct md_ops *typ_md_ops;
        struct proc_dir_entry *typ_procroot;
        char *typ_name;
        int  typ_refcnt;
};

struct brw_page {
        obd_off disk_offset; /* modulo PAGE_SIZE */
        obd_off page_offset; /* modulo PAGE_SIZE (obviously) */
        struct page *pg;
        int count;
        obd_flags flag;
};

enum async_flags {
        ASYNC_READY = 0x1, /* ap_make_ready will not be called before this
                              page is added to an rpc */
        ASYNC_URGENT = 0x2,
        ASYNC_COUNT_STABLE = 0x4, /* ap_refresh_count will not be called
                                     to give the caller a chance to update
                                     or cancel the size of the io */
        ASYNC_GROUP_SYNC = 0x8,  /* ap_completion will not be called, instead
                                    the page is accounted for in the
                                    obd_io_group given to 
                                    obd_queue_group_io */
};

struct obd_async_page_ops {
        int  (*ap_make_ready)(void *data, int cmd);
        int  (*ap_refresh_count)(void *data, int cmd);
        void (*ap_fill_obdo)(void *data, int cmd, struct obdo *oa);
        void (*ap_completion)(void *data, int cmd, struct obdo *oa, int rc);
};

/* the `oig' is passed down from a caller of obd rw methods.  the callee
 * records enough state such that the caller can sleep on the oig and
 * be woken when all the callees have finished their work */
struct obd_io_group {
        spinlock_t      oig_lock;
        atomic_t        oig_refcount;
        int             oig_pending;
        int             oig_rc;
        struct list_head oig_occ_list;
        wait_queue_head_t oig_waitq;
};

/* the oig callback context lets the callee of obd rw methods register
 * for callbacks from the caller. */
struct oig_callback_context {
        struct list_head occ_oig_item;
        /* called when the caller has received a signal while sleeping.
         * callees of this method are encouraged to abort their state 
         * in the oig.  This may be called multiple times. */
        void (*occ_interrupted)(struct oig_callback_context *occ);
        int interrupted;
};

/* if we find more consumers this could be generalized */
#define OBD_HIST_MAX 32
struct obd_histogram {
        spinlock_t      oh_lock;
        unsigned long   oh_buckets[OBD_HIST_MAX];
};

/* reports average service time with the help of lprocfs_status.c */
struct obd_service_time {
        __u32           st_num;
        __u64           st_total_us;
};

struct ost_server_data;

#define FILTER_SUBDIR_COUNT      32            /* set to zero for no subdirs */

#define FILTER_GROUP_LLOG 1
#define FILTER_GROUP_ECHO 2
#define FILTER_GROUP_FIRST_MDS 3

struct filter_subdirs {
        struct dentry *dentry[FILTER_SUBDIR_COUNT];
};

struct filter_group_llog {
        struct list_head list;
        int group;
        struct obd_llogs *llogs;
        struct obd_export *exp;
};

struct filter_obd {
        const char          *fo_fstype;
        struct super_block  *fo_sb;
        struct vfsmount     *fo_vfsmnt;
        struct lvfs_obd_ctxt *fo_lvfs_ctxt;

        int                    fo_group_count;
        struct dentry         *fo_dentry_O;     /* the "O"bject directory dentry */
        struct dentry         **fo_groups;      /* dentries for each group dir */
        struct filter_subdirs *fo_subdirs;      /* subdir array per group */
        __u64                 *fo_last_objids;  /* per-group last created objid */
        struct file          **fo_last_objid_files;
        struct semaphore     fo_init_lock;      /* group initialization lock */
        int                  fo_committed_group;

        spinlock_t           fo_objidlock;      /* protect fo_lastobjid increment */
        spinlock_t           fo_lastidlock;     /* protect last_id increment */
        spinlock_t           fo_translock;      /* protect fsd_last_rcvd increment */
        struct file         *fo_rcvd_filp;
        struct filter_server_data *fo_fsd;
        unsigned long       *fo_last_rcvd_slots;
        __u64                fo_mount_count;

        unsigned long        fo_destroys_in_progress;
        struct semaphore     fo_create_locks[32];

        struct file_operations *fo_fop;
        struct inode_operations *fo_iop;
        struct address_space_operations *fo_aops;

        struct list_head     fo_export_list;
        int                  fo_subdir_count;

        obd_size             fo_tot_dirty;      /* protected by obd_osfs_lock */
        obd_size             fo_tot_granted;    /* all values in bytes */
        obd_size             fo_tot_pending;

        obd_size             fo_readcache_max_filesize;

        struct obd_import   *fo_mdc_imp;
        struct obd_uuid      fo_mdc_uuid;
        struct lustre_handle fo_mdc_conn;

        struct semaphore     fo_alloc_lock;

        struct obd_histogram     fo_r_pages;
        struct obd_histogram     fo_w_pages;
        struct obd_histogram     fo_r_discont_pages;
        struct obd_histogram     fo_w_discont_pages;
        struct obd_histogram     fo_r_discont_blocks;
        struct obd_histogram     fo_w_discont_blocks;

        struct list_head         fo_llog_list;
        spinlock_t               fo_llog_list_lock;

        /* which secure flavor from remote is denied */
        spinlock_t               fo_denylist_lock;
        struct list_head         fo_denylist;

        /* capability related */
        int                      fo_capa_stat;
        struct crypto_tfm       *fo_capa_hmac;
        spinlock_t               fo_capa_lock;

        struct list_head         fo_capa_keys;
};

struct mds_server_data;

#define OSC_MAX_RIF_DEFAULT      16
#define OSC_MAX_RIF_MAX          64
#define OSC_MAX_DIRTY_DEFAULT    (4*OSC_MAX_RIF_DEFAULT*PTLRPC_MAX_BRW_SIZE>>20)
#define OSC_MAX_DIRTY_MB_MAX     512     /* totally arbitrary */

struct mdc_rpc_lock;
struct client_obd {
        struct obd_import       *cl_import;
        struct semaphore         cl_sem;
        int                      cl_conn_count;
        /* max_mds_easize is purely a performance thing so we don't have to
         * call obd_size_wiremd() all the time. */
        int                      cl_max_mds_easize;
        int                      cl_max_mds_cookiesize;
        kdev_t                   cl_sandev;

        /* security flavors */
        __u32                    cl_sec_flavor;
        unsigned long            cl_sec_flags;

        //struct llog_canceld_ctxt *cl_llcd; /* it's included by obd_llog_ctxt */
        void                    *cl_llcd_offset;

        struct obd_device       *cl_mgmtcli_obd;

        /* the grant values are protected by loi_list_lock below */
        long                     cl_dirty;         /* all _dirty_ in bytes */
        long                     cl_dirty_max;     /* allowed w/o rpc */
        long                     cl_avail_grant;   /* bytes of credit for ost */
        long                     cl_lost_grant;    /* lost credits (trunc) */
        struct list_head         cl_cache_waiters; /* waiting for cache/grant */

        /* keep track of objects that have lois that contain pages which
         * have been queued for async brw.  this lock also protects the
         * lists of osc_client_pages that hang off of the loi */
        spinlock_t               cl_loi_list_lock;
        struct list_head         cl_loi_ready_list;
        struct list_head         cl_loi_write_list;
        struct list_head         cl_loi_read_list;
        int                      cl_r_in_flight;
        int                      cl_w_in_flight;
        /* just a sum of the loi/lop pending numbers to be exported by /proc */
        int                      cl_pending_w_pages;
        int                      cl_pending_r_pages;
        int                      cl_max_pages_per_rpc;
        int                      cl_max_rpcs_in_flight;
        struct obd_histogram     cl_read_rpc_hist;
        struct obd_histogram     cl_write_rpc_hist;
        struct obd_histogram     cl_read_page_hist;
        struct obd_histogram     cl_write_page_hist;
        struct obd_service_time  cl_read_stime;
        struct obd_service_time  cl_write_stime;
        struct obd_service_time  cl_enter_stime;

        struct mdc_rpc_lock     *cl_rpc_lock;
        struct mdc_rpc_lock     *cl_setattr_lock; 
        struct mdc_rpc_lock     *cl_close_lock;
        struct osc_creator       cl_oscc;
        int                      cl_async:1;

        /* debug stuff */
        struct timeval           cl_last_write_time;
        unsigned long            cl_write_gap_sum;
        unsigned long            cl_write_gaps;
        unsigned long            cl_write_num;
        unsigned long            cl_read_num;
        unsigned long            cl_cache_wait_num;
        unsigned long            cl_cache_wait_sum;

        unsigned long            cl_dirty_num;
        unsigned long            cl_dirty_sum;
        unsigned long            cl_dirty_av;
        
        unsigned long            cl_dirty_dmax;
        unsigned long            cl_dirty_dmin;

        unsigned long            cl_sync_rpcs;
};

/* Like a client, with some hangers-on.  Keep mc_client_obd first so that we
 * can reuse the various client setup/connect functions. */
struct mgmtcli_obd {
        struct client_obd        mc_client_obd; /* nested */
        struct ptlrpc_thread    *mc_ping_thread;
        struct obd_export       *mc_ping_exp; /* XXX single-target */
        struct list_head         mc_registered;
        void                    *mc_hammer;
};

#define mc_import mc_client_obd.cl_import

struct mds_obd {
        struct ptlrpc_service           *mds_service;
        struct ptlrpc_service           *mds_setattr_service;
        struct ptlrpc_service           *mds_readpage_service;
        struct ptlrpc_service           *mds_close_service;
        struct super_block              *mds_sb;
        struct vfsmount                 *mds_vfsmnt;
        struct dentry                   *mds_id_de;
        struct lvfs_obd_ctxt            *mds_lvfs_ctxt;
        int                              mds_max_mdsize;
        int                              mds_max_cookiesize;
        struct file                     *mds_rcvd_filp;
        struct file                     *mds_fid_filp;
        struct file                     *mds_virtid_filp;
        spinlock_t                       mds_transno_lock;
        __u64                            mds_last_transno;
        __u64                            mds_mount_count;
        __u64                            mds_io_epoch;
        
        __u64                            mds_last_fid;
        __u64                            mds_virtid_fid;
        spinlock_t                       mds_last_fid_lock;
        
        struct semaphore                 mds_epoch_sem;
        struct lustre_id                 mds_rootid;
        struct mds_server_data          *mds_server_data;
        struct dentry                   *mds_pending_dir;
        struct dentry                   *mds_logs_dir;
        struct dentry                   *mds_objects_dir;
        struct llog_handle              *mds_cfg_llh;
        char                            *mds_profile;
        struct obd_device               *mds_dt_obd;
        struct obd_uuid                  mds_dt_uuid;
        struct obd_export               *mds_dt_exp;
        int                              mds_has_dt_desc;
        struct lov_desc                  mds_dt_desc;

        spinlock_t                       mds_dt_lock;
        obd_id                          *mds_dt_objids;
        struct file                     *mds_dt_objid_filp;
        int                              mds_dt_objids_valid;

        unsigned long                   *mds_client_bitmap;
        struct semaphore                 mds_orphan_recovery_sem;
        
        int                              mds_num;
        int                              mds_config_version;

        char                            *mds_md_name;
        struct obd_device               *mds_md_obd;
        struct obd_export               *mds_md_exp;
        struct semaphore                 mds_md_sem;
        struct obd_uuid                  mds_md_uuid;
        int                              mds_md_connected;

        struct ptlrpc_service           *mds_create_service;
        uid_t                            mds_squash_uid;
        gid_t                            mds_squash_gid;
        ptl_nid_t                        mds_nosquash_nid;
        atomic_t                         mds_real_clients;
        atomic_t                         mds_open_count;
        struct dentry                   *mds_id_dir;
        int                              mds_obd_type;
        struct dentry                   *mds_unnamed_dir; /* for mdt_obd_create only */

        /* security related */
        char                            *mds_mds_sec;
        char                            *mds_ost_sec;

        /* which secure flavor from remote to this mds is denied */
        spinlock_t                      mds_denylist_lock;
        struct list_head                mds_denylist;

        /* fid->ino mapping related fields */
        spinlock_t                      mds_fidmap_lock;
        struct hlist_head              *mds_fidmap_table;
        unsigned long                   mds_fidmap_size;

        /* cache fid extents stuff */
        spinlock_t                      mds_fidext_lock;
        __u64                           mds_fidext_thumb;
        int                             mds_crypto_type;

        /* capability related */
        int                              mds_capa_stat;     /* 1: on, 0: off */
        struct crypto_tfm               *mds_capa_hmac;
        unsigned long                    mds_capa_timeout;  /* sec */

        struct mds_capa_key              mds_capa_keys[2];  /* red & black key */
        int                              mds_capa_key_idx;  /* the red key index */
        struct file                     *mds_capa_keys_filp;
        unsigned long                    mds_capa_key_timeout; /* sec */
};

struct echo_obd {
        struct obdo          eo_oa;
        spinlock_t           eo_lock;
        __u64                eo_lastino;
        struct lustre_handle eo_nl_lock;
        atomic_t             eo_prep;
};

/*
 * this struct does double-duty acting as either a client or
 * server instance .. maybe not wise.
 */
struct ptlbd_obd {
        /* server's */
        struct ptlrpc_service *ptlbd_service;
        struct file *filp;
        /* client's */
        struct ptlrpc_client    bd_client;
        struct obd_import       *bd_import;
        struct obd_uuid         bd_server_uuid;
        struct obd_export       *bd_exp;
        int refcount; /* XXX sigh */
};

struct recovd_obd {
        spinlock_t            recovd_lock;
        struct list_head      recovd_managed_items; /* items managed  */
        struct list_head      recovd_troubled_items; /* items in recovery */

        wait_queue_head_t     recovd_recovery_waitq;
        wait_queue_head_t     recovd_ctl_waitq;
        wait_queue_head_t     recovd_waitq;
        struct task_struct   *recovd_thread;
        __u32                 recovd_state;
};

struct ost_obd {
        spinlock_t             ost_lock;
        struct ptlrpc_service *ost_service;
        struct ptlrpc_service *ost_create_service;
        struct ptlrpc_service *ost_destroy_service;
        struct obd_service_time ost_stimes[6];
};

struct echo_client_obd {
        struct obd_export      *ec_exp;     /* the local connection to osc/lov */
        spinlock_t              ec_lock;
        struct list_head        ec_objects;
        int                     ec_nstripes;
        __u64                   ec_unique;
};

struct cache_obd {
        struct obd_export      *master_exp; /* local connection to master obd */
        struct obd_export      *cache_exp;  /* local connection to cache obd */
        struct obd_export      *cache_real_exp;
        struct obd_export      *master_real_exp;
        struct obd_device      *master;
        struct obd_device      *cache;
        char                   *master_name;
        char                   *cache_name;
        int                     refcount;
        int                     cache_on;
        struct semaphore        sem;
        struct lov_desc         dt_desc; /* data lovdesc */
};

struct cm_obd {
        struct obd_export      *cache_exp;  /* local connection to cache obd */
        struct obd_export      *master_exp;
        struct obd_device      *cache_obd;
        struct obd_device      *master_obd;
        int                     master_group;
        struct cmobd_write_service *write_srv;
        struct lov_desc         master_desc; /* master device lovdesc */
};

struct conf_obd {
        struct super_block      *cfobd_sb;
        struct vfsmount         *cfobd_vfsmnt;
        struct dentry           *cfobd_logs_dir;
        struct dentry           *cfobd_objects_dir;
        struct dentry           *cfobd_pending_dir;
        struct llog_handle      *cfobd_cfg_llh;
        struct lvfs_obd_ctxt    *cfobd_lvfs_ctxt;
};

enum lov_tgt_flags {
        LTD_ACTIVE      = 0x1, /* is this target up for requests   */
        LTD_DEL_PENDING = 0x2, /* delete event pending for this tgt */
};

struct lov_tgt_desc {
        struct obd_uuid         uuid;
        __u32                   ltd_gen;
        struct obd_export      *ltd_exp;
        unsigned int            ltd_flags;
        int                     ltd_refcount;
};

struct lov_obd {
        spinlock_t              lov_lock;
        struct lov_desc         desc;
        int                     bufsize;
        int                     refcount;
        int                     lo_catalog_loaded:1, async:1;
        struct semaphore        lov_llog_sem;
        unsigned long           lov_connect_flags;
        wait_queue_head_t       lov_tgt_waitq;
        struct lov_tgt_desc    *tgts;
};

struct lmv_tgt_desc {
        struct obd_uuid         uuid;
        struct obd_export      *ltd_exp;
        int                     active;   /* is this target up for requests */
};

struct lmv_obd {
        int                     refcount;
        spinlock_t              lmv_lock;
        struct lmv_desc         desc;
        struct lmv_tgt_desc     *tgts;
        struct obd_uuid         cluuid;
        struct obd_export       *exp;

        int                     tgts_size;
        int                     connected;
        int                     max_easize;
        int                     max_cookiesize;
        int                     server_timeout;
        int                     connect_flags;
        struct semaphore        init_sem;
        struct obd_connect_data conn_data;
};
struct gks_crypto_key {
        char *key;
        int   len; 
};
struct gks_obd {
        struct ptlrpc_service    *gks_service;
        struct crypto_tfm        *gks_mac_tfm;       
        struct crypto_tfm        *gks_key_tfm;
        struct gks_crypto_key    gks_key;
};

struct niobuf_local {
        __u64 offset;
        __u32 len;
        __u32 flags;
        struct page *page;
        struct dentry *dentry;
        int lnb_grant_used;
        int rc;
};

#define OBD_MODE_ASYNC (1 << 0)
#define OBD_MODE_CROW  (1 << 1)

/* Don't conflict with on-wire flags OBD_BRW_WRITE, etc */
#define N_LOCAL_TEMP_PAGE 0x10000000

struct obd_trans_info {
        __u64                    oti_transno;
        __u64                   *oti_objid;

        /* only used on the server side for tracking acks. */
        struct oti_req_ack_lock {
                struct lustre_handle lock;
                __u32                mode;
        }                        oti_ack_locks[4];
        void                    *oti_handle;
        struct llog_cookie       oti_onecookie;
        struct llog_cookie      *oti_logcookies;
        int                      oti_numcookies;
        int                      oti_flags;
        /* save nid for security purposes like audit */
        __u64                    oti_nid; 
};

static inline void oti_alloc_cookies(struct obd_trans_info *oti,int num_cookies)
{
        if (!oti)
                return;

        if (num_cookies == 1)
                oti->oti_logcookies = &oti->oti_onecookie;
        else
                OBD_ALLOC(oti->oti_logcookies,
                          num_cookies * sizeof(oti->oti_onecookie));

        oti->oti_numcookies = num_cookies;
}

static inline void oti_free_cookies(struct obd_trans_info *oti)
{
        if (!oti || !oti->oti_logcookies)
                return;

        if (oti->oti_logcookies == &oti->oti_onecookie)
                LASSERT(oti->oti_numcookies == 1);
        else
                OBD_FREE(oti->oti_logcookies,
                         oti->oti_numcookies * sizeof(oti->oti_onecookie));
        oti->oti_logcookies = NULL;
        oti->oti_numcookies = 0;
}

/* llog contexts */
enum llog_ctxt_id {
        LLOG_CONFIG_ORIG_CTXT =  0,
        LLOG_CONFIG_REPL_CTXT =  1,
        LLOG_UNLINK_ORIG_CTXT =  2,
        LLOG_UNLINK_REPL_CTXT =  3,
        LLOG_SIZE_ORIG_CTXT   =  4,
        LLOG_SIZE_REPL_CTXT   =  5,
        LLOG_MD_ORIG_CTXT     =  6,
        LLOG_MD_REPL_CTXT     =  7,
        LLOG_RD1_ORIG_CTXT    =  8,
        LLOG_RD1_REPL_CTXT    =  9,
        LLOG_TEST_ORIG_CTXT   = 10,
        LLOG_TEST_REPL_CTXT   = 11,
        LLOG_REINT_ORIG_CTXT  = 12,
        LLOG_AUDIT_ORIG_CTXT  = 13,
        LLOG_MAX_CTXTS
};

struct obd_llogs {
        struct llog_ctxt        *llog_ctxt[LLOG_MAX_CTXTS];
};

struct target_recovery_data {
        svc_handler_t     trd_recovery_handler;
        pid_t             trd_processing_task;
        struct completion trd_starting;
        struct completion trd_finishing;
};

/* corresponds to one of the obd's */
struct obd_device {
        struct obd_type *obd_type;

        /* common and UUID name of this device */
        char *obd_name;
        struct obd_uuid obd_uuid;

        int obd_minor;
        unsigned int obd_attached:1, obd_set_up:1, obd_recovering:1,
                obd_abort_recovery:1, obd_replayable:1, obd_no_transno:1,
                obd_no_recov:1, obd_stopping:1, obd_req_replaying:1;
        atomic_t obd_refcount;
        wait_queue_head_t obd_refcount_waitq;
        struct proc_dir_entry *obd_proc_entry;
        struct list_head       obd_exports;
        int                    obd_num_exports;
        struct ldlm_namespace *obd_namespace;
        struct ptlrpc_client   obd_ldlm_client; /* XXX OST/MDS only */
        /* a spinlock is OK for what we do now, may need a semaphore later */
        spinlock_t             obd_dev_lock;
        __u64                  obd_last_committed;
        struct fsfilt_operations *obd_fsops;
        spinlock_t              obd_osfs_lock;
        struct obd_statfs       obd_osfs;
        unsigned long           obd_osfs_age;
        struct lvfs_run_ctxt    obd_lvfs_ctxt;
        struct obd_llogs        obd_llogs;
        struct llog_ctxt        *obd_llog_ctxt[LLOG_MAX_CTXTS];
        struct obd_device       *obd_observer;
        struct obd_export       *obd_self_export;

        struct target_recovery_data      obd_recovery_data;
        /* XXX encapsulate all this recovery data into target_recovery_data */
        int                              obd_max_recoverable_clients;
        int                              obd_connected_clients;
        int                              obd_recoverable_clients;
        spinlock_t                       obd_processing_task_lock;
        __u64                            obd_next_recovery_transno;
        int                              obd_replayed_requests;
        int                              obd_replayed_locks;
        int                              obd_requests_queued_for_recovery;
        wait_queue_head_t                obd_next_transno_waitq;
        struct list_head                 obd_uncommitted_replies;
        spinlock_t                       obd_uncommitted_replies_lock;
        struct timer_list                obd_recovery_timer;
        time_t                           obd_recovery_start;
        time_t                           obd_recovery_end;

        atomic_t                         obd_req_replay_clients;
        atomic_t                         obd_lock_replay_clients;

        struct list_head                 obd_req_replay_queue;
        struct list_head                 obd_lock_replay_queue;
        struct list_head                 obd_final_req_queue;
        int                              obd_recovery_stage;

        union {
                struct filter_obd        filter;
                struct mds_obd           mds;
                struct client_obd        cli;
                struct ost_obd           ost;
                struct echo_client_obd   echocli;
                struct echo_obd          echo;
                struct recovd_obd        recovd;
                struct lov_obd           lov;
                struct cache_obd         cobd;
                struct ptlbd_obd         ptlbd;
                struct mgmtcli_obd       mgmtcli;
                struct lmv_obd           lmv;
                struct cm_obd            cm;
                struct conf_obd          conf;
                struct gks_obd           gks;
        } u;
        
        /* fields used by LProcFS */
        unsigned int           obd_cntr_base;
        struct lprocfs_stats  *obd_stats;
        unsigned int           md_cntr_base;
        struct lprocfs_stats  *md_stats;

        struct proc_dir_entry *obd_svc_procroot;
        struct lprocfs_stats  *obd_svc_stats;
};

#define OBD_OPT_FORCE             (1 << 0)
#define OBD_OPT_FAILOVER          (1 << 1)
#define OBD_OPT_REAL_CLIENT       (1 << 2)
#define OBD_OPT_MDS_CONNECTION    (1 << 3)

#define OBD_LLOG_FL_SENDNOW       (1 << 0)
#define OBD_LLOG_FL_CREATE        (1 << 1)

struct mdc_op_data;

struct obd_ops {
        struct module *o_owner;
        int (*o_iocontrol)(unsigned int cmd, struct obd_export *exp, int len,
                           void *karg, void *uarg);
        int (*o_get_info)(struct obd_export *, __u32 keylen, void *key,
                          __u32 *vallen, void *val);
        int (*o_set_info)(struct obd_export *, __u32 keylen, void *key,
                          __u32 vallen, void *val);
        int (*o_attach)(struct obd_device *dev, obd_count len, void *data);
        int (*o_detach)(struct obd_device *dev);
        int (*o_setup) (struct obd_device *dev, obd_count len, void *data);
        int (*o_precleanup)(struct obd_device *dev, int flags);
        int (*o_cleanup)(struct obd_device *dev, int flags);
        int (*o_process_config)(struct obd_device *dev, obd_count len,
                                void *data);
        int (*o_postrecov)(struct obd_device *dev);
        int (*o_add_conn)(struct obd_import *imp, struct obd_uuid *uuid,
                          int priority);
        int (*o_del_conn)(struct obd_import *imp, struct obd_uuid *uuid);
        int (*o_connect)(struct lustre_handle *conn, struct obd_device *src,
                         struct obd_uuid *cluuid, struct obd_connect_data *data,
                         unsigned long flags);
        int (*o_connect_post)(struct obd_export *exp, unsigned, unsigned long);
        int (*o_disconnect)(struct obd_export *exp, unsigned long flags);

        int (*o_statfs)(struct obd_device *obd, struct obd_statfs *osfs,
                        unsigned long max_age);
        int (*o_packmd)(struct obd_export *exp, struct lov_mds_md **disk_tgt,
                        struct lov_stripe_md *mem_src);
        int (*o_unpackmd)(struct obd_export *exp,struct lov_stripe_md **mem_tgt,
                          struct lov_mds_md *disk_src, int disk_len);
        int (*o_revalidate_md)(struct obd_export *exp,  struct obdo *oa,
                               struct lov_stripe_md *ea,
                               struct obd_trans_info *oti);
        int (*o_preallocate)(struct lustre_handle *, obd_count *req,
                             obd_id *ids);
        int (*o_create)(struct obd_export *exp,  struct obdo *oa,
                        void *acl, int acl_size,
                        struct lov_stripe_md **ea, struct obd_trans_info *oti);
        int (*o_destroy)(struct obd_export *exp, struct obdo *oa,
                         struct lov_stripe_md *ea, struct obd_trans_info *oti);
        int (*o_setattr)(struct obd_export *exp, struct obdo *oa,
                         struct lov_stripe_md *ea, struct obd_trans_info *oti,
                         struct lustre_capa *capa);
        int (*o_getattr)(struct obd_export *exp, struct obdo *oa,
                         struct lov_stripe_md *ea);
        int (*o_getattr_async)(struct obd_export *exp, struct obdo *oa,
                               struct lov_stripe_md *ea,
                               struct ptlrpc_request_set *set);
        int (*o_brw)(int rw, struct obd_export *exp, struct obdo *oa,
                     struct lov_stripe_md *ea, obd_count oa_bufs,
                     struct brw_page *pgarr, struct obd_trans_info *oti);
        int (*o_brw_async)(int rw, struct obd_export *exp, struct obdo *oa,
                           struct lov_stripe_md *ea, obd_count oa_bufs,
                           struct brw_page *pgarr, struct ptlrpc_request_set *,
                           struct obd_trans_info *oti);
        int (*o_prep_async_page)(struct obd_export *exp, 
                                 struct lov_stripe_md *lsm,
                                 struct lov_oinfo *loi, 
                                 struct page *page, obd_off offset, 
                                 struct obd_async_page_ops *ops, void *data,
                                 void **res);
        int (*o_queue_async_io)(struct obd_export *exp, 
                                struct lov_stripe_md *lsm, 
                                struct lov_oinfo *loi, void *cookie, 
                                int cmd, obd_off off, int count, 
                                obd_flags brw_flags, obd_flags async_flags);
        int (*o_queue_group_io)(struct obd_export *exp, 
                                struct lov_stripe_md *lsm, 
                                struct lov_oinfo *loi, 
                                struct obd_io_group *oig, 
                                void *cookie, int cmd, obd_off off, int count, 
                                obd_flags brw_flags, obd_flags async_flags);
        int (*o_trigger_group_io)(struct obd_export *exp, 
                                  struct lov_stripe_md *lsm, 
                                  struct lov_oinfo *loi, 
                                  struct obd_io_group *oig);
        int (*o_set_async_flags)(struct obd_export *exp,
                                struct lov_stripe_md *lsm,
                                struct lov_oinfo *loi, void *cookie,
                                obd_flags async_flags);
        int (*o_teardown_async_page)(struct obd_export *exp,
                                     struct lov_stripe_md *lsm,
                                     struct lov_oinfo *loi, void *cookie);
        int (*o_adjust_kms)(struct obd_export *exp, struct lov_stripe_md *lsm,
                            obd_off size, int shrink);
        int (*o_punch)(struct obd_export *exp, struct obdo *oa,
                       struct lov_stripe_md *ea, obd_size start,
                       obd_size end, struct obd_trans_info *oti,
                       struct lustre_capa *capa);
        int (*o_sync)(struct obd_export *exp, struct obdo *oa,
                      struct lov_stripe_md *ea, obd_size start, obd_size end);
        int (*o_migrate)(struct lustre_handle *conn, struct lov_stripe_md *dst,
                         struct lov_stripe_md *src, obd_size start,
                         obd_size end, struct obd_trans_info *oti);
        int (*o_copy)(struct lustre_handle *dstconn, struct lov_stripe_md *dst,
                      struct lustre_handle *srconn, struct lov_stripe_md *src,
                      obd_size start, obd_size end, struct obd_trans_info *);
        int (*o_iterate)(struct lustre_handle *conn,
                         int (*)(obd_id, obd_gr, void *),
                         obd_id *startid, obd_gr group, void *data);
        int (*o_preprw)(int cmd, struct obd_export *exp, struct obdo *oa,
                        int objcount, struct obd_ioobj *obj,
                        int niocount, struct niobuf_remote *remote,
                        struct niobuf_local *local, struct obd_trans_info *oti,
                        struct lustre_capa *capa);
        int (*o_commitrw)(int cmd, struct obd_export *exp, struct obdo *oa,
                          int objcount, struct obd_ioobj *obj,
                          int niocount, struct niobuf_local *local,
                          struct obd_trans_info *oti, int rc);
        int (*o_do_cow)(struct obd_export *exp, struct obd_ioobj *obj, 
                        int objcount, struct niobuf_remote *rnb);
        int (*o_write_extents)(struct obd_export *exp, struct obd_ioobj *obj,
                               int objcount, int niocount, 
                               struct niobuf_local *local,int rc);
        int (*o_enqueue)(struct obd_export *, struct lov_stripe_md *,
                         __u32 type, ldlm_policy_data_t *, __u32 mode,
                         int *flags, void *bl_cb, void *cp_cb, void *gl_cb,
                         void *data, __u32 lvb_len, void *lvb_swabber,
                         struct lustre_handle *lockh);
        int (*o_match)(struct obd_export *, struct lov_stripe_md *, __u32 type,
                       ldlm_policy_data_t *, __u32 mode, int *flags, void *data,
                       struct lustre_handle *lockh);
        int (*o_change_cbdata)(struct obd_export *, struct lov_stripe_md *,
                               ldlm_iterator_t it, void *data);
        int (*o_cancel)(struct obd_export *, struct lov_stripe_md *md,
                        __u32 mode, struct lustre_handle *);
        int (*o_cancel_unused)(struct obd_export *, struct lov_stripe_md *,
                               int flags, void *opaque);
        int (*o_san_preprw)(int cmd, struct obd_export *exp,
                            struct obdo *oa, int objcount,
                            struct obd_ioobj *obj, int niocount,
                            struct niobuf_remote *remote);
        int (*o_init_export)(struct obd_export *exp);
        int (*o_destroy_export)(struct obd_export *exp);

        /* llog related obd_methods */
        int (*o_llog_init)(struct obd_device *, struct obd_llogs *,
                           struct obd_device *, int, struct llog_catid *);
        int (*o_llog_finish)(struct obd_device *, struct obd_llogs *, int);
        int (*o_llog_connect)(struct obd_export *, struct llogd_conn_body *);

       
        /* metadata-only methods */
        int (*o_pin)(struct obd_export *, obd_id ino, __u32 gen, int type,
                     struct obd_client_handle *, int flag);
        int (*o_unpin)(struct obd_export *, struct obd_client_handle *, int);

        int (*o_import_event)(struct obd_device *, struct obd_import *,
                              enum obd_import_event);

        int (*o_notify)(struct obd_device *obd, struct obd_device *watched,
                        int active, void *data);

        int (*o_init_ea_size)(struct obd_export *, int, int);

        /* 
         * NOTE: If adding ops, add another LPROCFS_OBD_OP_INIT() line
         * to lprocfs_alloc_obd_stats() in obdclass/lprocfs_status.c.
         * Also, add a wrapper function in include/linux/obd_class.h.
         */
};

struct md_ops {
        int (*m_getstatus)(struct obd_export *, struct lustre_id *);
        int (*m_change_cbdata)(struct obd_export *, struct lustre_id *,
                               ldlm_iterator_t, void *);
        int (*m_change_cbdata_name)(struct obd_export *, struct lustre_id *,
                                    char *, int, struct lustre_id *,
                                    ldlm_iterator_t, void *);
        int (*m_close)(struct obd_export *, struct mdc_op_data *,
                       struct obd_client_handle *, struct ptlrpc_request **);
        int (*m_create)(struct obd_export *, struct mdc_op_data *,
                        const void *, int, int, __u32, __u32,
                        __u64, struct ptlrpc_request **);
        int (*m_done_writing)(struct obd_export *, struct obdo *);
        int (*m_enqueue)(struct obd_export *, int, struct lookup_intent *,
                         int, struct mdc_op_data *, struct lustre_handle *,
                         void *, int, ldlm_completion_callback,
                         ldlm_blocking_callback, void *);
        int (*m_getattr)(struct obd_export *, struct lustre_id *,
                         __u64, const char *, const void *, unsigned int,
                         unsigned int, struct obd_capa *,
                         struct ptlrpc_request **);
        int (*m_access_check)(struct obd_export *, struct lustre_id *,
                              struct ptlrpc_request **);
        int (*m_getattr_lock)(struct obd_export *, struct lustre_id *,
                              char *, int, __u64,
                              unsigned int, struct ptlrpc_request **);
        int (*m_intent_lock)(struct obd_export *,
                             struct lustre_id *, const char *, int,
                             void *, int, struct lustre_id *,
                             struct lookup_intent *, int,
                             struct ptlrpc_request **,
                             ldlm_blocking_callback);
        int (*m_link)(struct obd_export *, struct mdc_op_data *,
                      struct ptlrpc_request **);
        int (*m_rename)(struct obd_export *, struct mdc_op_data *,
                        const char *, int, const char *, int,
                        struct ptlrpc_request **);
        int (*m_setattr)(struct obd_export *, struct mdc_op_data *,
                         struct iattr *, void *, int , void *, int,
                         void *, int, struct ptlrpc_request **);
        int (*m_sync)(struct obd_export *, struct lustre_id *,
                      struct ptlrpc_request **);
        int (*m_readpage)(struct obd_export *, struct lustre_id *,
                          __u64, struct page *, struct ptlrpc_request **);
        int (*m_unlink)(struct obd_export *, struct mdc_op_data *,
                        struct ptlrpc_request **);
        int (*m_valid_attrs)(struct obd_export *, struct lustre_id *);
        
        struct obd_device *(*m_get_real_obd)(struct obd_export *, struct lustre_id *);
        
        int (*m_req2lustre_md)(struct obd_export *exp, 
                               struct ptlrpc_request *req, unsigned int offset,
                               struct obd_export *osc_exp, struct lustre_md *md);
        int (*m_set_open_replay_data)(struct obd_export *exp,
                                      struct obd_client_handle *och,
                                      struct ptlrpc_request *open_req);
        int (*m_clear_open_replay_data)(struct obd_export *exp,
                                        struct obd_client_handle *och);
        int (*m_store_inode_generation)(struct obd_export *exp, 
                                        struct ptlrpc_request *req, int reqoff,
                                        int repoff);
        int (*m_set_lock_data)(struct obd_export *exp, __u64 *l, void *data);

        int (*m_delete_inode)(struct obd_export *, struct lustre_id *);

        /*
         * NOTE: If adding ops, add another LPROCFS_MD_OP_INIT() line to
         * lprocfs_alloc_md_stats() in obdclass/lprocfs_status.c. Also, add a
         * wrapper function in include/linux/obd_class.h.
         */
};

static inline void obd_transno_commit_cb(struct obd_device *obd,
                                         __u64 transno, int error)
{
        if (error) {
                CERROR("%s: transno "LPD64" commit error: %d\n",
                       obd->obd_name, transno, error);
                return;
        }
        
        CDEBUG(D_HA, "%s: transno "LPD64" committed\n",
               obd->obd_name, transno);

        if (transno > obd->obd_last_committed) {
                obd->obd_last_committed = transno;
                ptlrpc_commit_replies (obd);
        }
}

static inline int obd_md_type(struct obd_device *obd)
{
        if (!strcmp(obd->obd_type->typ_name, OBD_MDC_DEVICENAME) ||
            !strcmp(obd->obd_type->typ_name, OBD_LMV_DEVICENAME))
                return 1;

        return 0;
}

static inline int obd_dt_type(struct obd_device *obd)
{
        if (!strcmp(obd->obd_type->typ_name, OBD_LOV_DEVICENAME) ||
            !strcmp(obd->obd_type->typ_name, OBD_OSC_DEVICENAME))
                return 1;

        return 0;
}

#endif /* __OBD_H */
