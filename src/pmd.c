// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2015-2020 Micron Technology, Inc.  All rights reserved.
 */

/* DOC: Module info.
 *
 * Pool metadata (pmd) module.
 *
 * Defines functions for probing, reading, and writing drives in an mpool.
 *
 */

#include <linux/log2.h>
#include <linux/string.h>
#include <linux/atomic.h>
#include <linux/rwsem.h>
#include <linux/mutex.h>
#include <linux/sort.h>
#include <linux/delay.h>

#include <mpcore/upgrade.h>
#include "mpcore_defs.h"

#include <mpool/mpool_ioctl.h>

#include "mpcore/qos.h"

/*
 * lock for serializing certain pmd ops where required/desirable; could be per
 * mpool but no meaningful performance benefit in doing so for these rare ops
 *
 */
static DEFINE_MUTEX(pmd_s_lock);


static merr_t
pmd_write_meta_to_latest_version(
	struct mpool_descriptor    *mp,
	bool                        permitted,
	struct mpool_devrpt        *devrpt);

static void pmd_mda_init(struct mpool_descriptor *mp)
{
	int             sidx = 0;

	/*
	 * Initialize all MDC_SLOTS entries so they are ready to use, excepting
	 * recbuf which gets allocated dynamically if slot is actually used.
	 */
	spin_lock_init(&mp->pds_mda.mdi_slotvlock);
	mp->pds_mda.mdi_lslot = 0;
	mp->pds_mda.mdi_slotvcnt = 0;

	for (sidx = 0; sidx < MDC_SLOTS; sidx++) {
		mutex_init(&mp->pds_mda.mdi_slotv[sidx].mmi_compactlock);
		mutex_init(&mp->pds_mda.mdi_slotv[sidx].mmi_uqlock);
		init_rwsem(&mp->pds_mda.mdi_slotv[sidx].mmi_colock);
		mutex_init(&mp->pds_mda.mdi_slotv[sidx].mmi_uncolock);
		mutex_init(&mp->pds_mda.mdi_slotv[sidx].mmi_reflock);
		mp->pds_mda.mdi_slotv[sidx].mmi_luniq = 0;
		mp->pds_mda.mdi_slotv[sidx].mmi_recbuf = NULL;
		mp->pds_mda.mdi_slotv[sidx].mmi_obj = RB_ROOT;
		mp->pds_mda.mdi_slotv[sidx].mmi_uncobj = RB_ROOT;
		mp->pds_mda.mdi_slotv[sidx].mmi_lckpt =
			objid_make(0, OMF_OBJ_UNDEF, sidx);
		memset(&mp->pds_mda.mdi_slotv[sidx].mmi_stats, 0,
		       sizeof(mp->pds_mda.mdi_slotv[sidx].mmi_stats));

		/*
		 * Initial mpool metadata content version.
		 */
		mp->pds_mda.mdi_slotv[sidx].mmi_mdccver.mv_mdccver_major = 1;
		mp->pds_mda.mdi_slotv[sidx].mmi_mdccver.mv_mdccver_minor = 0;
		mp->pds_mda.mdi_slotv[sidx].mmi_mdccver.mv_mdccver_patch = 0;
		mp->pds_mda.mdi_slotv[sidx].mmi_mdccver.mv_mdccver_dev   = 0;

		mp->pds_mda.mdi_slotv[sidx].mmi_credit.ci_slot  = sidx;

		mutex_init(&mp->pds_mda.mdi_slotv[sidx].mmi_stats_lock);

	}
	mp->pds_mda.mdi_slotv[1].mmi_luniq = UROOT_OBJID_MAX;

	mp->pds_mda.mdi_sel.mds_tbl_idx.counter = 0;
}

static merr_t
pmd_mdc0_init(
	struct mpool_descriptor        *mp,
	struct ecio_layout_descriptor  *mdc01,
	struct ecio_layout_descriptor  *mdc02)
{
	struct pmd_mdc_info            *cinfo = &mp->pds_mda.mdi_slotv[0];
	struct ecio_layout_descriptor  *found = NULL;
	merr_t                          err;

	cinfo->mmi_recbuf = kcalloc(OMF_MDCREC_PACKLEN_MAX, sizeof(char),
				GFP_KERNEL);
	if (!cinfo->mmi_recbuf) {
		err = merr(ENOMEM);
		mp_pr_err("mpool %s, log rec buffer alloc %lu failed",
			  err, mp->pds_name, (ulong)OMF_MDCREC_PACKLEN_MAX);
		return err;
	}

	/*
	 * we put the mdc0 mlog layouts in mdc 0 because mdc0 mlog objids have a
	 * slot # of 0 so the rest of the code expects to find the layout there.
	 * this allows the majority of the code to treat mdc0 mlog metadata
	 * exactly the same as for mdcN (and user mlogs), even though mdc0
	 * metadata is actually stored in superblocks.  however there are a few
	 * places that need to recognize mdc0 mlogs are special, including
	 * pmd_mdc_compact() and pmd_obj_erase().
	 */

	mp->pds_mda.mdi_slotvcnt = 1;
	objid_to_layout_insert_mdc(&cinfo->mmi_obj, mdc01);
	objid_to_layout_insert_mdc(&cinfo->mmi_obj, mdc02);

	err = mp_mdc_open(mp, mdc01->eld_objid, mdc02->eld_objid,
		MDC_OF_SKIP_SER, &cinfo->mmi_mdc);
	if (err) {
		mp_pr_err("mpool %s, MDC0 open failed",
			  err, mp->pds_name);

		found =	objid_to_layout_search_mdc(&cinfo->mmi_obj,
						   mdc01->eld_objid);
		if (found)
			rb_erase(&found->eld_nodemdc, &cinfo->mmi_obj);

		found =	objid_to_layout_search_mdc(&cinfo->mmi_obj,
						   mdc02->eld_objid);
		if (found)
			rb_erase(&found->eld_nodemdc, &cinfo->mmi_obj);

		kfree(cinfo->mmi_recbuf);
		cinfo->mmi_recbuf = NULL;

		mp->pds_mda.mdi_slotvcnt = 0;
	}

	return err;
}

static struct omf_mdcrec_data *
devid_to_dstate_mdcrec_search(struct rb_root *root, struct mpool_uuid *key_uuid)
{
	struct rb_node *node = root->rb_node;

	while (node) {
		struct  omf_mdcrec_data *data =
			rb_entry(node, struct omf_mdcrec_data, omd_node);
		int result = mpool_uuid_compare(
			key_uuid, &data->u.dev.omd_parm.odp_devid);

		if (result < 0)
			node = node->rb_left;
		else if (result > 0)
			node = node->rb_right;
		else
			return data;
	}
	return NULL;
}

static bool
devid_to_dstate_mdcrec_insert(
	struct rb_root         *root,
	struct omf_mdcrec_data *data)
{
	struct rb_node    **new = &(root->rb_node), *parent = NULL;

	/* Figure out where to put new node */
	while (*new) {
		struct omf_mdcrec_data *this =
			rb_entry(*new, struct omf_mdcrec_data, omd_node);
		int result = mpool_uuid_compare(
			&data->u.dev.omd_parm.odp_devid,
			&this->u.dev.omd_parm.odp_devid);
		parent = *new;
		if (result < 0)
			new = &((*new)->rb_left);
		else if (result > 0)
			new = &((*new)->rb_right);
		else
			return false;
	}

	/* Add new node and rebalance tree. */
	rb_link_node(&data->omd_node, parent, new);
	rb_insert_color(&data->omd_node, root);

	return true;
}

/**
 * pmd_cmp_drv_mdc0() - compare the drive info read from the MDC0 drive list
 *	to what is obtained from the drive itself or from the configuration.
 *
 *	The drive is in list passed to mpool open or an UNAVAIL mdc0 drive.
 *
 * @mp:
 * @uuid_idx_rb_elem:
 * @pdrec:
 * @devrpt:
 */
static merr_t
pmd_cmp_drv_mdc0(
	struct mpool_descriptor *mp,
	struct uuid_to_idx_rb   *uuid_idx_rb_elem,
	struct omf_mdcrec_data  *pdrec,
	struct mpool_devrpt	*devrpt)
{
	struct mpool_dev_info *pd;
	struct mc_parms        mcp_pd;
	struct mc_parms        mcp_mdc0list;

	u64    pdh;

	pdh = uuid_idx_rb_elem->uti_idx;
	pd = &mp->pds_pdv[pdh];

	pd->pdi_state = pdrec->u.dev.omd_state;

	mc_pd_prop2mc_parms(&(pd->pdi_parm.dpr_prop), &mcp_pd);
	mc_omf_devparm2mc_parms(&(pdrec->u.dev.omd_parm), &mcp_mdc0list);

	if (!memcmp(&mcp_pd, &mcp_mdc0list, sizeof(mcp_pd)))
		return 0;

	if (mpool_pd_status_get(pd) == PD_STAT_UNAVAIL)
		mp_pr_warn("mpool %s, UNAVAIL mdc0 drive parms don't match those in drive list record for %s, mclassp %d %d zonepg %u %u sectorsz %u %u devtype %u %u features %lu %lu",
			   mp->pds_name, pd->pdi_name,
			   mcp_pd.mcp_classp, mcp_mdc0list.mcp_classp,
			   mcp_pd.mcp_zonepg, mcp_mdc0list.mcp_zonepg,
			   mcp_pd.mcp_sectorsz, mcp_mdc0list.mcp_sectorsz,
			   mcp_pd.mcp_devtype, mcp_mdc0list.mcp_devtype,
			   (ulong)mcp_pd.mcp_features,
			   (ulong)mcp_mdc0list.mcp_features);
	else {
		mpool_devrpt(devrpt, MPOOL_RC_PARM, pdh, NULL);

		mp_pr_warn("mpool %s, mismatch between MDC0 drive list record and drive parms for %s, mclassp %d %d zonepg %u %u sectorsz %u %u devtype %u %u features %lu %lu",
			   mp->pds_name, pd->pdi_name,
			   mcp_pd.mcp_classp, mcp_mdc0list.mcp_classp,
			   mcp_pd.mcp_zonepg, mcp_mdc0list.mcp_zonepg,
			   mcp_pd.mcp_sectorsz, mcp_mdc0list.mcp_sectorsz,
			   mcp_pd.mcp_devtype, mcp_mdc0list.mcp_devtype,
			   (ulong)mcp_pd.mcp_features,
			   (ulong)mcp_mdc0list.mcp_features);
	}

	return merr(EINVAL);
}

static const char *msg_unavail1 __maybe_unused =
	"defunct and unavailable drive still belong to the mpool";

static const char *msg_unavail2 __maybe_unused =
	"defunct and available drive still belong to the mpool";

static merr_t
pmd_props_load(struct mpool_descriptor *mp, struct mpool_devrpt *devrpt)
{
	merr_t                  err;
	struct omf_mdcrec_data  cdr;
	struct pmd_mdc_info    *cinfo = NULL;
	u64                     buflen = 0;
	struct rb_root          netdev = RB_ROOT;
	size_t                  rlen = 0;
	struct omf_mdcrec_data *found_mr = NULL;
	struct omf_mdcrec_data *pdrec = NULL;
	char                    uuid_str[40];
	int                     i = 0;
	struct rb_node         *node = NULL;
	struct uuid_to_idx_rb  *uuid_idx_rb_elem = NULL;
	u64                     pdh = 0;
	enum mp_media_classp    mclassp;
	u8                      ftmax = 0;
	struct media_class      *mc;
	int                      spzone[MP_MED_NUMBER];

	cinfo = &mp->pds_mda.mdi_slotv[0];
	buflen = OMF_MDCREC_PACKLEN_MAX;

	/*  note: single threaded here so don't need any locks */

	/*
	 * set mpool properties to defaults; overwritten by property
	 * records (if any).
	 */
	for (mclassp = 0; mclassp < MP_MED_NUMBER; mclassp++)
		spzone[mclassp] = -1;

	/*
	 * read mdc0 to capture net of drives, content version & other
	 * properties; ignore obj records
	 */
	err = mp_mdc_rewind(cinfo->mmi_mdc);
	if (err) {
		mp_pr_err("mpool %s, MDC0 init for read properties failed",
			  err, mp->pds_name);
		return err;
	}

	while (true) {
		err = mp_mdc_read(cinfo->mmi_mdc, cinfo->mmi_recbuf, buflen,
				&rlen);
		if (err) {
			mp_pr_err("mpool %s, MDC0 read next failed %lu",
				  err, mp->pds_name, (ulong)rlen);
			break;
		}
		if (rlen == 0)
			/* hit end of log */
			break;

		/*
		 * skip object-related mdcrec in mdc0; not ready to unpack
		 * these yet
		 */
		if (omf_mdcrec_isobj_le(cinfo->mmi_recbuf))
			continue;

		err = omf_mdcrec_unpack_letoh(&(cinfo->mmi_mdccver), mp, &cdr,
			cinfo->mmi_recbuf);
		if (err) {
			mp_pr_err("mpool %s, MDC0 property unpack failed",
				  err, mp->pds_name);
			break;
		}

		if (cdr.omd_rtype == OMF_MDR_MCCONFIG) {
			found_mr = devid_to_dstate_mdcrec_search(
				&netdev, &cdr.u.dev.omd_parm.odp_devid);
			if (found_mr) {
				rb_erase(&found_mr->omd_node, &netdev);
				kfree(found_mr);
			}
			mpool_unparse_uuid(&cdr.u.dev.omd_parm.odp_devid,
					 uuid_str);

			pdrec = kmalloc(sizeof(struct omf_mdcrec_data),
					GFP_KERNEL);
			if (pdrec == NULL) {
				err = merr(ENOMEM);
				mp_pr_err("mpool %s, MDC0 getting property mcconfig, allocation omf_mdcrec_data failed",
					  err, mp->pds_name);
				break;
			}

			memcpy(pdrec, &cdr, sizeof(struct omf_mdcrec_data));
			devid_to_dstate_mdcrec_insert(&netdev, pdrec);
		} else if (cdr.omd_rtype == OMF_MDR_MCSPARE) {
			mp_pr_debug("Found spare record for mclassp %u",
				    0, cdr.u.mcs.omd_mclassp);
			mclassp = cdr.u.mcs.omd_mclassp;
			if (mclassp_valid(mclassp)) {
				spzone[mclassp] = cdr.u.mcs.omd_spzone;
			} else {
				err = merr(EINVAL);

				/* Should never happen */
				mp_pr_err("mpool %s, MDC0 getting property media class spare record, invalid mclassp %u",
					  err, mp->pds_name, mclassp);
				break;
			}
		} else if (cdr.omd_rtype == OMF_MDR_VERSION) {
			cinfo->mmi_mdccver = cdr.u.omd_version;
			if (upg_ver_cmp(&cinfo->mmi_mdccver, ">",
					upg_mdccver_latest())) {

				char	buf1[MAX_MDCCVERSTR];
				char	buf2[MAX_MDCCVERSTR];

				upg_mdccver2str(&cinfo->mmi_mdccver,
						buf1, sizeof(buf1));
				upg_mdccver2str(upg_mdccver_latest(),
						buf2, sizeof(buf2));

				mpool_devrpt(devrpt, MPOOL_RC_ERRMSG, -1,
					     "binary too old for metadata %s",
					     buf1);

				err = merr(EOPNOTSUPP);
				mp_pr_err("mpool %s, MDC0 content version is %s while binary understands up to %s",
					  err, mp->pds_name,
					  buf1, buf2);
				break;
			}

		} else if (cdr.omd_rtype == OMF_MDR_MPCONFIG) {
			mp->pds_cfg = cdr.u.omd_cfg;
		}
	}

	if (ev(err)) {
		/* remove all pdrec from netdev and dealloc */
		node = rb_first(&netdev);
		while (node) {
			found_mr = rb_entry(node, struct omf_mdcrec_data,
					    omd_node);
			node = rb_next(node);
			rb_erase(&found_mr->omd_node, &netdev);
			kfree(found_mr);
		}
		return err;
	}

	/* reconcile net drive list with those in mpool descriptor */
	for (i = 0; i < mp->pds_pdvcnt; i++) {
		/* using state as flag to identify zombie drives */
		mp->pds_pdv[i].pdi_state = OMF_PD_DEFUNCT;
	}

	for (node = rb_first(&netdev); node;
		     node = rb_next(node)) {
		pdrec = rb_entry(node, struct omf_mdcrec_data,
				 omd_node);
		if (pdrec->u.dev.omd_state != OMF_PD_DEFUNCT) {
			uuid_idx_rb_elem = uuid_to_idx_search(
				&mp->pds_dev2pdh,
				&pdrec->u.dev.omd_parm.odp_devid);
			if (uuid_idx_rb_elem) {
				err = pmd_cmp_drv_mdc0(mp, uuid_idx_rb_elem,
					pdrec, devrpt);
				if (ev(err))
					break;
			} else {
				/* drive is UNAVAIL; add to descriptor */
				err = mpool_desc_unavail_add(
					mp, pdrec->u.dev.omd_state,
					&pdrec->u.dev.omd_parm);
				if (ev(err))
					break;
			}
		}
	}

	/* check for zombie drives and recompute uacnt[] */
	if (!err) {
		for (i = 0; i < MP_MED_NUMBER; i++) {
			mc = &mp->pds_mc[i];
			mc->mc_uacnt = 0;
		}

		for (pdh = 0; pdh < mp->pds_pdvcnt; pdh++) {
			mc = &mp->pds_mc[mp->pds_pdv[pdh].pdi_mclass];
			if (mp->pds_pdv[pdh].pdi_state == OMF_PD_DEFUNCT) {
				char uuid_str[40];

				mpool_unparse_uuid(&mp->pds_pdv[pdh].pdi_devid,
					uuid_str);
				err = merr(ENXIO);

				if (mpool_pd_status_get(&mp->pds_pdv[pdh]) ==
				    PD_STAT_UNAVAIL)
					mp_pr_err("mpool %s, drive %s %s %s",
						  err, mp->pds_name, uuid_str,
						  mp->pds_pdv[pdh].pdi_name,
						  msg_unavail1);
				else {
					mpool_devrpt(devrpt, MPOOL_RC_ZOMBIE,
						     pdh, NULL);
					mp_pr_err("mpool %s, drive %s %s %s",
						  err, mp->pds_name, uuid_str,
						  mp->pds_pdv[pdh].pdi_name,
						  msg_unavail2);
				}
				break;
			} else if (mpool_pd_status_get(&mp->pds_pdv[pdh])
				   == PD_STAT_UNAVAIL) {
				mc->mc_uacnt += 1;
			}
		}

		if (!err) {
			ftmax = 0;
			for (i = 0; i < MP_MED_NUMBER; i++) {
				mc = &mp->pds_mc[i];
				ftmax = max(ftmax, mc->mc_uacnt);
			}
			if (ftmax >= MP_MED_NUMBER) {
				err = merr(EINVAL);
				mpool_devrpt(devrpt, MPOOL_RC_MDC, 0, NULL);
				mp_pr_err("mpool %s, not enough good drives %d",
					  err, mp->pds_name, ftmax);
			}
		}
	}

	/*
	 * reset DEFUNCT drive states to ACTIVE so devices gets closed on
	 * failure exit path if not UNAVAIL
	 */
	for (pdh = 0; pdh < mp->pds_pdvcnt; pdh++)
		if (mp->pds_pdv[pdh].pdi_state == OMF_PD_DEFUNCT)
			mp->pds_pdv[pdh].pdi_state = OMF_PD_ACTIVE;

	/*
	 * Now it is possible to update the percent spare because all
	 * the media classes of the mpool have been created because all
	 * the mpool PDs have been added in their classes.
	 */
	if (!err) {
		for (mclassp = 0; mclassp < MP_MED_NUMBER; mclassp++) {
			if (spzone[mclassp] >= 0) {
				err = mc_set_spzone(mp, mclassp,
						    spzone[mclassp]);
				/*
				 * Should never happen, it should exist a class
				 * with perf. level mclassp with a least 1 PD.
				 */
				if (ev(err))
					break;
			}
		}
		if (err)
			mp_pr_err("mpool %s, can't set percent spare %u because the class %u has no PD",
				  err, mp->pds_name, spzone[mclassp], mclassp);
	}

	/* remove pdrec from netdev and dealloc */
	node = rb_first(&netdev);
	while (node) {
		found_mr = rb_entry(node, struct omf_mdcrec_data,
				    omd_node);
		node = rb_next(node);
		rb_erase(&found_mr->omd_node, &netdev);
		kfree(found_mr);
	}

	return err;
}

static merr_t
pmd_smap_insert(
	struct mpool_descriptor        *mp,
	struct ecio_layout_descriptor  *layout)
{
	merr_t  err;
	u16     pdh;

	pdh = layout->eld_ld.ol_pdh;

	err = smap_insert(mp, pdh, layout->eld_ld.ol_zaddr,
			  layout->eld_ld.ol_zcnt);
	if (err) {
		/* insert should never fail */
		mp_pr_err("mpool %s, allocating drive %s space for layout failed, objid 0x%lx",
			  err, mp->pds_name, mp->pds_pdv[pdh].pdi_name,
			  (ulong)layout->eld_objid);
	}

	return err;
}

static void pmd_mdc_rdunlock(struct rw_semaphore *sem)
{
	up_read(sem);
}

static void pmd_mdc_rdlock(struct rw_semaphore *sem, u8 slot)
{
	down_read_nested(sem, slot > 0 ? PMD_MDC_NORMAL : PMD_MDC_ZERO);
}

static merr_t pmd_mdc0_validate(struct mpool_descriptor *mp, int activation)
{
	merr_t                          err = 0;
	u64                             mdcmax = 0;
	u16                             slotvcnt = 0;
	int                             i = 0;
	struct pmd_mdc_info            *cinfo = NULL;
	struct ecio_layout_descriptor  *layout = NULL;
	struct rb_node                 *node = NULL;
	merr_t                          err1 = 0;
	merr_t                          err2 = 0;
	u64                             mdcn = 0;
	u64                             logid1 = 0;
	u64                             logid2 = 0;
	u32                             lcnt[MDC_SLOTS];

	/* initialize lcnt */
	for (i = 0; i < MDC_SLOTS; i++)
		lcnt[i] = 0;

	/*
	 * called during mpool activation and mdc alloc because a failed
	 * mdc alloc can result in extraneous mdc mlog objects which if
	 * found we attempt to clean-up here. when called during activation
	 * we may need to adjust mp.mda. this is not so when called from
	 * mdc alloc and in fact decreasing slotvcnt post activation would
	 * violate a key invariant.
	 * note: activation is single-threaded and mdc alloc is serialized
	 * so the number of active mdc (slotvcnt) will not change concurrent
	 * with this fn
	 */
	spin_lock(&mp->pds_mda.mdi_slotvlock);
	slotvcnt = mp->pds_mda.mdi_slotvcnt;
	spin_unlock(&mp->pds_mda.mdi_slotvlock);

	if (!slotvcnt) {
		/* must be at least mdc0 */
		err = merr(EINVAL);
		mp_pr_err("mpool %s, no MDC0", err, mp->pds_name);
		return err;
	}

	cinfo = &mp->pds_mda.mdi_slotv[0];

	pmd_mdc_rdlock(&cinfo->mmi_colock, 0);

	for (node = rb_first(&cinfo->mmi_obj); node;
	     node = rb_next(node)) {
		layout = rb_entry(node, struct ecio_layout_descriptor,
				  eld_nodemdc);
		mdcn = objid_uniq(layout->eld_objid) >> 1;
		if (mdcn < MDC_SLOTS) {
			lcnt[mdcn] = lcnt[mdcn] + 1;
			mdcmax = max(mdcmax, mdcn);
		}
		if (mdcn >= MDC_SLOTS ||
		    lcnt[mdcn] > 2 ||
		    objid_type(layout->eld_objid) != OMF_OBJ_MLOG ||
		    objid_slot(layout->eld_objid)) {
			err = merr(EINVAL);
			mp_pr_err("mpool %s, MDC0 number of MDCs %lu %u or obj type inconsistent, objid 0x%lx",
				  err, mp->pds_name, (ulong)mdcn,
				  lcnt[mdcn], (ulong)layout->eld_objid);
			break;
		}
	}

	pmd_mdc_rdunlock(&cinfo->mmi_colock);

	if (ev(err))
		return err;

	if (!mdcmax) {
		/*
		 * trivial case of mdc0 only; no mdc alloc failure to
		 * clean-up
		 */
		if (lcnt[0] != 2 || slotvcnt != 1) {
			err = merr(EINVAL);
			mp_pr_err("mpool %s, inconsistent number of MDCs or slots %d %d",
				  err, mp->pds_name, lcnt[0], slotvcnt);
			return err;
		}

		return 0;
	}

	if ((mdcmax != (slotvcnt - 1)) && mdcmax != slotvcnt) {
		err = merr(EINVAL);

		/*
		 * mdcmax is normally slotvcnt-1; can be slotvcnt if
		 * mdc alloc failed
		 */
		mp_pr_err("mpool %s, inconsistent max number of MDCs %lu %u",
			  err, mp->pds_name, (ulong)mdcmax, slotvcnt);
		return err;
	}

	/* both logs must always exist below mdcmax */
	for (i = 0; i < mdcmax; i++) {
		if (lcnt[i] != 2) {
			err = merr(ENOENT);
			mp_pr_err("mpool %s, MDC0 missing mlogs %lu %d %u",
				  err, mp->pds_name, (ulong)mdcmax, i, lcnt[i]);
			return err;
		}
	}

	/* clean-up from failed mdc alloc if needed */
	if (lcnt[mdcmax] != 2 || mdcmax == slotvcnt) {
		/* note: if activation then mdcmax == slotvcnt-1 always */
		err1 = 0;
		err2 = 0;
		logid1 = logid_make(2 * mdcmax, 0);
		logid2 = logid_make(2 * mdcmax + 1, 0);

		layout = pmd_obj_find_get(mp, logid1);
		if (layout) {
			err1 = pmd_obj_delete(mp, layout);
			if (err1)
				mp_pr_err("mpool %s, MDC0 %d, can't delete mlog %lu %lu %u %u",
					  err1, mp->pds_name,
					  activation, (ulong)logid1,
					  (ulong)mdcmax, lcnt[mdcmax],
					  slotvcnt);
		}

		layout = pmd_obj_find_get(mp, logid2);
		if (layout) {
			err2 = pmd_obj_delete(mp, layout);
			if (err2)
				mp_pr_err("mpool %s, MDC0 %d, can't delete mlog %lu %lu %u %u",
					  err2, mp->pds_name,
					  activation, (ulong)logid2,
					  (ulong)mdcmax, lcnt[mdcmax],
					  slotvcnt);
		}

		if (activation) {
			/* mpool activation can ignore mdc alloc clean-up
			 * failures; single-threaded; don't need slotvlock
			 * or uqlock to adjust mda
			 */
			cinfo->mmi_luniq = mdcmax - 1;
			mp->pds_mda.mdi_slotvcnt = mdcmax;
			mp_pr_warn("mpool %s, MDC0 activation, mdc alloc recovery: uniq %llu slotvcnt %d",
				   mp->pds_name,
				   (unsigned long long)cinfo->mmi_luniq,
				   mp->pds_mda.mdi_slotvcnt);
		} else {
			/* mdc alloc cannot tolerate clean-up failures */
			if (err1)
				err = err1;
			else if (err2)
				err = err2;

			if (err)
				mp_pr_err("mpool %s, MDC0 alloc recovery, clean-up failure %lu %u %u",
					  err, mp->pds_name,
					  (ulong)mdcmax, lcnt[mdcmax],
					  slotvcnt);
			else
				mp_pr_warn("mpool %s, MDC0 alloc recovery",
					   mp->pds_name);

		}
	}

	return err;
}

/**
 * pmd_update_mdc_stats() - update per-MDC space usage
 * @mp:
 * @layout:
 * @cinfo:
 * @op: object opcode
 */
static merr_t
pmd_update_mdc_stats(
	struct mpool_descriptor        *mp,
	struct ecio_layout_descriptor  *layout,
	struct pmd_mdc_info            *cinfo,
	enum pmd_obj_op                 op)
{
	struct pmd_mdc_stats   *pms;
	enum obj_type_omf       otype;

	merr_t err = 0;
	u64    cap;

	otype = pmd_objid_type(layout->eld_objid);

	mutex_lock(&cinfo->mmi_stats_lock);
	pms = &cinfo->mmi_stats;

	/* Update space usage and mblock/mlog count */
	switch (op) {
	case PMD_OBJ_LOAD:
		if (otype == OMF_OBJ_MBLOCK)
			pms->pms_mblock_wlen += layout->eld_mblen;
		/* fall through */
	case PMD_OBJ_ALLOC:
		cap = ecio_obj_get_cap_from_layout(mp, layout);
		if (otype == OMF_OBJ_MLOG) {
			pms->pms_mlog_cnt++;
			pms->pms_mlog_alen += cap;
		} else if (otype == OMF_OBJ_MBLOCK) {
			pms->pms_mblock_cnt++;
			pms->pms_mblock_alen += cap;
		}
		break;
	case PMD_OBJ_COMMIT:
		if (otype == OMF_OBJ_MBLOCK)
			pms->pms_mblock_wlen += layout->eld_mblen;
		break;
	case PMD_OBJ_DELETE:
		if (otype == OMF_OBJ_MBLOCK)
			pms->pms_mblock_wlen -= layout->eld_mblen;
		/* fall through */
	case PMD_OBJ_ABORT:
		cap = ecio_obj_get_cap_from_layout(mp, layout);
		if (otype == OMF_OBJ_MLOG) {
			pms->pms_mlog_cnt--;
			pms->pms_mlog_alen -= cap;
		} else if (otype == OMF_OBJ_MBLOCK) {
			pms->pms_mblock_cnt--;
			pms->pms_mblock_alen -= cap;
		}
		break;
	default:
		assert(0);
	}

	mutex_unlock(&cinfo->mmi_stats_lock);

	return err;
}

static merr_t
pmd_objs_load(
	struct mpool_descriptor    *mp,
	u8                          cslot,
	struct mpool_devrpt        *devrpt)
{
	u64                         argv[2] = { 0 };
	struct omf_mdcrec_data      cdr;
	struct pmd_mdc_info        *cinfo;
	struct rb_root             *cobj;
	struct rb_node             *node;
	const char                 *msg;
	merr_t                      err;
	size_t                      recbufsz;
	char                       *recbuf;
	u64                         mdcmax;

	/* note: single threaded here so don't need any locks */

	recbufsz = OMF_MDCREC_PACKLEN_MAX;
	memset(&cdr, 0, sizeof(cdr));
	msg = "(no detail)";
	mdcmax = 0;

	cinfo = &mp->pds_mda.mdi_slotv[cslot];

	/* Initialize mdc if not mdc0.
	 */
	if (cslot) {
		u64 logid1 = logid_make(2 * cslot, 0);
		u64 logid2 = logid_make(2 * cslot + 1, 0);

		/* freed in pmd_mda_free() */
		cinfo->mmi_recbuf = kmalloc(recbufsz, GFP_KERNEL);
		if (!cinfo->mmi_recbuf) {
			msg = "MDC recbuf alloc failed";
			err = merr(ENOMEM);
			goto errout;
		}

		err = mp_mdc_open(mp, logid1, logid2, MDC_OF_SKIP_SER,
				  &cinfo->mmi_mdc);
		if (ev(err)) {
			msg = "mdc open failed";
			goto errout;
		}
	}

	/* Read mdc and capture net result of object data records.
	 */
	err = mp_mdc_rewind(cinfo->mmi_mdc);
	if (ev(err)) {
		msg = "mdc rewind failed";
		goto errout;
	}

	/* Cache these pointers to simplify the ensuing code.
	 */
	recbuf = cinfo->mmi_recbuf;
	cobj = &cinfo->mmi_obj;

	while (true) {
		struct ecio_layout_descriptor  *layout;
		struct ecio_layout_descriptor  *found;

		size_t rlen = 0;
		u64 objid;

		err = mp_mdc_read(cinfo->mmi_mdc, recbuf, recbufsz, &rlen);
		if (ev(err)) {
			msg = "mdc read data failed";
			break;
		}
		if (rlen == 0)
			break; /* hit end of log */

		/*
		 * Version record, if present, must be first.
		 */
		if (omf_mdcrec_unpack_type_letoh(recbuf) == OMF_MDR_VERSION) {
			omf_mdccver_unpack_letoh(&cdr, recbuf);
			cinfo->mmi_mdccver = cdr.u.omd_version;

			if (upg_ver_cmp(&cinfo->mmi_mdccver, ">",
					upg_mdccver_latest())) {

				char	buf1[MAX_MDCCVERSTR];
				char	buf2[MAX_MDCCVERSTR];

				upg_mdccver2str(&cinfo->mmi_mdccver,
						buf1, sizeof(buf1));
				upg_mdccver2str(upg_mdccver_latest(),
						buf2, sizeof(buf2));

				mpool_devrpt(devrpt, MPOOL_RC_ERRMSG, -1,
					     "binary too old for metadata %s",
					     buf1);

				err = merr(EOPNOTSUPP);
				mp_pr_err("mpool %s, MDC%u content version is %s while binary understands up to %s",
					  err, mp->pds_name, cslot,
					  buf1, buf2);
				break;
			}
			continue;
		}

		/* Skip non object-related mdcrec in mdc0; i.e., property
		 * records.
		 */
		if (!cslot && !omf_mdcrec_isobj_le(recbuf))
			continue;

		err = omf_mdcrec_unpack_letoh(&(cinfo->mmi_mdccver), mp, &cdr,
					      recbuf);
		if (ev(err)) {
			msg = "mlog record unpack failed";
			break;
		}

		objid = cdr.u.obj.omd_objid;

		if (objid_slot(objid) != cslot) {
			msg = "mlog record wrong slot";
			err = merr(EBADSLT);
			break;
		}

		if (cdr.omd_rtype == OMF_MDR_OCREATE) {
			layout = cdr.u.obj.omd_layout;
			layout->eld_state = ECIO_LYT_COMMITTED;

			found = objid_to_layout_insert_mdc(cobj, layout);
			if (found) {
				msg = "OCREATE duplicate object ID";
				ecio_layout_free(layout);
				err = merr(EEXIST);
				break;
			}

			atomic_inc(&cinfo->mmi_pco_cnt.pcc_cr);
			atomic_inc(&cinfo->mmi_pco_cnt.pcc_cobj);

			continue;
		}

		if (cdr.omd_rtype == OMF_MDR_ODELETE) {
			found = objid_to_layout_search_mdc(cobj, objid);
			if (!found) {
				msg = "ODELETE object not found";
				err = merr(ENOENT);
				break;
			}

			rb_erase(&found->eld_nodemdc, cobj);
			ecio_layout_free(found);

			atomic_inc(&cinfo->mmi_pco_cnt.pcc_del);
			atomic_dec(&cinfo->mmi_pco_cnt.pcc_cobj);

			continue;
		}

		if (cdr.omd_rtype == OMF_MDR_OIDCKPT) {
			/*
			 * objid == mmi_lckpt == 0 is legit. Such records
			 * are appended by mpool MDC compaction due to a
			 * mpool metadata upgrade on an empty mpool.
			 */
			if ((objid_uniq(objid) || objid_uniq(cinfo->mmi_lckpt))
				&& (objid_uniq(objid) <=
				objid_uniq(cinfo->mmi_lckpt))) {
				msg = "OIDCKPT cdr ckpt %lu <= cinfo ckpt %lu";
				argv[0] = objid_uniq(objid);
				argv[1] = objid_uniq(cinfo->mmi_lckpt);
				err = merr(EINVAL);
				break;
			}

			cinfo->mmi_lckpt = objid;
			continue;
		}

		if (cdr.omd_rtype == OMF_MDR_OERASE) {
			layout = objid_to_layout_search_mdc(cobj, objid);
			if (!layout) {
				msg = "OERASE object not found";
				err = merr(ENOENT);
				break;
			}

			/* Note: OERASE gen can equal layout gen after
			 * a compaction.
			 */
			if (cdr.u.obj.omd_gen < layout->eld_gen) {
				msg = "OERASE cdr gen %lu < layout gen %lu";
				argv[0] = cdr.u.obj.omd_gen;
				argv[1] = layout->eld_gen;
				err = merr(EINVAL);
				break;
			}

			layout->eld_gen = cdr.u.obj.omd_gen;

			atomic_inc(&cinfo->mmi_pco_cnt.pcc_er);
			continue;
		}

		if (cdr.omd_rtype == OMF_MDR_OUPDATE) {
			layout = cdr.u.obj.omd_layout;

			found = objid_to_layout_search_mdc(cobj, objid);
			if (!found) {
				msg = "OUPDATE object not found";
				ecio_layout_free(layout);
				err = merr(ENOENT);
				break;
			}

			rb_erase(&found->eld_nodemdc, cobj);
			ecio_layout_free(found);

			layout->eld_state = ECIO_LYT_COMMITTED;
			objid_to_layout_insert_mdc(cobj, layout);

			atomic_inc(&cinfo->mmi_pco_cnt.pcc_up);

			continue;
		}
	}

	if (ev(err))
		goto errout;

	/*
	 * Add all existing objects to space map.
	 * Also add/update per-mpool space usage stats
	 */
	for (node = rb_first(cobj); node; node = rb_next(node)) {
		struct ecio_layout_descriptor *layout;

		layout = rb_entry(node, struct ecio_layout_descriptor,
				  eld_nodemdc);

		/* Remember objid and gen in case of error...
		 */
		cdr.u.obj.omd_objid = layout->eld_objid;
		cdr.u.obj.omd_gen = layout->eld_gen;

		if (objid_slot(layout->eld_objid) != cslot) {
			msg = "layout wrong slot";
			err = merr(EBADSLT);
			break;
		}

		err = pmd_smap_insert(mp, layout);
		if (ev(err)) {
			msg = "smap insert failed";
			break;
		}

		err = pmd_update_mdc_stats(mp, layout, cinfo, PMD_OBJ_LOAD);
		if (err) {
			msg = "alloc per-mdc space usage stats failed";
			err = merr(ENOMEM);
			break;
		}

		/* For mdc0 track last logical mdc created.
		 */
		if (!cslot)
			mdcmax = max(mdcmax,
				     (objid_uniq(layout->eld_objid) >> 1));
	}

	if (ev(err))
		goto errout;

	cdr.u.obj.omd_objid = 0;
	cdr.u.obj.omd_gen = 0;

	if (!cslot) {
		/* mdc0: finish initializing mda */
		cinfo->mmi_luniq = mdcmax;
		mp->pds_mda.mdi_slotvcnt = mdcmax + 1;
		mp->pds_mda.mdi_slotvcnt_shift = 0;
		if (mdcmax > 1)
			mp->pds_mda.mdi_slotvcnt_shift =
				__builtin_ctz(roundup_pow_of_two(mdcmax));


		/* mdc0 only: validate other mdc metadata; may make adjustments
		 * to mp.mda.
		 */
		err = pmd_mdc0_validate(mp, 1);
		if (ev(err))
			msg = "MDC0 validation failed";
	} else {
		/*
		 * other mdc: set luniq to guaranteed max value
		 * previously used and ensure next objid allocation
		 * will be checkpointed; supports realloc of
		 * uncommitted objects after a crash
		 */
		cinfo->mmi_luniq = objid_uniq(cinfo->mmi_lckpt) +
			OBJID_UNIQ_DELTA - 1;
	}

errout:
	if (err) {
		char msgbuf[64];

		snprintf(msgbuf, sizeof(msgbuf), msg, argv[0], argv[1]);

		mp_pr_err("mpool %s, %s: cslot %u, ckpt %lx, %lx/%lu",
			  err, mp->pds_name, msgbuf,
			  cslot, (ulong)cinfo->mmi_lckpt,
			  (ulong)cdr.u.obj.omd_objid,
			  (ulong)cdr.u.obj.omd_gen);
	}

	return err;
}

void pmd_mda_free(struct mpool_descriptor *mp)
{
	int                             sidx = 0;
	struct pmd_mdc_info            *cinfo = NULL;
	struct rb_node                 *node = NULL;
	struct ecio_layout_descriptor  *layout = NULL;

	/*
	 * close mdc0 last because closing other mdc logs can result in
	 * mdc0 updates
	 */

	/* since caller of this fn will call smap_mpool_free() */
	for (sidx = mp->pds_mda.mdi_slotvcnt - 1; sidx > -1; sidx--) {
		cinfo = &mp->pds_mda.mdi_slotv[sidx];

		mp_mdc_close(cinfo->mmi_mdc);
		kfree(cinfo->mmi_recbuf);
		cinfo->mmi_recbuf = NULL;

		/* free cinfo->mmi_obj objects */
		node = rb_first(&cinfo->mmi_obj);
		while (node) {
			layout = rb_entry(node, struct ecio_layout_descriptor,
					  eld_nodemdc);
			node = rb_next(node);
			rb_erase(&layout->eld_nodemdc, &cinfo->mmi_obj);

			ecio_layout_free(layout);
		}

		/* free cinfo->mmi_uncobj objects */
		node = rb_first(&cinfo->mmi_uncobj);

		while (node) {
			layout = rb_entry(node, struct ecio_layout_descriptor,
					  eld_nodemdc);
			node = rb_next(node);
			rb_erase(&layout->eld_nodemdc, &cinfo->mmi_uncobj);

			ecio_layout_free(layout);
		}
	}
}

/**
 * pmd_objs_load_worker() -
 * @ws:
 *
 * worker thread for loading user MDC 1~N
 * Each worker instance will do the following (not counting errors):
 * * grab an MDC number atomically from olw->olw_progress
 * * If the MDC number is invalid, exit
 * * load the objects from that MDC
 *
 * If an error occurs in this or any other worker, don't load any more MDCs
 */
static void pmd_objs_load_worker(struct work_struct *ws)
{
	struct pmd_obj_load_work       *olw;
	int                             sidx;
	merr_t                          err;

	olw = container_of(ws, struct pmd_obj_load_work, olw_work);

	while (1) {
		if (*olw->olw_err)
			break; /* Stop, another worker it an error */

		sidx = atomic_fetch_add(1, olw->olw_progress);
		if (sidx >= olw->olw_mp->pds_mda.mdi_slotvcnt)
			break; /* No more MDCs to load */

		err = pmd_objs_load(olw->olw_mp, sidx, &olw->olw_devrpt);
		if (ev(err)) {
			*olw->olw_err = err;
			break;
		}
	}
}

/**
 * pmd_objs_load_parallel() - load MDC 1~N in parallel
 * @mp:
 * @devrpt:
 *
 * By loading user MDCs in parallel, we can reduce the mpool activate
 * time, since the jobs of loading MDC 1~N are independent.
 * On the other hand, we don't want to start all the jobs at once.
 * If any one fails, we don't have to start others.
 */
static merr_t
pmd_objs_load_parallel(struct mpool_descriptor *mp, struct mpool_devrpt *devrpt)
{
	struct pmd_obj_load_work   *olwv;
	volatile merr_t             err = 0;

	atomic_t    progress = ATOMIC_INIT(1);
	uint        njobs, i;

	if (mp->pds_mda.mdi_slotvcnt < 2)
		return 0; /* No user MDCs allocated */

	njobs = mp->pds_params.mp_objloadjobs;
	njobs = clamp_t(uint, njobs, 1, mp->pds_mda.mdi_slotvcnt - 1);

	olwv = kcalloc(njobs, sizeof(*olwv), GFP_KERNEL);
	if (!olwv)
		return merr(ENOMEM);

	/* Each of njobs workers will atomically grab MDC numbers from &progress
	 * and load them, until all valid user MDCs have been loaded.
	 */
	for (i = 0; i < njobs; i++) {
		INIT_WORK(&olwv[i].olw_work, pmd_objs_load_worker);
		olwv[i].olw_progress = &progress;
		olwv[i].olw_err = &err;
		olwv[i].olw_mp = mp;
		queue_work(mp->pds_workq, &olwv[i].olw_work);
	}

	/* Wait for all worker threads to complete */
	flush_workqueue(mp->pds_workq);

	if (ev(err)) {
		/* Update devrpt passed in. */
		for (i = 0; i < njobs; i++)
			if (olwv[i].olw_devrpt.mdr_rcode) {
				*devrpt = olwv[i].olw_devrpt;
				break;
			}
	}

	kfree(olwv);

	return err;
}

merr_t
pmd_mpool_activate(
	struct mpool_descriptor        *mp,
	struct ecio_layout_descriptor  *mdc01,
	struct ecio_layout_descriptor  *mdc02,
	int                             create,
	struct mpool_devrpt            *devrpt,
	u32                             flags)
{
	merr_t  err;

	mp_pr_debug("mdc01: %lu mdc02: %lu",
		    0, (ulong)mdc01->eld_objid,
		    (ulong)mdc02->eld_objid);

	/* activation is intense; serialize it when have multiple mpools */
	mutex_lock(&pmd_s_lock);

	/* init metadata array for mpool */
	pmd_mda_init(mp);

	/* initialize mdc0 for mpool */
	err = pmd_mdc0_init(mp, mdc01, mdc02);
	if (ev(err)) {
		/*
		 * pmd_mda_free() will dealloc mdc01/2 on subsequent
		 * activation failures
		 */
		ecio_layout_free(mdc01);
		ecio_layout_free(mdc02);
		goto exit;
	}

	/* load mpool properties from mdc0 including drive list and states */
	if (!create) {
		err = pmd_props_load(mp, devrpt);
		if (ev(err))
			goto exit;
	}

	/*
	 * initialize smaps for all drives in mpool (now that list
	 * is finalized)
	 */
	err = smap_mpool_init(mp);
	if (ev(err))
		goto exit;

	/* load mdc layouts from mdc0 and finalize mda initialization */
	err = pmd_objs_load(mp, 0, devrpt);
	if (ev(err))
		goto exit;

	/* load user object layouts from all other mdc */
	err = pmd_objs_load_parallel(mp, devrpt);
	if (ev(err)) {
		mp_pr_err("mpool %s, failed to load user MDCs",
			  err, mp->pds_name);
		goto exit;
	}

	/*
	 * If the format of the mpool metadata read from media during activate
	 * is not the latest, it is time to write the metadata on media with
	 * the latest format.
	 */
	if (!create) {
		err = pmd_write_meta_to_latest_version(mp, true, devrpt);
		if (ev(err)) {
			mp_pr_err("mpool %s, failed to compact MDCs (because of metadata conversion)",
				  err, mp->pds_name);
			goto exit;
		}
	}
exit:
	if (err) {
		/* activation failed; cleanup */
		pmd_mda_free(mp);
		smap_mpool_free(mp);
	}

	mutex_unlock(&pmd_s_lock);
	return err;
}

void pmd_mpool_deactivate(struct mpool_descriptor *mp)
{
	/* deactivation is intense; serialize it when have multiple mpools */
	mutex_lock(&pmd_s_lock);

	/* close all open user (non-mdc) mlogs */
	mlogutil_closeall(mp);

	pmd_mda_free(mp);
	smap_mpool_free(mp);

	mutex_unlock(&pmd_s_lock);
}

merr_t
pmd_obj_alloc(
	struct mpool_descriptor        *mp,
	enum obj_type_omf               otype,
	struct pmd_obj_capacity        *ocap,
	enum mp_media_classp            mclassp,
	struct ecio_layout_descriptor **olayout)
{
	return pmd_obj_alloc_cmn(mp, 0, otype, ocap, mclassp, 0, olayout);
}

merr_t
pmd_obj_realloc(
	struct mpool_descriptor        *mp,
	u64                             objid,
	struct pmd_obj_capacity        *ocap,
	enum mp_media_classp            mclassp,
	struct ecio_layout_descriptor **olayout)
{
	merr_t err;

	if (!pmd_objid_isuser(objid)) {
		*olayout = NULL;
		err = merr(EINVAL);
		mp_pr_err("mpool %s, re-allocation of an object is only authorized for an application object",
			  err, mp->pds_name);
		return err;
	}

	return pmd_obj_alloc_cmn(mp, objid, objid_type(objid),
				 ocap, mclassp, 1, olayout);
}

u64 pmd_objid_to_uhandle(u64 objid)
{
	return omf_objid_to_uhandle(objid);
}

u64 pmd_uhandle_to_objid(u64 uhandle)
{
	return omf_uhandle_to_objid(uhandle);
}

void pmd_mdc_lock(struct mutex *lock, u8 slot)
{
	mutex_lock_nested(lock, slot > 0 ? PMD_MDC_NORMAL : PMD_MDC_ZERO);
}

void pmd_mdc_unlock(struct mutex *lock)
{
	mutex_unlock(lock);
}

/*
 * Nesting levels for pmd_mdc_info rwsem.
 */

static void pmd_mdc_wrlock(struct rw_semaphore *sem, u8 slot)
{
	down_write_nested(sem, slot > 0 ? PMD_MDC_NORMAL : PMD_MDC_ZERO);
}

static void pmd_mdc_wrunlock(struct rw_semaphore *sem)
{
	up_write(sem);
}

merr_t
pmd_mdc_append(
	struct mpool_descriptor    *mp,
	u8                          cslot,
	struct omf_mdcrec_data     *cdr,
	int                         sync)
{
	merr_t                  err;
	struct pmd_mdc_info    *cinfo = &mp->pds_mda.mdi_slotv[cslot];
	s64                     plen;

	plen = omf_mdcrec_pack_htole(mp, cdr, cinfo->mmi_recbuf);
	if (plen < 0) {
		mp_pr_warn("mpool %s, MDC%u append failed",
			   mp->pds_name, cslot);
		return plen;
	}

	err = mp_mdc_append(cinfo->mmi_mdc, cinfo->mmi_recbuf, plen, sync);
	ev(err);

	return err;
}

/**
 * pmd_log_all_mdc_cobjs() - write in the new active mlog the object records.
 * @mp:
 * @cslot:
 * @compacted: output
 * @total: output
 */
static merr_t
pmd_log_all_mdc_cobjs(
	struct mpool_descriptor    *mp,
	u8                          cslot,
	u32                        *compacted,
	u32                        *total)
{
	merr_t                          err = 0;
	struct pmd_mdc_info            *cinfo = &mp->pds_mda.mdi_slotv[cslot];
	struct ecio_layout_descriptor  *layout;
	struct omf_mdcrec_data          cdr;
	struct rb_node                 *node;

	for (node = rb_first(&cinfo->mmi_obj); node; node = rb_next(node)) {
		layout = rb_entry(node, struct ecio_layout_descriptor,
				  eld_nodemdc);
		if (!objid_mdc0log(layout->eld_objid)) {
			cdr.omd_rtype = OMF_MDR_OCREATE;
			cdr.u.obj.omd_layout = layout;
			err = pmd_mdc_append(mp, cslot, &cdr, 0);
			if (err) {
				mp_pr_err("mpool %s, MDC%u log committed object failed, objid 0x%lx",
					  err, mp->pds_name, cslot,
					  (ulong)layout->eld_objid);
				break;
			}

			++(*compacted);
		}
		++(*total);
	}

	for (; node; node = rb_next(node))
		++(*total);

	return err;
}

/**
 * pmd_log_mdc0_cobjs() - write in the new active mlog (of MDC0) the MDC0
 *	records that are particular to MDC0.
 * @mp:
 */
static merr_t pmd_log_mdc0_cobjs(struct mpool_descriptor *mp)
{
	struct mpool_dev_info    *pd;
	merr_t                    err = 0;
	int                       i;
	/*
	 * Log a drive record (OMF_MDR_MCCONFIG) for every drive in pds_pdv[]
	 * that is not defunct.
	 */
	for (i = 0; i < mp->pds_pdvcnt; i++) {
		pd = &(mp->pds_pdv[i]);
		if (pd->pdi_state != OMF_PD_DEFUNCT) {
			err = pmd_prop_mcconfig(mp, pd, true);
			if (ev(err))
				return err;
		}
	}

	/*
	 * Log a media class spare record (OMF_MDR_MCSPARE) for every media
	 * class.
	 * mc count can't change now. Because the MDC0 compact lock is held
	 * and that blocks the addition of PDs in the  mpool.
	 */
	for (i = 0; i < MP_MED_NUMBER; i++) {
		struct media_class *mc;

		mc = &mp->pds_mc[i];
		if (mc->mc_pdmc >= 0) {
			err = pmd_prop_mcspare(mp, mc->mc_parms.mcp_classp,
				mc->mc_sparms.mcsp_spzone, true);
			if (ev(err))
				return err;
		}
	}

	err = pmd_prop_mpconfig(mp, &mp->pds_cfg, true);
	if (ev(err))
		return err;

	return 0;
}

/**
 * pmd_log_non_mdc0_cobjs() - write in the new active mlog (of MDCi i>0) the
 *	MDCi records that are particular to MDCi (not used by MDC0).
 * @mp:
 * @cslot:
 */
static merr_t pmd_log_non_mdc0_cobjs(struct mpool_descriptor *mp, u8 cslot)
{
	struct omf_mdcrec_data  cdr;
	struct pmd_mdc_info    *cinfo;
	merr_t err;

	cinfo = &mp->pds_mda.mdi_slotv[cslot];
	/*
	 * if not mdc0 log last objid checkpoint to support realloc of
	 * uncommitted objects after a crash and to guarantee objids are
	 * never reused.
	 */
	cdr.omd_rtype = OMF_MDR_OIDCKPT;
	cdr.u.obj.omd_objid = cinfo->mmi_lckpt;
	err = pmd_mdc_append(mp, cslot, &cdr, 0);
	ev(err);

	return err;
}

/**
 * pmd_pre_compact_reset() - called on MDCi i>0
 * @cinfo:
 * @compacted: object create records appended in the new active mlog.
 *
 * Locking:
 *	MDCi compact lock is held by the caller.
 */
static void pmd_pre_compact_reset(struct pmd_mdc_info *cinfo, u32 compacted)
{
	struct pre_compact_ctrs    *pco_cnt;

	pco_cnt = &cinfo->mmi_pco_cnt;
	assert(pco_cnt->pcc_cobj.counter == compacted);
	atomic_set(&pco_cnt->pcc_cr, compacted);
	atomic_set(&pco_cnt->pcc_cobj, compacted);
	atomic_set(&pco_cnt->pcc_up, 0);
	atomic_set(&pco_cnt->pcc_del, 0);
	atomic_set(&pco_cnt->pcc_er, 0);
}

/**
 * pmd_mdc_compact() - compact an mpool MDCi with i >= 0.
 * @mp:
 * @cslot: the "i" of MDCi
 *
 * Locking:
 * 1) caller must hold MDCi compact lock
 * 2) MDC compaction freezes the state of all MDCs objects [and for MDC0
 *    also freezes all mpool properties] by simply holding MDC
 *    mmi_compactlock mutex. Hence, MDC compaction does not need to
 *    read-lock individual object layouts or mpool property data
 *    structures to read them. It is why this function and its callees don't
 *    take any lock.
 *
 * Note: this function or its callees must call pmd_mdc_append() with no sync
 *	instead of pmd_mdc_addrec() to avoid trigerring nested compaction of
 *	a same MDCi.
 *	The sync/flush is done by append of cend, no need to sync before that.
 */
static merr_t pmd_mdc_compact(struct mpool_descriptor *mp, u8 cslot)
{
	u64                     logid1 = logid_make(2 * cslot, 0);
	u64                     logid2 = logid_make(2 * cslot + 1, 0);
	struct pmd_mdc_info    *cinfo = &mp->pds_mda.mdi_slotv[cslot];
	int                     retry = 0;
	merr_t                  err = 0;

	for (retry = 0; retry < MPOOL_MDC_COMPACT_RETRY_DEFAULT; retry++) {
		u32 compacted = 0;
		u32 total = 0;

		if (err) {
			err = mp_mdc_open(mp, logid1, logid2,
					MDC_OF_SKIP_SER, &cinfo->mmi_mdc);
			if (ev(err))
				continue;
		}

		mp_pr_debug("mpool %s, MDC%u start: mlog1 gen %lu mlog2 gen %lu",
			    err, mp->pds_name, cslot,
			    (ulong)((struct ecio_layout_descriptor *)
				    cinfo->mmi_mdc->mdc_logh1)->eld_gen,
			    (ulong)((struct ecio_layout_descriptor *)
				    cinfo->mmi_mdc->mdc_logh2)->eld_gen);

		err = mp_mdc_cstart(cinfo->mmi_mdc);
		if (ev(err))
			continue;

		if (upg_ver_cmp2(upg_mdccver_latest(), ">=", 1, 0, 0, 1)) {
			err = pmd_mdc_addrec_version(mp, cslot);
			if (ev(err)) {
				mp_mdc_close(cinfo->mmi_mdc);
				continue;
			}
		}

		if (cslot)
			err = pmd_log_non_mdc0_cobjs(mp, cslot);
		else
			err = pmd_log_mdc0_cobjs(mp);
		if (ev(err))
			continue;

		err = pmd_log_all_mdc_cobjs(mp, cslot, &compacted, &total);

		mp_pr_debug("mpool %s, MDC%u compacted %u of %u objects: retry=%d",
			    err, mp->pds_name, cslot, compacted, total, retry);

		if (!ev(err))
			/*
			 * Append the compaction end record in the new active
			 * mlog, and flush/sync all the previous records
			 * appended in the new active log by the compaction
			 * above.
			 */
			err = mp_mdc_cend(cinfo->mmi_mdc);
		if (!ev(err)) {
			if (cslot) {
				/*
				 * MDCi i>0 compacted successfully
				 * MDCi compact lock is held.
				 */
				pmd_pre_compact_reset(cinfo, compacted);
			}

			mp_pr_debug("mpool %s, MDC%u end: mlog1 gen %lu mlog2 gen %lu",
				    err, mp->pds_name, cslot,
				    (ulong)
				    ((struct ecio_layout_descriptor *)
				     cinfo->mmi_mdc->mdc_logh1)->eld_gen,
				    (ulong)
				    ((struct ecio_layout_descriptor *)
				     cinfo->mmi_mdc->mdc_logh2)->eld_gen);
			break;
		}
	}

	if (err)
		mp_pr_crit("mpool %s, MDC%u compaction failed",
			   err, mp->pds_name, cslot);

	return err;
}

static merr_t
pmd_mdc_addrec(
	struct mpool_descriptor    *mp,
	u8                          cslot,
	struct omf_mdcrec_data     *cdr)
{
	merr_t err;

	err = pmd_mdc_append(mp, cslot, cdr, 1);

	if (merr_errno(err) == EFBIG) {
		err = pmd_mdc_compact(mp, cslot);
		if (!ev(err))
			err = pmd_mdc_append(mp, cslot, cdr, 1);
	}

	if (err)
		mp_pr_rl("mpool %s, MDC%u append failed%s",
			 err, mp->pds_name, cslot,
			 (merr_errno(err) == EFBIG) ?
			 " post compaction" : "");

	return err;
}

static merr_t
pmd_log_create(
	struct mpool_descriptor        *mp,
	struct ecio_layout_descriptor  *layout)
{
	struct omf_mdcrec_data  cdr;

	cdr.omd_rtype = OMF_MDR_OCREATE;
	cdr.u.obj.omd_layout = layout;
	return pmd_mdc_addrec(mp, objid_slot(layout->eld_objid), &cdr);
}

merr_t
pmd_obj_commit(
	struct mpool_descriptor        *mp,
	struct ecio_layout_descriptor  *layout)
{
	struct pmd_mdc_info           *cinfo;
	merr_t                         err;
	u8                             cslot;
	struct ecio_layout_descriptor *found;

	pmd_obj_wrlock(mp, layout);
	if (!objtype_user(objid_type(layout->eld_objid))) {
		pmd_obj_wrunlock(mp, layout);

		err = merr(EINVAL);
		mp_pr_err("mpool %s, wrong object type, commit failed, objid 0x%lx",
			  err, mp->pds_name, (ulong)layout->eld_objid);
		return err;
	} else if (layout->eld_state & ECIO_LYT_COMMITTED) {
		pmd_obj_wrunlock(mp, layout);

		mp_pr_warn("mpool %s, object already committed, state 0x%x",
			   mp->pds_name, layout->eld_state);
		return 0;
	}

	/*
	 * must log create before marking object committed to guarantee it will
	 * exist after a crash; must hold cinfo.compactclock while log create,
	 * update layout.state, and add to list of committed objects to prevent
	 * a race with mdc compaction
	 */
	cslot = objid_slot(layout->eld_objid);
	cinfo = &mp->pds_mda.mdi_slotv[cslot];

	pmd_mdc_lock(&cinfo->mmi_compactlock, cslot);

	err = pmd_log_create(mp, layout);
	if (!ev(err)) {

		layout->eld_state |= ECIO_LYT_COMMITTED;

		pmd_mdc_lock(&cinfo->mmi_uncolock, cslot);
		found =	objid_to_layout_search_mdc(&cinfo->mmi_uncobj,
						   layout->eld_objid);
		if (found)
			rb_erase(&found->eld_nodemdc, &cinfo->mmi_uncobj);
		pmd_mdc_unlock(&cinfo->mmi_uncolock);

		pmd_mdc_wrlock(&cinfo->mmi_colock, cslot);
		found = objid_to_layout_insert_mdc(&cinfo->mmi_obj, layout);
		pmd_mdc_wrunlock(&cinfo->mmi_colock);

		if (found) {
			err = merr(EEXIST);

			/*
			 * if objid exists in committed object list this is a
			 * SERIOUS bug; need to log a warning message; should
			 * never happen. Note in this case we are stuck because
			 * we just logged a second create for an existing
			 * object.  If mdc compaction runs before a restart this
			 * extraneous create record will be eliminated,
			 * otherwise pmd_objs_load() will see the conflict and
			 * fail the next mpool activation.  We could make
			 * pmd_objs_load() tolerate this but for now it is
			 * better to get an activation failure so that
			 * it's obvious this bug occurred. Best we can do is put
			 * the layout back in the uncommitted object list so the
			 * caller can abort after getting the commit failure.
			 */
			mp_pr_crit("mpool %s, obj 0x%lx collided during commit",
				   err, mp->pds_name, (ulong)layout->eld_objid);

			/* Put the object back in the uncommited objects tree */
			pmd_mdc_lock(&cinfo->mmi_uncolock, cslot);
			objid_to_layout_insert_mdc(&cinfo->mmi_uncobj, layout);

			pmd_mdc_unlock(&cinfo->mmi_uncolock);
		} else {
			atomic_inc(&cinfo->mmi_pco_cnt.pcc_cr);
			atomic_inc(&cinfo->mmi_pco_cnt.pcc_cobj);
		}
	}

	pmd_mdc_unlock(&cinfo->mmi_compactlock);
	pmd_obj_wrunlock(mp, layout);

	if (!err)
		pmd_update_mdc_stats(mp, layout, cinfo, PMD_OBJ_COMMIT);

	return err;
}

static void pmd_obj_erase_cb(struct work_struct *work)
{
	struct mpool_descriptor        *mp;
	struct ecio_layout_descriptor  *layout;
	struct ecio_err_report          erpt;
	enum obj_type_omf               otype;
	struct pmd_obj_erase_work      *oef;

	oef = container_of(work, struct pmd_obj_erase_work, oef_wqstruct);
	mp = oef->oef_mp;
	layout = oef->oef_layout;

	otype = pmd_objid_type(layout->eld_objid);
	if (otype == OMF_OBJ_MLOG)
		/* discard is advisory and no need to check the result */
		ecio_mlog_erase(mp, layout, 0, &erpt);
	else if (otype == OMF_OBJ_MBLOCK)
		ecio_mblock_erase(mp, layout, &erpt);

	if (oef->oef_cache)
		kmem_cache_free(oef->oef_cache, oef);

	pmd_layout_free(mp, layout);
}

static void
pmd_obj_erase_start(
	struct mpool_descriptor        *mp,
	struct ecio_layout_descriptor  *layout)
{
	struct pmd_obj_erase_work   oefbuf, *oef;
	bool                        async = true;

	oef = kmem_cache_zalloc(pmd_obj_erase_work_cache, GFP_KERNEL);
	if (!oef) {
		oef = &oefbuf;
		async = false;
	}

	/* If async oef will be freed in pmd_obj_erase_and_free() */
	oef->oef_mp = mp;
	oef->oef_layout = layout;
	oef->oef_cache = async ? pmd_obj_erase_work_cache : NULL;
	INIT_WORK(&oef->oef_wqstruct, pmd_obj_erase_cb);

	queue_work(mp->pds_erase_wq, &oef->oef_wqstruct);

	if (!async)
		flush_work(&oef->oef_wqstruct);
}

merr_t
pmd_obj_abort(
	struct mpool_descriptor        *mp,
	struct ecio_layout_descriptor  *layout)
{
	struct pmd_mdc_info            *cinfo = NULL;
	struct ecio_layout_descriptor  *found;
	u8                              cslot;
	merr_t                          err;

	pmd_obj_wrlock(mp, layout);

	if (!objtype_user(objid_type(layout->eld_objid)) ||
	    (layout->eld_state & ECIO_LYT_COMMITTED)) {
		pmd_obj_wrunlock(mp, layout);

		err = merr(EINVAL);
		mp_pr_rl("mpool %s, abort failed objid %lx, state 0x%x",
			 err, mp->pds_name, (ulong)layout->eld_objid,
			 layout->eld_state);

		return err;
	}

	cslot = objid_slot(layout->eld_objid);
	cinfo = &mp->pds_mda.mdi_slotv[cslot];

	pmd_mdc_lock(&cinfo->mmi_reflock, cslot);
	if (layout->eld_isdel || layout->eld_refcnt > 2) {
		int rc = layout->eld_isdel ? EINVAL : EBUSY;

		pmd_mdc_unlock(&cinfo->mmi_reflock);
		pmd_obj_wrunlock(mp, layout);

		err = merr(rc);
		mp_pr_rl("mpool %s, abort failed objid %lx, state 0x%x, refcnt %ld, isdel %d",
			 err, mp->pds_name, (ulong)layout->eld_objid,
			 layout->eld_state,
			 layout->eld_refcnt, layout->eld_isdel);

		return err;
	}

	layout->eld_refcnt = 0;
	layout->eld_isdel = true;
	layout->eld_state |= ECIO_LYT_REMOVED;
	pmd_mdc_unlock(&cinfo->mmi_reflock);

	pmd_mdc_lock(&cinfo->mmi_uncolock, cslot);
	found = objid_to_layout_search_mdc(&cinfo->mmi_uncobj,
					   layout->eld_objid);
	if (found)
		rb_erase(&found->eld_nodemdc, &cinfo->mmi_uncobj);
	pmd_mdc_unlock(&cinfo->mmi_uncolock);

	pmd_obj_wrunlock(mp, layout);

	pmd_update_mdc_stats(mp, layout, cinfo, PMD_OBJ_ABORT);

	pmd_obj_erase_start(mp, layout);

	return 0;
}

merr_t
pmd_log_delete(
	struct mpool_descriptor    *mp,
	u64                         objid)
{
	struct omf_mdcrec_data  cdr;

	cdr.omd_rtype = OMF_MDR_ODELETE;
	cdr.u.obj.omd_objid = objid;
	return pmd_mdc_addrec(mp, objid_slot(objid), &cdr);
}

/*
 * The internal flag allows deletion of objects with a zero refcount.
 */
merr_t
pmd_obj_delete_impl(
	struct mpool_descriptor        *mp,
	struct ecio_layout_descriptor  *layout,
	int                             bgdel)
{
	merr_t                          err;
	struct pmd_mdc_info            *cinfo = NULL;
	struct ecio_layout_descriptor  *found = NULL;
	u8                              cslot;
	bool                            is_mblock;
	bool                            is_mlog;
	u64                             objid;

	pmd_obj_wrlock(mp, layout);

	objid     = layout->eld_objid;
	is_mblock = mblock_objid(objid);
	is_mlog   = mlog_objid(objid);

	if (!objtype_user(objid_type(objid)) ||
	    !(layout->eld_state & ECIO_LYT_COMMITTED) ||
	    (layout->eld_state & ECIO_LYT_REMOVED)) {
		pmd_obj_wrunlock(mp, layout);

		err = merr(EINVAL);
		mp_pr_rl("mpool %s, delete failed objid 0x%lx type (%s) state 0x%x",
			 err, mp->pds_name, (ulong)objid,
			 is_mblock ? "mblock" : "mlog",
			 layout->eld_state);
		return err;
	}

	cslot = objid_slot(objid);
	cinfo = &mp->pds_mda.mdi_slotv[cslot];

	/*
	 * Must log delete record before removing object for crash recovery.
	 * Must hold cinfo.compactlock while logging delete record and
	 * removing object from the list of committed objects to prevent
	 * race with MDC compaction
	 */
	pmd_mdc_lock(&cinfo->mmi_compactlock, cslot);

	pmd_mdc_lock(&cinfo->mmi_reflock, cslot);
	if (layout->eld_isdel || layout->eld_refcnt > 2) {
		int rc = layout->eld_isdel ? EINVAL : EBUSY;

		pmd_mdc_unlock(&cinfo->mmi_reflock);
		pmd_mdc_unlock(&cinfo->mmi_compactlock);
		pmd_obj_wrunlock(mp, layout);

		err = merr(rc);
		mp_pr_rl("mpool %s, delete failed objid %lx, state 0x%x, refcnt %ld, isdel %d, type (%s)",
			 err, mp->pds_name, (ulong)objid,
			 layout->eld_state,
			 layout->eld_refcnt, layout->eld_isdel,
			 is_mblock ? "mblock" : "mlog");

		return err;
	}

	layout->eld_refcnt = 0;
	layout->eld_isdel = true;
	layout->eld_state |= ECIO_LYT_REMOVED;
	pmd_mdc_unlock(&cinfo->mmi_reflock);

	err = pmd_log_delete(mp, objid);
	if (!ev(err)) {
		pmd_mdc_wrlock(&cinfo->mmi_colock, cslot);
		found = objid_to_layout_search_mdc(&cinfo->mmi_obj, objid);
		if (found)
			rb_erase(&found->eld_nodemdc, &cinfo->mmi_obj);
		pmd_mdc_wrunlock(&cinfo->mmi_colock);
	} else {
		/* It is legal to delete the object,
		 * but we failed to put an object delete message into the log
		 */
		pmd_mdc_lock(&cinfo->mmi_reflock, cslot);
		layout->eld_refcnt = 2;
		layout->eld_isdel = false;
		layout->eld_state &= ~ECIO_LYT_REMOVED;
		pmd_mdc_unlock(&cinfo->mmi_reflock);
	}

	pmd_mdc_unlock(&cinfo->mmi_compactlock);
	pmd_obj_wrunlock(mp, layout);

	if (found) {
		atomic_dec(&cinfo->mmi_pco_cnt.pcc_cobj);
		pmd_update_mdc_stats(mp, layout, cinfo, PMD_OBJ_DELETE);
	}

	if (ev(err)) {
		mp_pr_err("mpool %s, objid 0x%lx, pmd_log_del failed",
			  err, mp->pds_name, (ulong)objid);
		return err;
	}

	atomic_inc(&cinfo->mmi_pco_cnt.pcc_del);

	pmd_obj_erase_start(mp, layout);

	return 0;
}

merr_t
pmd_obj_delete(
	struct mpool_descriptor        *mp,
	struct ecio_layout_descriptor  *layout)
{
	return pmd_obj_delete_impl(mp, layout, 0);
}


merr_t pmd_log_erase(struct mpool_descriptor *mp, u64 objid, u64 gen)
{
	struct omf_mdcrec_data  cdr;

	cdr.omd_rtype = OMF_MDR_OERASE;
	cdr.u.obj.omd_objid = objid;
	cdr.u.obj.omd_gen = gen;
	return pmd_mdc_addrec(mp, objid_slot(objid), &cdr);
}

/**
 * pmd_mdc0_meta_update_update() - update on media the MDC0 metadata.
 * @mp:
 * @layout: Used to know on which drives to write the MDC0 metadata.
 *
 * For now write the whole super block, but only the MDC0 metadata needs
 * to be updated, the rest of the superblock doesn't change.
 *
 * In 1.0 the MDC0 metadata is replicated on the 4 superblocks of the drive.
 * In case of failure, the SBs of a same drive may end up having different
 * values for the MDC0 metadata.
 * To adress this situation voting could be used along with the SB gen number
 * psb_gen. But for 1.0 a simpler approach is taken: SB gen number is not used
 * and SB0 is the authoritative replica. The other 3 replicas of MDC0 metadata
 * are not used when the mpool activates.
 */
static merr_t
pmd_mdc0_meta_update(
	struct mpool_descriptor       *mp,
	struct ecio_layout_descriptor *layout)
{
	struct omf_sb_descriptor *sb;
	struct mc_parms		  mc_parms;
	struct mpool_dev_info    *pd;
	merr_t err;

	pd = &(mp->pds_pdv[layout->eld_ld.ol_pdh]);

	sb = kzalloc(sizeof(*sb), GFP_KERNEL);
	if (!sb)
		return merr(ENOMEM);

	/*
	 * set superblock values common to all new drives in pool
	 * (new or extant)
	 */
	sb->osb_magic = OMF_SB_MAGIC;
	strlcpy((char *) sb->osb_name, mp->pds_name, sizeof(sb->osb_name));
	sb->osb_vers = OMF_SB_DESC_VER_LAST;
	mpool_uuid_copy(&sb->osb_poolid, &mp->pds_poolid);
	sb->osb_gen = 1;

	/* set superblock values specific to this drive */
	mpool_uuid_copy(&sb->osb_parm.odp_devid, &pd->pdi_devid);
	sb->osb_parm.odp_devsz = pd->pdi_parm.dpr_devsz;
	sb->osb_parm.odp_zonetot = pd->pdi_parm.dpr_zonetot;
	mc_pd_prop2mc_parms(&pd->pdi_parm.dpr_prop, &mc_parms);
	mc_parms2omf_devparm(&mc_parms, &sb->osb_parm);

	sbutil_mdc0_copy(sb, &mp->pds_sbmdc0);

	err = 0;
	mp_pr_debug("MDC0 compaction gen1 %lu gen2 %lu",
		    err, (ulong)sb->osb_mdc01gen,
		    (ulong)sb->osb_mdc02gen);

	/*
	 * sb_write_update() succeeds if at least SB0 is written. It is
	 * not a problem to have SB1 not written because the authoritative
	 * MDC0 metadata replica is the one in SB0.
	 */
	err = sb_write_update(pd, sb);
	if (ev(err)) {
		mp_pr_err("compacting %s MDC0, writing superblock on drive %s failed",
			  err, mp->pds_name, pd->pdi_name);
	}

	kfree(sb);
	return err;
}

merr_t
pmd_obj_erase(
	struct mpool_descriptor       *mp,
	struct ecio_layout_descriptor *layout,
	u64                            gen)
{
	merr_t err;
	u64    objid = layout->eld_objid;

	if ((pmd_objid_type(objid) != OMF_OBJ_MLOG) ||
	     (!(layout->eld_state & ECIO_LYT_COMMITTED)) ||
	     (layout->eld_state & ECIO_LYT_REMOVED) ||
	     (gen <= layout->eld_gen)) {
		mp_pr_warn("mpool %s, object erase failed to start, objid 0x%lx state 0x%x gen %lu",
			   mp->pds_name,
			  (ulong)objid, layout->eld_state, (ulong)gen);

		return merr(EINVAL);
	}

	/*
	 * Must log the higher gen number for the old active mlog before
	 * updating object state (layout->eld_gen of the old active mlog).
	 * It is to guarantee that a activate after crash will know which is the
	 * new active mlog.
	 */

	if (objid_mdc0log(objid)) {
		/* compact lock is held by the caller */

		/*
		 * Change MDC0 metadata image in RAM
		 */
		if (objid == MDC0_OBJID_LOG1)
			mp->pds_sbmdc0.osb_mdc01gen = gen;
		else
			mp->pds_sbmdc0.osb_mdc02gen = gen;

		/*
		 * Write the updated MDC0 metadata in the super blocks of the
		 * drives holding MDC0 metadata.
		 * Note: for 1.0, there is only one drive.
		 */
		err = pmd_mdc0_meta_update(mp, layout);
		if (!ev(err))
			/*
			 * Update in-memory eld_gen, only if on-media
			 * gen gets successfully updated
			 */
			layout->eld_gen = gen;
	} else {
		struct pmd_mdc_info *cinfo;
		u8                   cslot;

		/*
		 * Take the MDC0 (or mlog MDCi for user MDC) compact lock to
		 * avoid a race with MDC0 (or mlog MDCi) compaction).
		 */
		cslot = objid_slot(layout->eld_objid);
		cinfo = &mp->pds_mda.mdi_slotv[cslot];

		pmd_mdc_lock(&cinfo->mmi_compactlock, cslot);

		err = pmd_log_erase(mp, layout->eld_objid, gen);
		if (!ev(err)) {
			layout->eld_gen = gen;
			if (cslot)
				atomic_inc(&cinfo->mmi_pco_cnt.pcc_er);

		}
		pmd_mdc_unlock(&cinfo->mmi_compactlock);
	}

	return err;
}

merr_t
pmd_obj_get(
	struct mpool_descriptor        *mp,
	struct ecio_layout_descriptor  *layout)
{
	struct pmd_mdc_info    *cinfo;
	int                     rc;
	u8                      cslot;

	cslot = objid_slot(layout->eld_objid);
	cinfo = &mp->pds_mda.mdi_slotv[cslot];

	pmd_mdc_lock(&cinfo->mmi_reflock, cslot);
	rc = layout->eld_isdel ? ENOSPC : 0;
	if (!rc)
		layout->eld_refcnt++;
	pmd_mdc_unlock(&cinfo->mmi_reflock);

	return rc ? merr(rc) : 0;
}

void
pmd_obj_put(
	struct mpool_descriptor       *mp,
	struct ecio_layout_descriptor *layout)
{
	struct pmd_mdc_info    *cinfo;
	bool                    put;
	u8                      cslot;
	merr_t                  err;

	assert(layout);
	if (!layout)
		return;

	cslot = objid_slot(layout->eld_objid);
	cinfo = &mp->pds_mda.mdi_slotv[cslot];

	pmd_obj_rdlock(mp, layout);

	if (!objtype_user(objid_type(layout->eld_objid)) ||
	    (layout->eld_state & ECIO_LYT_REMOVED)) {
		pmd_obj_rdunlock(mp, layout);

		err = merr(EINVAL);
		mp_pr_rl("mpool %s, invalid state, objid 0x%lx state 0x%x",
			 err, mp->pds_name,
			 (ulong)layout->eld_objid, layout->eld_state);

		return;
	}

	pmd_mdc_lock(&cinfo->mmi_reflock, cslot);
	put = layout->eld_refcnt > 1 && !layout->eld_isdel;
	if (put)
		layout->eld_refcnt--;
	pmd_mdc_unlock(&cinfo->mmi_reflock);

	pmd_obj_rdunlock(mp, layout);

	if (put)
		return;

	err = merr(EINVAL);
	mp_pr_rl("mpool %s, put failed: objid %lx refcnt %ld isdel %d",
		 err, mp->pds_name, (ulong)layout->eld_objid,
		 layout->eld_refcnt, layout->eld_isdel);
}

struct ecio_layout_descriptor *
pmd_obj_find_get(
	struct mpool_descriptor    *mp,
	u64                         objid)
{
	struct ecio_layout_descriptor  *found;
	struct pmd_mdc_info            *cinfo;
	u8                              cslot;
	merr_t                          err;

	if (!objtype_user(objid_type(objid)))
		return NULL;

	cslot = objid_slot(objid);
	cinfo = &mp->pds_mda.mdi_slotv[cslot];
	err = 0;

	pmd_mdc_rdlock(&cinfo->mmi_colock, cslot);
	found = objid_to_layout_search_mdc(&cinfo->mmi_obj, objid);
	if (found)
		err = pmd_obj_get(mp, found);
	pmd_mdc_rdunlock(&cinfo->mmi_colock);

	/* If we did not find the object in the committed list we must
	 * look in the uncommitted list.
	 */
	if (!found) {
		pmd_mdc_lock(&cinfo->mmi_uncolock, cslot);
		found = objid_to_layout_search_mdc(&cinfo->mmi_uncobj, objid);
		if (found)
			err = pmd_obj_get(mp, found);
		pmd_mdc_unlock(&cinfo->mmi_uncolock);
	}

	return err ? NULL : found;
}

/* Please see pmd.h for the various nesting levels for a locking class */

/*
 * Nesting levels for layout rwlock.
 *
 * The mpool_descriptor is passed as an argument to pmd_obj_*lock/unlock
 * routines in prep for moving to a lock pool(for layout rw lock).
 */
void
pmd_obj_rdlock(
	struct mpool_descriptor        *mp,
	struct ecio_layout_descriptor  *layout)
{
	if (objid_slot(layout->eld_objid))
		down_read_nested(layout->eld_rwlock, PMD_OBJ_CLIENT);
	else if (objid_mdc0log(layout->eld_objid))
		down_read_nested(layout->eld_rwlock, PMD_MDC_ZERO);
	else
		down_read_nested(layout->eld_rwlock, PMD_MDC_NORMAL);
}

void
pmd_obj_rdunlock(
	struct mpool_descriptor        *mp,
	struct ecio_layout_descriptor  *layout)
{
	up_read(layout->eld_rwlock);
}

void
pmd_obj_wrlock(
	struct mpool_descriptor        *mp,
	struct ecio_layout_descriptor  *layout)
{
	if (objid_slot(layout->eld_objid))
		down_write_nested(layout->eld_rwlock, PMD_OBJ_CLIENT);
	else if (objid_mdc0log(layout->eld_objid))
		down_write_nested(layout->eld_rwlock, PMD_MDC_ZERO);
	else
		down_write_nested(layout->eld_rwlock, PMD_MDC_NORMAL);
}

void
pmd_obj_wrunlock(
	struct mpool_descriptor        *mp,
	struct ecio_layout_descriptor  *layout)
{
	up_write(layout->eld_rwlock);
}

merr_t
pmd_mdc_alloc(
	struct mpool_descriptor    *mp,
	u64                         mincap,
	u32                         iter)
{
	merr_t err;
	u64    mdcslot = 0;
	u64    logid1 = 0;
	u64    logid2 = 0;
	u32    pdcnt;
	bool   reverse = false;

	struct ecio_err_report	 erpt;
	enum mp_media_classp	 mclassp;
	struct pmd_obj_capacity  ocap;
	struct pmd_mdc_info	*cinfo = NULL;
	struct pmd_mdc_info	*cinew = NULL;
	struct ecio_layout_descriptor *layout1 = NULL;
	struct ecio_layout_descriptor *layout2 = NULL;
	const char              *msg = "(no detail)";

	/*
	 * serialize to prevent gap in mdc slot space in event of failure
	 */
	mutex_lock(&pmd_s_lock);

	/*
	 * recover previously failed mdc alloc if needed; cannot continue
	 * if fails
	 * note: there is an unlikely corner case where we logically delete an
	 * mlog from a previously failed mdc alloc but a background op is
	 * preventing its full removal; this will show up later in this
	 * fn as a failed alloc.
	 */
	err = pmd_mdc0_validate(mp, 0);
	if (err) {
		mutex_unlock(&pmd_s_lock);

		mp_pr_err("mpool %s, allocating an MDC, inconsistent MDC0",
			  err, mp->pds_name);
		return err;
	}

	/* mdc0 exists by definition; created as part of mpool creation */
	cinfo = &mp->pds_mda.mdi_slotv[0];

	pmd_mdc_lock(&cinfo->mmi_uqlock, 0);
	mdcslot = cinfo->mmi_luniq;
	pmd_mdc_unlock(&cinfo->mmi_uqlock);

	if (mdcslot >= MDC_SLOTS - 1) {
		mutex_unlock(&pmd_s_lock);

		err = merr(ENOSPC);
		mp_pr_err("mpool %s, allocating an MDC, too many %lu",
			  err, mp->pds_name, (ulong)mdcslot);
		return err;
	}
	mdcslot = mdcslot + 1;

	/*
	 * Alloc rec buf for new mdc slot; not visible so don't need to
	 * lock fields.
	 */
	cinew = &mp->pds_mda.mdi_slotv[mdcslot];
	cinew->mmi_recbuf = kzalloc(OMF_MDCREC_PACKLEN_MAX, GFP_KERNEL);
	if (!cinew->mmi_recbuf) {
		mutex_unlock(&pmd_s_lock);

		mp_pr_warn("mpool %s, MDC%lu pack/unpack buf alloc failed %lu",
			   mp->pds_name, (ulong)mdcslot,
			   (ulong)OMF_MDCREC_PACKLEN_MAX);
		return merr(ENOMEM);
	}
	cinew->mmi_credit.ci_slot = mdcslot;

	mclassp = MP_MED_CAPACITY;
	pdcnt = 1;

	/*
	 * Create new mdcs with same parameters and on same media class
	 * as mdc0.
	 */
	ocap.moc_captgt = mincap;
	ocap.moc_spare  = false;

	logid1 = logid_make(2 * mdcslot, 0);
	logid2 = logid_make(2 * mdcslot + 1, 0);

	if (!(pdcnt & 0x1) && ((iter * 2 / pdcnt) & 0x1)) {
		/*
		 * Reverse the allocation order.
		 * The goal is to have active mlogs on all the mpool PDs.
		 * If 2 PDs, no parity, no reserve, the active mlogs
		 * will be on PDs 0,1,0,1,0,1,0,1 etc
		 * instead of 0,0,0,0,0 etc without reversing.
		 * No need to reverse if the number of PDs is odd.
		 */
		reverse = true;
	}

	/*
	 * Each mlog must meet mincap since only one is active at a
	 * time.
	 */
	err = pmd_obj_alloc_cmn(mp, reverse ? logid2 : logid1, OMF_OBJ_MLOG,
				&ocap, mclassp, 0, &layout1);
	if (ev(err)) {
		if (merr_errno(err) != ENOENT)
			msg = "allocation of first mlog failed";
		goto exit;
	}

	err = pmd_obj_alloc_cmn(mp, reverse ? logid1 : logid2,
				OMF_OBJ_MLOG, &ocap, mclassp, 0, &layout2);
	if (ev(err)) {
		pmd_obj_abort(mp, layout1);
		if (merr_errno(err) != ENOENT)
			msg = "allocation of second mlog failed";
		goto exit;
	}

	/*
	 * Must erase before commit to guarantee new mdc logs start
	 * empty; mlogs not committed so pmd_obj_erase()
	 * not needed to make atomic.
	 */
	pmd_obj_wrlock(mp, layout1);
	pmd_obj_wrlock(mp, layout2);
	err = ecio_mlog_erase(mp, layout1, 0, &erpt);
	if (err) {
		msg = "erase of first mlog failed";
	} else {
		err = ecio_mlog_erase(mp, layout2, 0, &erpt);
		if (err)
			msg = "erase of second mlog failed";
	}
	pmd_obj_wrunlock(mp, layout2);
	pmd_obj_wrunlock(mp, layout1);
	if (ev(err)) {
		pmd_obj_abort(mp, layout1);
		pmd_obj_abort(mp, layout2);
		goto exit;
	}

	/*
	 * don't need to commit logid1 and logid2 atomically; mdc0
	 * validation deletes non-paired mdc logs to handle failing part
	 * way through this process
	 */
	err = pmd_obj_commit(mp, layout1);
	if (ev(err)) {
		pmd_obj_abort(mp, layout1);
		pmd_obj_abort(mp, layout2);
		msg = "commit of first mlog failed";
		goto exit;
	} else {
		err = pmd_obj_commit(mp, layout2);
		if (ev(err)) {
			pmd_obj_delete(mp, layout1);
			pmd_obj_abort(mp, layout2);
			msg = "commit of second mlog failed";
			goto exit;
		}
	}

	/*
	 * Finalize new mdc slot before making visible; don't need to
	 * lock fields.
	 */
	err = mp_mdc_open(mp, logid1, logid2, MDC_OF_SKIP_SER, &cinew->mmi_mdc);
	if (ev(err)) {
		msg = "mdc open failed";

		/* failed open so just delete logid1/2; don't
		 * need to delete atomically since mdc0 validation
		 * will cleanup any detritus
		 */
		pmd_obj_delete(mp, layout1);
		pmd_obj_delete(mp, layout2);
		goto exit;
	}

	/*
	 * Append the version record.
	 */
	if (upg_ver_cmp2(upg_mdccver_latest(), ">=", 1, 0, 0, 1)) {
		err = pmd_mdc_addrec_version(mp, mdcslot);
		if (ev(err)) {
			msg = "error adding the version record";
			/*
			 * No version record in a MDC will trigger a MDC
			 * compaction if a activate is attempted later with this
			 * empty MDC.
			 * The compaction will add the version record in that
			 * empty MDC.
			 * Same error handling as above.
			 */
			pmd_obj_delete(mp, layout1);
			pmd_obj_delete(mp, layout2);
			goto exit;
		}
	}

	/* make new mdc visible */
	pmd_mdc_lock(&cinfo->mmi_uqlock, 0);

	spin_lock(&mp->pds_mda.mdi_slotvlock);
	cinfo->mmi_luniq = mdcslot;
	mp->pds_mda.mdi_slotvcnt = mdcslot + 1;
	mp->pds_mda.mdi_slotvcnt_shift = 0;
	if (mdcslot > 1)
		mp->pds_mda.mdi_slotvcnt_shift =
			__builtin_ctz(roundup_pow_of_two(mdcslot));
	spin_unlock(&mp->pds_mda.mdi_slotvlock);

	pmd_mdc_unlock(&cinfo->mmi_uqlock);

exit:
	if (err) {
		kfree(cinew->mmi_recbuf);
		cinew->mmi_recbuf = NULL;
	}

	mutex_unlock(&pmd_s_lock);

	mp_pr_debug("new mdc logid1 %llu logid2 %llu",
		    0, (unsigned long long)logid1,
		    (unsigned long long)logid2);

	if (err) {
		mp_pr_err("mpool %s, MDC%lu: %s",
			  err, mp->pds_name, (ulong)mdcslot, msg);

	} else {
		mp_pr_debug("mpool %s, delta slotvcnt from %u to %llu",
			    0, mp->pds_name, mp->pds_mda.mdi_slotvcnt,
			    (unsigned long long)mdcslot + 1);

	}
	return err;
}

void
pmd_mdc_cap(
	struct mpool_descriptor    *mp,
	u64                        *mdcmax,
	u64                        *mdccap,
	u64                        *mdc0cap)
{
	struct pmd_mdc_info            *cinfo = NULL;
	struct ecio_layout_descriptor  *layout = NULL;
	struct rb_node                 *node = NULL;
	u64                             mlogsz;
	u32                             zonepg = 0;
	u16                             mdcn = 0;

	if (!mdcmax || !mdccap || !mdc0cap)
		return;

	/* serialize to prevent race with pmd_mdc_alloc() */
	mutex_lock(&pmd_s_lock);

	/*
	 * exclude mdc0 from stats because not used for mpool user
	 * object metadata
	 */
	cinfo = &mp->pds_mda.mdi_slotv[0];

	pmd_mdc_lock(&cinfo->mmi_uqlock, 0);
	*mdcmax = cinfo->mmi_luniq;
	pmd_mdc_unlock(&cinfo->mmi_uqlock);

	/*  taking compactlock to freeze all object layout metadata in mdc0 */
	pmd_mdc_lock(&cinfo->mmi_compactlock, 0);
	pmd_mdc_rdlock(&cinfo->mmi_colock, 0);

	for (node = rb_first(&cinfo->mmi_obj); node; node = rb_next(node)) {
		layout = rb_entry(node, struct ecio_layout_descriptor,
				  eld_nodemdc);
		mdcn = objid_uniq(layout->eld_objid) >> 1;

		if (mdcn > *mdcmax)
			/* Ignore detritus from failed pmd_mdc_alloc() */
			continue;

		zonepg = ecio_zonepg(mp, layout);
		mlogsz = (layout->eld_ld.ol_zcnt * zonepg) << PAGE_SHIFT;

		if (!mdcn)
			*mdc0cap = *mdc0cap + mlogsz;
		else
			*mdccap = *mdccap + mlogsz;
	}

	pmd_mdc_rdunlock(&cinfo->mmi_colock);
	pmd_mdc_unlock(&cinfo->mmi_compactlock);
	mutex_unlock(&pmd_s_lock);

	/* only count capacity of one mlog in each mdc mlog pair */
	*mdccap  = *mdccap >> 1;
	*mdc0cap = *mdc0cap >> 1;
}

merr_t
pmd_prop_mcconfig(
	struct mpool_descriptor    *mp,
	struct mpool_dev_info      *pd,
	bool			    compacting)
{
	merr_t                  err;
	struct omf_mdcrec_data  cdr;
	struct mc_parms		mc_parms;

	cdr.omd_rtype = OMF_MDR_MCCONFIG;
	cdr.u.dev.omd_state = pd->pdi_state;
	mpool_uuid_copy(&cdr.u.dev.omd_parm.odp_devid, &pd->pdi_devid);
	mc_pd_prop2mc_parms(&pd->pdi_parm.dpr_prop, &mc_parms);
	mc_parms2omf_devparm(&mc_parms, &cdr.u.dev.omd_parm);
	cdr.u.dev.omd_parm.odp_zonetot = pd->pdi_parm.dpr_zonetot;
	cdr.u.dev.omd_parm.odp_devsz = pd->pdi_parm.dpr_devsz;

	if (compacting)
		/* No sync needed and don't trigger another compaction. */
		err = pmd_mdc_append(mp, 0, &cdr, 0);
	else
		err = pmd_mdc_addrec(mp, 0, &cdr);

	return ev(err);
}

merr_t
pmd_prop_mcspare(
	struct mpool_descriptor    *mp,
	enum mp_media_classp        mclassp,
	u8                          spzone,
	bool			    compacting)
{
	merr_t                  err = 0;
	struct omf_mdcrec_data  cdr;

	if (!mclassp_valid(mclassp) || spzone > 100) {
		err = merr(EINVAL);
		mp_pr_err("persisting %s spare zone info, invalid arguments %d %u",
			  err, mp->pds_name, mclassp, spzone);
		return err;
	}

	cdr.omd_rtype = OMF_MDR_MCSPARE;
	cdr.u.mcs.omd_mclassp = mclassp;
	cdr.u.mcs.omd_spzone = spzone;

	if (compacting) {
		/* No sync needed and don't trigger another compaction. */
		err = pmd_mdc_append(mp, 0, &cdr, 0);
		ev(err);
	} else {
		err = pmd_mdc_addrec(mp, 0, &cdr);
		ev(err);
	}

	return err;
}

merr_t
pmd_prop_mpconfig(
	struct mpool_descriptor    *mp,
	const struct mpool_config  *cfg,
	bool                        compacting)
{
	struct omf_mdcrec_data  cdr = { };
	merr_t                  err;

	cdr.omd_rtype = OMF_MDR_MPCONFIG;
	cdr.u.omd_cfg = *cfg;

	if (compacting)
		err = pmd_mdc_append(mp, 0, &cdr, 0);
	else
		err = pmd_mdc_addrec(mp, 0, &cdr);

	return ev(err);
}

void
pmd_layout_free(
	struct mpool_descriptor        *mp,
	struct ecio_layout_descriptor  *layout)
{
	merr_t  err;
	u16     pdh;

	pdh = layout->eld_ld.ol_pdh;

	err = smap_free(mp, pdh, layout->eld_ld.ol_zaddr,
			layout->eld_ld.ol_zcnt);
	if (err) {
		/* smap_free() should never fail */
		mp_pr_err("releasing %s drive %s space for layout failed, objid 0x%lx",
			  err, mp->pds_name, mp->pds_pdv[pdh].pdi_name,
			  (ulong)layout->eld_objid);
	}

	ecio_layout_free(layout);
}

/**
 * pmd_layout_calculate() -
 * @mp:
 * @ocap:
 * @mc:
 * @zcnt:
 * @otype:
 */
static void
pmd_layout_calculate(
	struct mpool_descriptor   *mp,
	struct pmd_obj_capacity   *ocap,
	struct media_class        *mc,
	u64                       *zcnt,
	enum obj_type_omf          otype)
{
	u32    zonepg;

	zonepg = mp->pds_pdv[mc->mc_pdmc].pdi_parm.dpr_zonepg;

	if (!ocap->moc_captgt) {
		/* Obj capacity not specified; use one zone. */
		*zcnt = 1;
		return;
	}

	*zcnt = 1 + ((ocap->moc_captgt - 1) / (zonepg << PAGE_SHIFT));
}

/**
 * pmd_layout_alloc() -
 * @mp:
 * @ocap:
 * @otype:
 * @layoutp:
 * @mc:		media class
 * @zcnt:
 */
static merr_t
pmd_layout_alloc(
	struct mpool_descriptor         *mp,
	struct pmd_obj_capacity         *ocap,
	struct ecio_layout_descriptor  **layoutp,
	struct media_class              *mc,
	u64                              zcnt)
{
	struct ecio_layout_descriptor  *layout = *layoutp;
	enum smap_space_type            spctype;
	struct mc_smap_parms            mcsp;

	u64     zoneaddr;
	u64     align;
	u8      pdh;
	merr_t  err;

	spctype = SMAP_SPC_USABLE_ONLY;
	if (ocap->moc_spare)
		spctype = SMAP_SPC_SPARE_2_USABLE;

	/* To reduce/eliminate fragmenation, make sure the alignment is
	 * a power of 2.
	 */
	err = mc_smap_parms_get(mp, mc->mc_parms.mcp_classp, &mcsp);
	if (ev(err))
		return err;

	align = min_t(u64, zcnt, mcsp.mcsp_align);
	align = roundup_pow_of_two(align);

	pdh = mc->mc_pdmc;
	err = smap_alloc(mp, pdh, zcnt, spctype, &zoneaddr, align);
	if (ev(err))
		return err;

	layout->eld_ld.ol_pdh = pdh;
	layout->eld_ld.ol_zaddr = zoneaddr;

	return 0;
}

static merr_t pmd_log_idckpt(struct mpool_descriptor *mp, u64 objid)
{
	struct omf_mdcrec_data  cdr;

	cdr.omd_rtype = OMF_MDR_OIDCKPT;
	cdr.u.obj.omd_objid = objid;
	return pmd_mdc_addrec(mp, objid_slot(objid), &cdr);
}

/**
 * pmd_alloc_idgen() - generate an id for an allocated object.
 * @mp:
 * @otype:
 * @objid: outpout
 *
 * Does a round robin on the MDC1/255 avoiding the ones that are candidate
 * for pre compaction.
 *
 * The round robin has a bias toward the MDCs with the smaller number of
 * objects. This is to recover from rare and very big allocation bursts.
 * During an allocation, the MDC[s] candidate for pre compaction are avoided.
 * If the allocation is a big burst, the result is that these MDC[s] have much
 * less objects in them as compared to the other ones.
 * After the burst if a relatively constant allocation rate takes place, the
 * deficit in objects of the MDCs avoided during the burst, is never recovered.
 * The bias in the round robin allows to recover. After a while all MDCs ends
 * up again with about the same number of objects.
 */
static merr_t
pmd_alloc_idgen(
	struct mpool_descriptor    *mp,
	enum obj_type_omf           otype,
	u64                        *objid)
{
	struct pmd_mdc_info    *cinfo = NULL;

	merr_t  err = 0;
	u8      cslot;
	u32     tidx;

	if (mp->pds_mda.mdi_slotvcnt < 2) {
		/* no mdc available to assign object to; cannot use mdc0 */
		err = merr(ENOSPC);
		mp_pr_err("mpool %s, no MDCi with i>0", err, mp->pds_name);
		*objid = 0;
		return err;
	}

	/* get next mdc for allocation */
	tidx = atomic_inc_return(&mp->pds_mda.mdi_sel.mds_tbl_idx) % MDC_TBL_SZ;
	assert(tidx <= MDC_TBL_SZ);

	cslot = mp->pds_mda.mdi_sel.mds_tbl[tidx];
	cinfo = &mp->pds_mda.mdi_slotv[cslot];

	pmd_mdc_lock(&cinfo->mmi_uqlock, cslot);
	*objid = objid_make(cinfo->mmi_luniq + 1, otype, cslot);
	if (objid_ckpt(*objid)) {

		/* Must checkpoint objid before assigning it to an object
		 * to guarantee it will not reissue objid after a crash.
		 * Must hold cinfo.compactlock while log checkpoint to mdc
		 * to prevent a race with mdc compaction.
		 */
		pmd_mdc_lock(&cinfo->mmi_compactlock, cslot);
		err = pmd_log_idckpt(mp, *objid);
		if (!err)
			cinfo->mmi_lckpt = *objid;
		pmd_mdc_unlock(&cinfo->mmi_compactlock);
	}

	if (!err)
		cinfo->mmi_luniq = cinfo->mmi_luniq + 1;
	pmd_mdc_unlock(&cinfo->mmi_uqlock);

	if (ev(err)) {
		mp_pr_rl("mpool %s, checkpoint append for objid 0x%lx failed",
			 err, mp->pds_name, (ulong)*objid);
		*objid = 0;
		return err;
	}

	return 0;
}

static merr_t
pmd_realloc_idvalidate(struct mpool_descriptor *mp, u64 objid)
{
	merr_t                  err = 0;
	u8                      cslot = objid_slot(objid);
	u64                     uniq = objid_uniq(objid);
	struct pmd_mdc_info    *cinfo = NULL;

	/* we never realloc objects in mdc0 */
	if (!cslot) {
		err = merr(EINVAL);
		mp_pr_err("mpool %s, can't re-allocate an object 0x%lx associated to MDC0",
			  err, mp->pds_name, (ulong)objid);
		return err;
	}

	spin_lock(&mp->pds_mda.mdi_slotvlock);
	if (cslot >= mp->pds_mda.mdi_slotvcnt)
		err = merr(EINVAL);
	spin_unlock(&mp->pds_mda.mdi_slotvlock);

	if (err) {
		mp_pr_err("mpool %s, can't re-allocate an object, slot number %u is too big %u 0x%lx",
			  err, mp->pds_name,
			  cslot, mp->pds_mda.mdi_slotvcnt, (ulong)objid);
	} else {
		cinfo = &mp->pds_mda.mdi_slotv[cslot];
		pmd_mdc_lock(&cinfo->mmi_uqlock, cslot);
		if (uniq > cinfo->mmi_luniq)
			err = merr(EINVAL);
		pmd_mdc_unlock(&cinfo->mmi_uqlock);

		if (err) {
			mp_pr_err("mpool %s, can't re-allocate an object, its unique id %lu is too big %lu 0x%lx",
				  err, mp->pds_name, (ulong)uniq,
				  (ulong)cinfo->mmi_luniq, (ulong)objid);
		}
	}
	return err;
}

/**
 * pmd_alloc_argcheck() -
 * @mp:      Mpool descriptor
 * @objid:   Object ID
 * @otype:   Object type
 * @ocap:    Object capacity info
 * @mclassp: Media class
 */
static merr_t
pmd_alloc_argcheck(
	struct mpool_descriptor    *mp,
	u64                         objid,
	enum obj_type_omf           otype,
	struct pmd_obj_capacity    *ocap,
	enum mp_media_classp        mclassp)
{
	merr_t err;

	if (!mp)
		return merr(EINVAL);

	if (!objtype_user(otype) || !mpool_mc_isvalid(mclassp)) {
		err = merr(EINVAL);
		mp_pr_err("mpool %s, unknown object type or media class %d %d",
			  err, mp->pds_name, otype, mclassp);
		return err;
	}

	if (objid && objid_type(objid) != otype) {
		err = merr(EINVAL);
		mp_pr_err("mpool %s, unknown object type mismatch %d %d",
			  err, mp->pds_name, objid_type(objid), otype);
		return err;
	}

	return 0;
}

merr_t
pmd_obj_alloc_cmn(
	struct mpool_descriptor        *mp,
	u64                             objid,
	enum obj_type_omf               otype,
	struct pmd_obj_capacity        *ocap,
	enum mp_media_classp            mclassp,
	int                             realloc,
	struct ecio_layout_descriptor **layout)
{
	u64                     zcnt = 0;
	struct pmd_mdc_info    *cinfo = NULL;
	merr_t                  err;
	int                     retries, flush;
	bool                    beffort, fallback;
	u8                      cslot;
	struct media_class     *mc;
	struct mpool_uuid       uuid;

	*layout = NULL;

	err = pmd_alloc_argcheck(mp, objid, otype, ocap, mclassp);
	if (ev(err))
		return err;

	if (!objid) {
		/*
		 * alloc: generate objid, checkpoint as needed to
		 * support realloc of uncommitted objects after crash and to
		 * guarantee objids never reuse
		 */
		err = pmd_alloc_idgen(mp, otype, &objid);
	} else if (realloc) {
		/* realloc: validate objid */
		err = pmd_realloc_idvalidate(mp, objid);
	}

	if (err)
		return ev(err);

	beffort = mpool_mc_isbe(mclassp);
	mclassp = mpool_mc_first_get(mclassp);

	/*
	 * Retry from 1 to 2ms if fallback is requested, and if no fallback,
	 * retry from 128 to 256ms with a flush every 1/8th of the retries.
	 * This is a workaround for the async mblock trim problem.
	 */
	fallback = (beffort && (mclassp < MP_MED_NUMBER - 1));
	retries  = fallback ? 8 : 1024;
	flush    = fallback ? 0 : retries >> 3;

retry:
	down_read(&mp->pds_pdvlock);

	do {
		mc = &mp->pds_mc[mclassp];
		if (mc->mc_pdmc >= 0)
			break;

		if (ev(!beffort || ++mclassp >= MP_MED_NUMBER)) {
			up_read(&mp->pds_pdvlock);

			return merr(ENOENT);
		}
	} while (beffort);

	assert(mclassp < MP_MED_NUMBER);

	if (otype == OMF_OBJ_MLOG)
		mpool_generate_uuid(&uuid);

	while (true) {
		/*
		 * Calculate the height (zcnt) of layout.
		 */
		pmd_layout_calculate(mp, ocap, mc, &zcnt, otype);

		*layout = ecio_layout_alloc(mp, &uuid, objid, 0, 0, zcnt);
		if (!*layout) {
			up_read(&mp->pds_pdvlock);
			*layout = NULL;
			return merr(ENOMEM);
		}

		/* Try to allocate zones from drives in media class */
		err = pmd_layout_alloc(mp, ocap, layout, mc, zcnt);
		if (!err)
			break;

		up_read(&mp->pds_pdvlock);

		ecio_layout_free(*layout);
		*layout = NULL;

		/* TODO: Retry only if mperasewq is busy... */
		if (retries-- > 0) {
			usleep_range(128, 256);

			if (flush && (retries % flush == 0))
				flush_workqueue(mp->pds_erase_wq);

			goto retry;
		}

		if (beffort && ++mclassp < MP_MED_NUMBER) {
			if (mclassp == MP_MED_NUMBER - 1) {
				retries = 1024;
				flush = retries >> 3;
			}
			goto retry;
		}

		mp_pr_rl("mpool %s, layout alloc failed: objid 0x%lx %lu %u",
			 err, mp->pds_name, (ulong)objid, (ulong)zcnt, otype);

		return err;
	}

	assert(!err);

	up_read(&mp->pds_pdvlock);

	cslot = objid_slot(objid);
	cinfo = &mp->pds_mda.mdi_slotv[cslot];

	/*
	 * We will try to update the per-mdc stats. if it fails,
	 * we don't need to insert the object to the uncommitted tree.
	 * However, if later we fail to add the object to the uncommitted tree,
	 * we need to call pmd_update_mdc_stats() again with opcode
	 * PMD_OBJ_ABORT to undo this step.
	 */
	err = pmd_update_mdc_stats(mp, *layout, cinfo, PMD_OBJ_ALLOC);
	if (err) {
		mp_pr_err("mpool %s, object 0x%lx failed to allocate per-mdc stats",
			  err, mp->pds_name, (ulong)objid);
		pmd_layout_free(mp, *layout);
		*layout = NULL;
		return err;
	}

	/* If realloc, we MUST confirm (while holding the uncommited obj
	 * tree lock) that objid is not in the committed obj tree in order
	 * to protect against an invalid *_realloc() call.
	 */
	pmd_mdc_lock(&cinfo->mmi_uncolock, cslot);
	if (realloc) {
		pmd_mdc_rdlock(&cinfo->mmi_colock, cslot);
		if (objid_to_layout_search_mdc(&cinfo->mmi_obj, objid))
			err = merr(EEXIST);
		pmd_mdc_rdunlock(&cinfo->mmi_colock);
	}

	/* For both alloc and realloc, confirm that objid is not in the
	 * uncommitted obj tree and insert it.  Note that a reallocated
	 * objid can collide, but a generated objid should never collide.
	 */
	if (!err) {
		struct ecio_layout_descriptor *dup;

		dup = objid_to_layout_insert_mdc(&cinfo->mmi_uncobj, *layout);
		if (dup)
			err = merr(EEXIST);
	}
	pmd_mdc_unlock(&cinfo->mmi_uncolock);

	if (err) {
		mp_pr_err("mpool %s, %sallocated object 0x%lx should not be in the %scommitted tree",
			  err, mp->pds_name, realloc ? "re-" : "",
			  (ulong)objid, realloc ? "" : "un");

		/*
		 * Since object insertion failed, we need to undo the
		 * per-mdc stats update we did earlier in this routine
		 */
		pmd_update_mdc_stats(mp, *layout, cinfo, PMD_OBJ_ABORT);
		pmd_layout_free(mp, *layout);
		*layout = NULL;
	}

	return err;
}

/**
 * pmd_mdc_needed() - determines if new MDCns should be created
 * @mp:  mpool descriptor
 *
 * New MDC's are created if total free space across all MDC's
 * is above a threshold value and the garbage to reclaim space
 * is below a garbage threshold.
 *
 * Locking: no lock needs to be held when calling this function.
 *
 */
bool pmd_mdc_needed(struct mpool_descriptor *mp)
{
	struct pmd_mdc_info        *cinfo;
	struct pre_compact_ctrs    *pco_cnt;
	u64    cap;
	u64    tcap;
	u64    used;
	u64    garbage;
	u64    record;
	u64    rec;
	u64    cobj;
	u32    pct;
	u32    pctg;
	u32    mdccnt;
	u16    cslot;


	/*
	 * - There is room for new MDCs to be created
	 * - Skip non-active MDC
	 * - Accumulate total capacity, total garbage and total in-use
	 *   capacity across all active MDCs.
	 * - Return true if total used capacity across all MDCs is
	 *    > threshold and garbage is < a threshold that would yield
	 *    significant free space upon compaction.
	 */

	cap = used = garbage = record = pctg = 0;

	assert(mp->pds_mda.mdi_slotvcnt <= MDC_SLOTS);
	if (mp->pds_mda.mdi_slotvcnt == MDC_SLOTS) {
		merr_t err = merr(ENOSPC);

		mp_pr_debug("mpool %s is at MAX %u MDCs",
			    err, mp->pds_name, mp->pds_mda.mdi_slotvcnt);
		return false;
	}


	for (cslot = 1, mdccnt = 0; cslot < mp->pds_mda.mdi_slotvcnt; cslot++) {

		cinfo = &mp->pds_mda.mdi_slotv[cslot];
		pco_cnt = &(cinfo->mmi_pco_cnt);

		tcap  = atomic64_read(&pco_cnt->pcc_cap);

		if (tcap == 0) {
			/* MDC closed for now and will not be considered
			 * in making a decision to create new MDC.
			 */
			mp_pr_warn("MDC %u not open", cslot);
			continue;
		}
		cap += tcap;

		mdccnt++;

		used += atomic64_read(&pco_cnt->pcc_len);
		rec  = atomic_read(&pco_cnt->pcc_cr) +
			atomic_read(&pco_cnt->pcc_up) +
			atomic_read(&pco_cnt->pcc_del) +
			atomic_read(&pco_cnt->pcc_er);

		cobj = atomic_read(&pco_cnt->pcc_cobj);

		if (rec > cobj)
			garbage += (rec - cobj);

		record  += rec;
	}

	if (mdccnt == 0) {
		mp_pr_warn("No mpool MDCs available");
		return false;
	}

	/* percentage capacity used across all MDCs */
	pct  = (used  * 100)/cap;

	/* percentage garbage available across all MDCs */
	if (garbage)
		pctg = (garbage * 100)/record;

	if (pct > mp->pds_params.mp_crtmdcpctfull &&
	    pctg < mp->pds_params.mp_crtmdcpctgrbg) {
		merr_t err = 0;

		mp_pr_debug("NEED MDCn %u Total cap %u used %u, REC %u grbg %u, PCT used %u grbg %u, Thres %u-%u",
			    err, mdccnt, (u32)cap, (u32)used,
			    (u32)record, (u32)garbage, pct, pctg,
			    (u32)mp->pds_params.mp_crtmdcpctfull,
			    (u32)mp->pds_params.mp_crtmdcpctgrbg);
		return true;
	}

	return false;
}

/**
 * pmd_compare_free_space - compare free space between MDCs
 * @f:  First  MDC
 * @s:  Second MDC
 *
 * Arrange MDCs in descending order of free space
 */
static int pmd_compare_free_space(const void *first, const void *second)
{
	const struct pmd_mdc_info *f = *(const struct pmd_mdc_info **)first;
	const struct pmd_mdc_info *s = *(const struct pmd_mdc_info **)second;

	/* return < 0 - first member should be ahead for second */
	if (f->mmi_credit.ci_free > s->mmi_credit.ci_free)
		return -1;

	/* return > 0 - first member should be after second */
	if (f->mmi_credit.ci_free < s->mmi_credit.ci_free)
		return 1;

	/* return  0 - do not swap */
	return 1;

}

/**
 * pmd_update_ms_tbl() - udpates mds_tlb with MDC slot numbers
 * @mp:  mpool descriptor
 * @slotnum:  array of slot numbers
 *
 * This function creates an array of mdc slot and credit sets by interleaving
 * MDC slots. Interleave maximize the interval at which the slots appear in
 * the mds_tbl.
 *
 * The first set in the array is reference set with only 1 member and has max
 * assigned credits. Subsequent sets are formed to match the reference set and
 * may contain one or more member such that total credit of the set will match
 * the reference set. The last set may have fewer credit than the reference set
 *
 * Locking: no lock need to be held when calling this function.
 *
 */
static void
pmd_update_mds_tbl(struct mpool_descriptor *mp, u8 num_mdc, u8 *slotnum)
{
	struct mdc_credit_set  *cset;
	struct mdc_credit_set  *cs;
	struct pmd_mdc_info    *cinfo;
	u8     csidx;
	u8     csmidx;
	u8     num_cset;
	u8     i;
	u16    refcredit;
	u16    neededcredit;
	u16    tidx;
	u16    totalcredit = 0;


	cset = kcalloc(num_mdc, sizeof(*cset), GFP_KERNEL);
	if (!cset)
		return;

	cinfo = &mp->pds_mda.mdi_slotv[slotnum[0]];
	refcredit = cinfo->mmi_credit.ci_credit;

	csidx = 0; /* creditset index */
	i     = 0; /* slotnum index   */
	while (i < num_mdc) {

		cs = &cset[csidx++];
		neededcredit = refcredit;

		csmidx = 0;
		/* setup members of the credit set */
		while (csmidx < MPOOL_MDC_SET_SZ  && i < num_mdc) {

			/* slot 0 should never be there */
			assert(slotnum[i] != 0);
			cinfo = &mp->pds_mda.mdi_slotv[slotnum[i]];
			cs->cs_num_csm    = csmidx+1;
			cs->csm[csmidx].m_slot = slotnum[i];


			if (neededcredit <= cinfo->mmi_credit.ci_credit) {
				/* more than required credit is available,
				 * leftover will be assigned to the next set.
				 */
				cs->csm[csmidx].m_credit    += neededcredit;
				cinfo->mmi_credit.ci_credit -= neededcredit;
				totalcredit += neededcredit; /* Debug */
				neededcredit = 0;
				if (cinfo->mmi_credit.ci_credit == 0)
					i++;

				/* some credit available stay at this mdc */

				break;

			} else {

				/* available credit is < needed, assign all
				 * the available credit and move to the next
				 * mdc slot.
				 */
				cs->csm[csmidx].m_credit +=
						cinfo->mmi_credit.ci_credit;
				neededcredit -=
						 cinfo->mmi_credit.ci_credit;
				totalcredit  += cinfo->mmi_credit.ci_credit;
				cinfo->mmi_credit.ci_credit = 0;

				/* move to the next mdcslot and set member */
				i++;
				csmidx++;
			}
		}
	}
	assert(totalcredit == MDC_TBL_SZ);
	num_cset = csidx;

	tidx  = 0;
	csidx = 0;
	while (tidx < MDC_TBL_SZ) {
		cs = &cset[csidx];

		if (cs->cs_idx < cs->cs_num_csm) {
			csmidx = cs->cs_idx;
			if (cs->csm[csmidx].m_credit) {
				cs->csm[csmidx].m_credit--;
				mp->pds_mda.mdi_sel.mds_tbl[tidx] =
							cs->csm[csmidx].m_slot;
				totalcredit--;

				if (cs->csm[csmidx].m_credit == 0)
					cs->cs_idx += 1;

				tidx++;
			}
		}
		/* loop over the sets */
		csidx = (csidx + 1) % num_cset;
	}
	assert(totalcredit == 0);

	kfree(cset);
}

/**
 * pmd_update_credit() - udpates MDC credit if new MDCs should be created
 * @mp:  mpool descriptor
 *
 * Credits are assigned as a ratio between MDC such that MDC with least free
 * space will fill up at the same time as other MDC.
 *
 * Locking: no lock need to be held when calling this function.
 *
 */
void pmd_update_credit(struct mpool_descriptor *mp)
{
	struct pmd_mdc_info        *cinfo;
	struct pre_compact_ctrs    *pco_cnt;

	u8       sidx;
	u8       nidx;
	u8       num_mdc;
	u16      credit;
	u16      cslot;
	u64      cap;
	u64      used;
	u64      free;
	u64      nmtoc;
	u8       slotnum[16] = { 0 };
	void   **sarray = mp->pds_mda.mdi_sel.mds_smdc;
	u32      nbnoalloc = (u32)mp->pds_params.mp_pconbnoalloc;

	/* assert(!spin_is_locked(&mp->pds_mda.mdi_slotvlock)); */

	if (mp->pds_mda.mdi_slotvcnt < 2) {
		mp_pr_warn("ENTER MDCn cnt %u NO MDCn",
			   mp->pds_mda.mdi_slotvcnt - 1);
		return;
	}

	nmtoc = atomic_read(&mp->pds_pco.pco_nmtoc);
	nmtoc = nmtoc % (mp->pds_mda.mdi_slotvcnt - 1) + 1;

	/* slotvcnt includes MDC 0 and MDCn that are in precompaction
	 * list and should be excluded. If there are less than (nbnoalloc
	 * +2) MDCs exclusion is not possible. 2 is added to account for
	 * MDC0 and the MDC pointed to by pco_nmtoc.
	 *
	 * MDC that is in pre-compacting state and two MDCs that follows
	 * are excluded from allocation. This is done to prevent stall/
	 * delays for a sync that follows an allocation as both take
	 * take a compaction lock.
	 */
	if (mp->pds_mda.mdi_slotvcnt < (nbnoalloc + 2)) {
		merr_t err = 0;

		num_mdc = mp->pds_mda.mdi_slotvcnt - 1;
		cslot  = 1;
		mp_pr_debug("ENTER MDCn cnt %u, CANNOT skip %u num_mdc %u",
			    err, mp->pds_mda.mdi_slotvcnt - 1,
			    (u32)nmtoc, num_mdc);
	} else {
		num_mdc = mp->pds_mda.mdi_slotvcnt - (nbnoalloc + 2);
		cslot  = (nmtoc + nbnoalloc) % (mp->pds_mda.mdi_slotvcnt - 1);
	}


	/* Walkthrough all MDCs and exclude MDCs that are almost full */
	for (nidx = 0, sidx = 0; nidx < num_mdc; nidx++) {

		cslot = cslot % (mp->pds_mda.mdi_slotvcnt - 1) + 1;

		if (cslot == 0)
			cslot = 1;

		cinfo = &mp->pds_mda.mdi_slotv[cslot];
		pco_cnt = &(cinfo->mmi_pco_cnt);

		cap  = atomic64_read(&pco_cnt->pcc_cap);
		used = atomic64_read(&pco_cnt->pcc_len);

		if ((cap - used) < (cap / 400)) {
			/* consider < .25% free space as full */
			mp_pr_warn("MDC slot %u almost full", cslot);
			continue;
		}
		sarray[sidx++] = cinfo;
		cinfo->mmi_credit.ci_free = cap - used;
	}

	/* Sort the array with decreasing order of space */
	sort((void *)sarray, sidx,
			 sizeof(sarray[0]), pmd_compare_free_space, NULL);

	/* Count of valid MDCs in the array but no more than
	 * MPOOL_MDC_SET_SZ. All the testing prior to this implementation
	 * has been done with MDCs MPOOL_MDC_SET_SZ-3 therefore sticking
	 * with MPOOL_MDC_SET_SZ for now. A larger set can be considered
	 * later.
	 */
	num_mdc = min_t(u8, sidx, MPOOL_MDC_SET_SZ);


	/* Calculate total free space across the chosen MDC set */
	for (sidx = 0, free = 0; sidx < num_mdc; sidx++) {
		cinfo = sarray[sidx];
		free += cinfo->mmi_credit.ci_free;
		slotnum[sidx] = cinfo->mmi_credit.ci_slot;
	}

	/* Assign credit to MDCs in the MDC set. Credit is relative and
	 * will not exceed the total slots in mds_tbl
	 */
	for (sidx = 0, credit = 0; sidx < num_mdc; sidx++) {

		cinfo = &mp->pds_mda.mdi_slotv[slotnum[sidx]];
		cinfo->mmi_credit.ci_credit = (MDC_TBL_SZ *
					cinfo->mmi_credit.ci_free) / free;
		credit += cinfo->mmi_credit.ci_credit;
	}
	assert(credit <= MDC_TBL_SZ);

	/* If the credit is not equal to the table size, assign
	 * credits so table can be filled all the way.
	 */
	if (credit < MDC_TBL_SZ) {

		credit = MDC_TBL_SZ - credit;

		sidx = 0;
		while (credit > 0) {
			sidx = (sidx % num_mdc);
			cinfo = &mp->pds_mda.mdi_slotv[slotnum[sidx]];
			cinfo->mmi_credit.ci_credit += 1;
			sidx++;
			credit--;
		}
	}

	pmd_update_mds_tbl(mp, num_mdc, slotnum);
}

/**
 * pmd_mdc_alloc_set() - allocates a set of MDCs
 * @mp: mpool descriptor
 *
 * Creates MDCs in multiple of MPOOL_MDC_SET_SZ. If allocation had
 * failed in prior iteration allocate MDCs to make it even multiple
 * of MPOOL_MDC_SET_SZ.
 *
 * Locking: lock should not be held when calling this function.
 */

void pmd_mdc_alloc_set(struct mpool_descriptor *mp)
{
	u8       mdc_cnt;
	u8       sidx;
	merr_t   err;

	/* assert(!spin_is_locked(&mp->pds_mda.mdi_slotvlock)); */

	/* MDCs are created in multiple of MPOOL_MDC_SET_SZ.
	 * However, if past allocation had failed there may not be an
	 * even multiple of MDCs in that case create any remaining
	 * MDCs to get an even multiple.
	 */

	mdc_cnt =  MPOOL_MDC_SET_SZ -
			((mp->pds_mda.mdi_slotvcnt - 1) % MPOOL_MDC_SET_SZ);

	mdc_cnt = min(mdc_cnt,
			(u8)(MDC_SLOTS - (mp->pds_mda.mdi_slotvcnt)));

	for (sidx = 1; sidx <= mdc_cnt; sidx++) {
		err = pmd_mdc_alloc(mp, mp->pds_params.mp_mdcncap, 0);
		if (err) {
			mp_pr_err("mpool %s, only %u of %u MDCs created",
				  err, mp->pds_name, sidx-1, mdc_cnt);

			/* For MDCN creation failure ignore the error.
			 * Attempt to create any remaining MDC next time
			 * next time new mdcs are required.
			 */
			err = 0;
			break;
		}
	}
}


/**
 * pmd_need_compact() - determine if MDCi corresponding to cslot
 *	need compaction of not.
 * @mp:
 * @cslot:
 *
 * The MDCi needs compaction if the active mlog is above some threshold and
 * if there is enough garbage (that can be eliminated by the compaction).
 *
 * Locking: not lock need to be held when calling this function.
 *	as a result of not holding lock the result may be off if a compaction
 *	of MDCi (with i = cslot) is taking place at the same time.
 */
static bool pmd_need_compact(
	struct mpool_descriptor    *mp,
	u8                          cslot,
	char                       *msgbuf,
	size_t                      msgsz)
{
	struct pre_compact_ctrs    *pco_cnt;
	struct pmd_mdc_info        *cinfo;

	u64 rec, cobj, len, cap;
	u32 garbage, pct;

	assert(cslot > 0);

	cinfo = &mp->pds_mda.mdi_slotv[cslot];
	pco_cnt = &(cinfo->mmi_pco_cnt);

	cap = atomic64_read(&pco_cnt->pcc_cap);
	if (cap == 0)
		return false; /* MDC closed for now. */

	len = atomic64_read(&pco_cnt->pcc_len);
	rec = atomic_read(&pco_cnt->pcc_cr) +
		atomic_read(&pco_cnt->pcc_up) +
		atomic_read(&pco_cnt->pcc_del) +
		atomic_read(&pco_cnt->pcc_er);
	cobj = atomic_read(&pco_cnt->pcc_cobj);

	pct = (len * 100) / cap;
	if (pct < mp->pds_params.mp_pcopctfull)
		return false; /* Active mlog not filled enough */

	if (rec > cobj) {
		garbage = (rec - cobj) * 100;
		garbage /= rec;
	} else {

		/* We may arrive here rarely if the caller doesn't
		 * hold the compact lock. In that case, the update of
		 * the counters may be seen out of order or a compaction
		 * may take place at the same time.
		 */
		garbage = 0;
	}

	if (garbage < mp->pds_params.mp_pcopctgarbage)
		return false; /* Insufficient garbage to compact */

	if (msgbuf)
		snprintf(msgbuf, msgsz,
			 "bytes used %lu, total %lu, pct %u, records %lu, objects %lu, garbage %u",
			 (ulong)len, (ulong)cap, pct, (ulong)rec,
			 (ulong)cobj, garbage);

	return true;
}

/**
 * pmd_precompact() - precompact an mpool MDC
 * @work:
 *
 * The goal of this thread is to minimize the application objects commit time.
 * This thread pre compacts the MDC1/255. As a consequence MDC1/255 compaction
 * does not occurs in the context of an application object commit.
 */
static void pmd_precompact(struct work_struct *work)
{
	struct pre_compact_ctrl    *pco;
	struct mpool_descriptor    *mp;
	struct pmd_mdc_info        *cinfo;

	char    msgbuf[128];
	uint    nmtoc, delay;
	bool    compact;
	u8      cslot;

	pco = container_of(work, typeof(*pco), pco_dwork.work);
	mp = pco->pco_mp;

	nmtoc = atomic_fetch_add(1, &pco->pco_nmtoc);

	/* Only compact MDC1/255 not MDC0. */
	cslot = (nmtoc % (mp->pds_mda.mdi_slotvcnt - 1)) + 1;

	/* Check if the next mpool mdc to compact needs compaction.
	 *
	 * Note that this check is done without taking any lock.
	 * This is safe because the mpool MDCs don't go away as long as
	 * the mpool is activated. The mpool can't deactivate before
	 * this thread exit.
	 */
	compact = pmd_need_compact(mp, cslot, NULL, 0);
	if (compact) {
		cinfo = &mp->pds_mda.mdi_slotv[cslot];

		/* Check a second time while we hold the compact lock
		 * to avoid doing a useless compaction.
		 */
		pmd_mdc_lock(&cinfo->mmi_compactlock, cslot);
		compact = pmd_need_compact(mp, cslot, msgbuf, sizeof(msgbuf));
		if (compact)
			pmd_mdc_compact(mp, cslot);
		pmd_mdc_unlock(&cinfo->mmi_compactlock);

		if (compact)
			mp_pr_info("mpool %s, MDC%u %s",
				   mp->pds_name, cslot, msgbuf);
	}

	/* If running low on MDC space create new MDCs */
	if (pmd_mdc_needed(mp))
		pmd_mdc_alloc_set(mp);

	pmd_update_credit(mp);

	delay = clamp_t(uint, mp->pds_params.mp_pcoperiod, 1, 3600);

	queue_delayed_work(mp->pds_workq, &pco->pco_dwork,
			   msecs_to_jiffies(delay * 1000));
}

void pmd_precompact_start(struct mpool_descriptor *mp)
{
	struct pre_compact_ctrl *pco;

	pco = &mp->pds_pco;
	pco->pco_mp = mp;
	atomic_set(&pco->pco_nmtoc, 0);

	INIT_DELAYED_WORK(&pco->pco_dwork, pmd_precompact);
	queue_delayed_work(mp->pds_workq, &pco->pco_dwork, 1);
}

void pmd_precompact_stop(struct mpool_descriptor *mp)
{
	cancel_delayed_work_sync(&mp->pds_pco.pco_dwork);
}

/*
 * pmd_mlogid2cslot() - Given an mlog object ID which makes one of the mpool
 *	core MDCs (MDCi with i >0), it returns i.
 *	Given an client created object ID (mblock or mlog), it returns -1.
 * @mlogid:
 */
static int pmd_mlogid2cslot(u64 mlogid)
{
	u64 uniq;

	if (pmd_objid_type(mlogid) != OMF_OBJ_MLOG)
		return -1;
	if (objid_slot(mlogid))
		return -1;
	uniq = objid_uniq(mlogid);
	if (uniq > (2 * MDC_SLOTS) - 1)
		return -1;

	return(uniq/2);
}

void
pmd_precompact_alsz(struct mpool_descriptor *mp, u64 objid, u64 len, u64 cap)
{
	struct pre_compact_ctrs    *pco_cnt;
	struct pmd_mdc_info        *cinfo;
	int ret;
	u8  cslot; /* MDCi index, aka "i" */


	ret = pmd_mlogid2cslot(objid);
	if (ret <= 0)
		/* Nothing to record, not a MDC1/255 */
		return;

	cslot = ret;
	cinfo = &mp->pds_mda.mdi_slotv[cslot];
	pco_cnt = &(cinfo->mmi_pco_cnt);
	atomic64_set(&pco_cnt->pcc_len, len);
	atomic64_set(&pco_cnt->pcc_cap, cap);
}

void pmd_mpool_usage(struct mpool_descriptor *mp, struct mp_usage *usage)
{
	int    sidx;
	u16    slotvcnt;

	/*
	 * Get a local copy of MDC count (slotvcnt), and then drop the lock
	 * It's okay another MDC is added concurrently, since pds_ds_info
	 * is always stale by design
	 */
	spin_lock(&mp->pds_mda.mdi_slotvlock);
	slotvcnt = mp->pds_mda.mdi_slotvcnt;
	spin_unlock(&mp->pds_mda.mdi_slotvlock);

	for (sidx = 1; sidx < slotvcnt; sidx++) {
		struct pmd_mdc_stats   *pms;
		struct pmd_mdc_info    *cinfo;

		cinfo = &mp->pds_mda.mdi_slotv[sidx];
		pms   = &cinfo->mmi_stats;

		mutex_lock(&cinfo->mmi_stats_lock);
		usage->mpu_mblock_alen += pms->pms_mblock_alen;
		usage->mpu_mblock_wlen += pms->pms_mblock_wlen;
		usage->mpu_mlog_alen   += pms->pms_mlog_alen;
		usage->mpu_mblock_cnt  += pms->pms_mblock_cnt;
		usage->mpu_mlog_cnt    += pms->pms_mlog_cnt;
		mutex_unlock(&cinfo->mmi_stats_lock);
	}

	if (slotvcnt < 2)
		return;

	usage->mpu_alen = (usage->mpu_mblock_alen + usage->mpu_mlog_alen);
	usage->mpu_wlen = (usage->mpu_mblock_wlen + usage->mpu_mlog_alen);
}

static merr_t
pmd_write_meta_to_latest_version(
	struct mpool_descriptor   *mp,
	bool                       permitted,
	struct mpool_devrpt       *devrpt)
{
	struct pmd_mdc_info *cinfo;
	struct pmd_mdc_info *cinfo_converted = NULL;
	char	buf1[MAX_MDCCVERSTR] __maybe_unused;
	char	buf2[MAX_MDCCVERSTR] __maybe_unused;
	merr_t	err;
	u32	cslot;

	/*
	 * Compact MDC0 first (before MDC1-255 compaction appends in MDC0) to
	 * avoid having a potential mix of new and old records in MDC0.
	 */
	for (cslot = 0; cslot < mp->pds_mda.mdi_slotvcnt; cslot++) {
		cinfo = &mp->pds_mda.mdi_slotv[cslot];

		/*
		 * At that point the version on media should be smaller or
		 * equal to the latest version supported by this binary.
		 * If it is not the case, the activate fails earlier.
		 */
		if (upg_ver_cmp(&(cinfo->mmi_mdccver), "==",
				upg_mdccver_latest()))
			continue;

		upg_mdccver2str(&cinfo->mmi_mdccver, buf1, sizeof(buf1));
		upg_mdccver2str(upg_mdccver_latest(), buf2, sizeof(buf2));

		if (!permitted) {
			const char *com1 =
				upg_mdccver_comment(&cinfo->mmi_mdccver);
			const char *com2 = upg_mdccver_latest_comment();

			mpool_devrpt(devrpt, MPOOL_RC_ERRMSG, -1,
				     "metadata upgrade needed from version %s (%s) to %s (%s)",
				     buf1, com1, buf2, com2);

			err = merr(EPERM);
			mp_pr_err("mpool %s, MDC%u upgrade needed from version %s to %s",
				  err, mp->pds_name, cslot, buf1, buf2);
			return err;
		}

		mp_pr_info("mpool %s, MDC%u upgraded from version %s to %s",
			   mp->pds_name, cslot, buf1, buf2);

		cinfo_converted = cinfo;

		pmd_mdc_lock(&cinfo->mmi_compactlock, cslot);
		err = pmd_mdc_compact(mp, cslot);
		pmd_mdc_unlock(&cinfo->mmi_compactlock);

		if (ev(err)) {
			mpool_devrpt(devrpt, MPOOL_RC_MDC_COMPACT_ACTIVATE,
				     -1, NULL);
			return err;
		}
	}

	if (cinfo_converted != NULL)
		mp_pr_info("mpool %s, converted MDC from version %s to %s",
			   mp->pds_name,
			   upg_mdccver2str(&(cinfo_converted->mmi_mdccver),
					   buf1, sizeof(buf1)),
			   upg_mdccver2str(upg_mdccver_latest(),
					   buf2, sizeof(buf2)));

	return 0;
}

merr_t pmd_mdc_addrec_version(struct mpool_descriptor *mp, u8 cslot)
{
	struct omf_mdcrec_data cdr;

	cdr.omd_rtype = OMF_MDR_VERSION;
	upg_mdccver_latest2(&(cdr.u.omd_version));

	return pmd_mdc_addrec(mp, cslot, &cdr);
}
