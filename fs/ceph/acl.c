// SPDX-License-Identifier: GPL-2.0-only
/*
 * linux/fs/ceph/acl.c
 *
 * Copyright (C) 2013 Guangliang Zhao, <lucienchao@gmail.com>
 */

#include <linux/ceph/ceph_debug.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/xattr.h>
#include <linux/posix_acl_xattr.h>
#include <linux/posix_acl.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include "super.h"
#include "mds_client.h"

void ceph_init_inode_acl_cache(struct inode *inode)
{
	struct posix_acl *acl, *default_acl;

	acl = xchg(&inode->i_acl, ACL_DONT_CACHE);
	default_acl = xchg(&inode->i_default_acl, ACL_DONT_CACHE);
	if (!is_uncached_acl(acl))
		posix_acl_release(acl);
	if (!is_uncached_acl(default_acl))
		posix_acl_release(default_acl);
}

static void ceph_acl_read_begin(struct inode *inode, struct ceph_acl_read_ctx *ctx)
{
	struct ceph_mds_client *mdsc = ceph_sb_to_mdsc(inode->i_sb);

	ctx->hit = NULL;
	ctx->armed = false;
	ctx->copied = false;
	ctx->view_changed = false;
	spin_lock(&mdsc->quarantine_lock);
	ctx->epoch = atomic64_read(&mdsc->quarantine_epoch);
	ctx->negs = atomic64_read(&mdsc->quarantine_negs);
	ctx->supported = READ_ONCE(mdsc->quarantine_supported);
	ctx->stable = !(ctx->epoch & 1) && !atomic_read(&mdsc->quarantine_splices);
	spin_unlock(&mdsc->quarantine_lock);
}

int __ceph_acl_view_check(struct inode *inode, struct ceph_acl_read_ctx *ctx)
{
	struct ceph_mds_client *mdsc = ceph_sb_to_mdsc(inode->i_sb);
	int err = 0;

	spin_lock(&mdsc->quarantine_lock);
	if (ceph_inode_is_shutdown(inode))
		err = -ESTALE;
	else if (current_cred() != ctx->cred || ceph_inode_quarantine_direct(inode))
		err = -EACCES;
	else if (!ctx->stable ||
		 ctx->epoch != (u64)atomic64_read(&mdsc->quarantine_epoch) ||
		 ctx->negs != (u64)atomic64_read(&mdsc->quarantine_negs) ||
		 ctx->supported != READ_ONCE(mdsc->quarantine_supported) ||
		 (ctx->stable && atomic_read(&mdsc->quarantine_splices)))
		err = -EAGAIN;
	spin_unlock(&mdsc->quarantine_lock);
	return err;
}

bool __ceph_acl_cache_valid(struct inode *inode, struct ceph_acl_read_ctx *ctx)
{
	struct ceph_inode_info *ci = ceph_inode(inode);

	lockdep_assert_held(&ci->i_ceph_lock);
	if (!ctx->stable || ceph_in_snap(inode) ||
	    test_bit(CEPH_I_ASYNC_CREATE_BIT, &ci->i_ceph_flags) ||
	    !ci->i_xattrs.version || ci->i_xattrs.need_refill ||
	    !ceph_acl_xattr_data_known(&ci->i_xattrs) ||
	    !__ceph_caps_issued_mask_metric(ci, CEPH_CAP_XATTR_SHARED, 1) ||
	    (ctx->supported &&
	     (!__ceph_quarantine_head_cap_valid(ci) ||
	      !__ceph_caps_issued_mask_metric(ci, CEPH_CAP_AUTH_SHARED, 1))))
		return false;

	return !__ceph_acl_view_check(inode, ctx);
}

static bool ceph_acl_read_finish(struct inode *inode, struct ceph_acl_read_ctx *ctx,
				 struct posix_acl *acl)
{
	struct ceph_inode_info *ci = ceph_inode(inode);
	struct posix_acl *old = NULL;
	bool valid;

	spin_lock(&ci->i_ceph_lock);
	valid = !ci->i_xattrs.need_refill &&
		ceph_acl_xattr_data_known(&ci->i_xattrs) &&
		ci->i_xattrs.version == ctx->version &&
		ci->i_metadata_cap_gen == ctx->cap_gen &&
		(ctx->armed ? __ceph_acl_cache_valid(inode, ctx) :
		 !__ceph_acl_view_check(inode, ctx));
	if (valid && ctx->armed && !IS_ERR_OR_NULL(acl)) {
		old = ci->i_parsed_acl[ctx->type].acl;
		ci->i_parsed_acl[ctx->type].acl = posix_acl_dup(acl);
		ci->i_parsed_acl[ctx->type].version = ctx->version;
		ci->i_parsed_acl[ctx->type].cap_gen = ctx->cap_gen;
	}
	spin_unlock(&ci->i_ceph_lock);
	posix_acl_release(old);
	return valid;
}

struct posix_acl *ceph_get_acl(struct inode *inode, int type, bool rcu)
{
	struct ceph_client *cl = ceph_inode_to_client(inode);
	struct ceph_inode_info *ci = ceph_inode(inode);
	u64 cap_gen = 0;
	int size;
	unsigned int retry_cnt = 0;
	unsigned int retries = 0;
	bool refilling = false;
	bool direct_refreshed = false, direct_denial = false;
	const char *name;
	char *value = NULL;
	struct posix_acl *acl;
	struct ceph_acl_read_ctx ctx = {};

	if (ceph_snap(inode) == CEPH_SNAPDIR || S_ISLNK(inode->i_mode))
		return NULL;

	if (rcu)
		return ERR_PTR(-ECHILD);

	switch (type) {
	case ACL_TYPE_ACCESS:
		name = XATTR_NAME_POSIX_ACL_ACCESS;
		break;
	case ACL_TYPE_DEFAULT:
		name = XATTR_NAME_POSIX_ACL_DEFAULT;
		break;
	default:
		BUG();
	}
	ctx.type = type == ACL_TYPE_DEFAULT;
	ctx.cred = get_current_cred();

retry:
	if (++retries > 4) {
		size = ceph_quarantine_backoff(inode);
		if (size) {
			acl = ERR_PTR(size);
			goto out;
		}
		retries = 0;
	}
	if (!ceph_in_snap(inode) && ceph_inode_quarantine_direct(inode)) {
		if (direct_refreshed) {
			direct_denial = true;
			size = -EACCES;
			goto handle_result;
		}
		direct_refreshed = true;
		size = ceph_quarantine_recover_direct(inode);
		if (size == -EACCES) {
			direct_denial = true;
			goto handle_result;
		}
		if (size) {
			/* Neither error means that this inode has no ACL. */
			acl = ERR_PTR(size == -EAGAIN || size == -EOPNOTSUPP ?
				      -EIO : size);
			goto out;
		}
	}
	ceph_acl_read_begin(inode, &ctx);
	if (!ctx.stable) {
		size = ceph_quarantine_wait(inode);
		if (size) {
			acl = ERR_PTR(size);
			goto out;
		}
		goto retry;
	}
	spin_lock(&ci->i_ceph_lock);
	cap_gen = ci->i_metadata_cap_gen;
	refilling |= ci->i_xattrs.need_refill;
	spin_unlock(&ci->i_ceph_lock);
	size = ceph_get_acl_xattr(inode, name, "", 0, type == ACL_TYPE_ACCESS, &ctx);
	if (ctx.hit) {
		acl = ctx.hit;
		goto out;
	}
	if (size > 0) {
		value = kzalloc(size, GFP_NOFS);
		if (!value) {
			acl = ERR_PTR(-ENOMEM);
			goto out;
		}
		size = ceph_get_acl_xattr(inode, name, value, size,
					  type == ACL_TYPE_ACCESS, &ctx);
		if (ctx.hit) {
			acl = ctx.hit;
			goto out;
		}
	}

handle_result:
	if (size == -EACCES && !direct_refreshed &&
	    !ceph_in_snap(inode) && ceph_inode_quarantine_direct(inode)) {
		kfree(value);
		value = NULL;
		goto retry;
	}
	if (size == -ERANGE && retry_cnt < 10) {
		retry_cnt++;
		kfree(value);
		value = NULL;
		goto retry;
	}
	if (size == -EAGAIN && type == ACL_TYPE_ACCESS &&
	    S_ISDIR(inode->i_mode) && !ceph_in_snap(inode) &&
	    READ_ONCE(ceph_sb_to_mdsc(inode->i_sb)->quarantine_supported)) {
		int caps = CEPH_CAP_AUTH_SHARED | CEPH_CAP_XATTR_SHARED;
		bool cached, need_refill;

		kfree(value);
		value = NULL;
		if (ctx.view_changed)
			goto retry;
		if (ceph_inode_quarantine_direct(inode)) {
			if (!direct_refreshed)
				goto retry;
			direct_denial = true;
			size = -EACCES;
		} else {
			spin_lock(&ci->i_ceph_lock);
			need_refill = ci->i_xattrs.need_refill;
			cached = ci->i_xattrs.version &&
				 __ceph_caps_issued_mask_metric(ci, caps, 1);
			spin_unlock(&ci->i_ceph_lock);
			if (!cached || need_refill) {
				size = ceph_head_stat_authorize_caps(inode, caps);
				if (size == -EAGAIN)
					goto retry;
				if (size && size != -EACCES) {
					acl = ERR_PTR(size == -EOPNOTSUPP ? -EIO : size);
					goto out;
				}
				if (!size) {
					spin_lock(&ci->i_ceph_lock);
					cached = ci->i_xattrs.version &&
						 !ci->i_xattrs.need_refill &&
						 ceph_acl_xattr_data_known(&ci->i_xattrs) &&
						 __ceph_caps_issued_mask_metric(ci, caps, 1);
					spin_unlock(&ci->i_ceph_lock);
					if (!cached) {
						acl = ERR_PTR(-EIO);
						goto out;
					}
				}
			}
			if (cached || !size)
				goto retry;
		}
	}
	if (size == -EAGAIN) {
		bool invalidated;

		spin_lock(&ci->i_ceph_lock);
		invalidated = ci->i_xattrs.need_refill ||
			      ci->i_metadata_cap_gen != cap_gen;
		spin_unlock(&ci->i_ceph_lock);
		/* Re-read xattrs invalidated while building their index. */
		if (invalidated || ctx.stable || ctx.view_changed) {
			refilling |= invalidated;
			kfree(value);
			value = NULL;
			goto retry;
		}
	}

	if (size > 0) {
		acl = posix_acl_from_xattr(&init_user_ns, value, size);
		if (ctx.copied && !ceph_acl_read_finish(inode, &ctx, acl)) {
			if (!IS_ERR(acl))
				posix_acl_release(acl);
			kfree(value);
			value = NULL;
			goto retry;
		}
	} else if (size == -ENODATA || size == 0) {
		acl = NULL;
	} else if (size == -EACCES) {
		if (!direct_denial) {
			spin_lock(&ci->i_ceph_lock);
			refilling |= ci->i_xattrs.need_refill ||
				     ci->i_metadata_cap_gen != cap_gen;
			spin_unlock(&ci->i_ceph_lock);
		}
		/* A failed ACL fetch is not a DAC denial to be overridden. */
		if (type == ACL_TYPE_ACCESS && !ceph_in_snap(inode) &&
		    (direct_denial || ceph_inode_quarantine_direct(inode) ||
		     S_ISDIR(inode->i_mode) || refilling) &&
		    READ_ONCE(ceph_sb_to_mdsc(inode->i_sb)->quarantine_supported))
			acl = ERR_PTR(-EPERM);
		else
			acl = ERR_PTR(-EACCES);
	} else if ((size == -ERESTARTSYS || size == -EINTR || size == -ESTALE) &&
		   type == ACL_TYPE_ACCESS && S_ISDIR(inode->i_mode) &&
		   !ceph_in_snap(inode) &&
		   READ_ONCE(ceph_sb_to_mdsc(inode->i_sb)->quarantine_supported)) {
		acl = ERR_PTR(size);
	} else {
		/* The VFS uses -EAGAIN to fall back to the mode bits. */
		pr_err_ratelimited_client(cl, "%llx.%llx failed, err=%d\n",
					  ceph_vinop(inode), size);
		acl = ERR_PTR(-EIO);
	}

out:
	kfree(value);
	put_cred(ctx.cred);

	return acl;
}

static int __ceph_set_acl(struct mnt_idmap *idmap, struct inode *inode,
			  struct posix_acl *acl, int type, bool update_mode)
{
	int ret = 0;
	size_t size = 0;
	const char *name = NULL;
	char *value = NULL;
	struct iattr newattrs;
	struct timespec64 old_ctime = inode_get_ctime(inode);
	umode_t new_mode = inode->i_mode, old_mode = inode->i_mode;

	if (ceph_in_snap(inode)) {
		ret = -EROFS;
		goto out;
	}

	switch (type) {
	case ACL_TYPE_ACCESS:
		name = XATTR_NAME_POSIX_ACL_ACCESS;
		if (acl) {
			if (update_mode) {
				ret = posix_acl_update_mode(idmap, inode,
							    &new_mode, &acl);
				if (ret)
					goto out;
			} else {
				ret = posix_acl_equiv_mode(acl, NULL);
				if (ret < 0)
					goto out;
				if (!ret)
					acl = NULL;
				ret = 0;
			}
		}
		break;
	case ACL_TYPE_DEFAULT:
		if (!S_ISDIR(inode->i_mode)) {
			ret = acl ? -EINVAL : 0;
			goto out;
		}
		name = XATTR_NAME_POSIX_ACL_DEFAULT;
		break;
	default:
		ret = -EINVAL;
		goto out;
	}

	if (acl) {
		value = posix_acl_to_xattr(&init_user_ns, acl, &size, GFP_NOFS);
		if (!value) {
			ret = -ENOMEM;
			goto out;
		}
	}

	if (update_mode && new_mode != old_mode) {
		newattrs.ia_ctime = current_time(inode);
		newattrs.ia_mode = new_mode;
		newattrs.ia_valid = ATTR_MODE | ATTR_CTIME;
		ret = __ceph_setattr(idmap, inode, &newattrs, NULL);
		if (ret)
			goto out_free;
	}

	ret = __ceph_setxattr(inode, name, value, size, 0);
	if (ret) {
		if (update_mode && new_mode != old_mode) {
			newattrs.ia_ctime = old_ctime;
			newattrs.ia_mode = old_mode;
			newattrs.ia_valid = ATTR_MODE | ATTR_CTIME;
			__ceph_setattr(idmap, inode, &newattrs, NULL);
		}
		goto out_free;
	}

out_free:
	kfree(value);
out:
	return ret;
}

int ceph_set_acl(struct mnt_idmap *idmap, struct dentry *dentry,
		 struct posix_acl *acl, int type)
{
	return __ceph_set_acl(idmap, d_inode(dentry), acl, type, true);
}

/* __posix_acl_chmod() cloned the ACL and checked the entries used here. */
static void ceph_acl_chmod_mode(struct posix_acl *acl, umode_t mode)
{
	struct posix_acl_entry *entry, *end, *group = NULL, *mask = NULL;

	FOREACH_ACL_ENTRY(entry, acl, end) {
		switch (entry->e_tag) {
		case ACL_USER_OBJ:
			entry->e_perm = (mode & 0700) >> 6;
			break;
		case ACL_GROUP_OBJ:
			group = entry;
			break;
		case ACL_MASK:
			mask = entry;
			break;
		case ACL_OTHER:
			entry->e_perm = mode & 0007;
			break;
		}
	}
	if (mask)
		mask->e_perm = (mode & 0070) >> 3;
	else
		group->e_perm = (mode & 0070) >> 3;
}

int ceph_acl_chmod(struct mnt_idmap *idmap, struct dentry *dentry,
		   umode_t mode)
{
	struct inode *inode = d_inode(dentry);
	struct ceph_inode_info *ci = ceph_inode(inode);
	struct posix_acl *acl;
	umode_t acl_mode = mode;
	int ret;

	if (!IS_POSIXACL(inode))
		return 0;
	if (!inode->i_op->set_acl)
		return -EOPNOTSUPP;

	acl = get_inode_acl(inode, ACL_TYPE_ACCESS);
	if (IS_ERR_OR_NULL(acl)) {
		if (acl == ERR_PTR(-EOPNOTSUPP))
			return 0;
		return PTR_ERR(acl);
	}

	ret = __posix_acl_chmod(&acl, GFP_KERNEL, mode);
	if (ret)
		return ret;
	/* A current auth cap may carry a newer mode than this chmod. */
	spin_lock(&ci->i_ceph_lock);
	if (__ceph_caps_issued_mask_metric(ci, CEPH_CAP_AUTH_SHARED, 0))
		mode = inode->i_mode;
	spin_unlock(&ci->i_ceph_lock);
	if ((acl_mode ^ mode) & 0777)
		ceph_acl_chmod_mode(acl, mode);
	ret = __ceph_set_acl(idmap, inode, acl, ACL_TYPE_ACCESS, false);
	posix_acl_release(acl);
	return ret;
}

int ceph_pre_init_acls(struct inode *dir, umode_t *mode,
		       struct ceph_acl_sec_ctx *as_ctx)
{
	struct posix_acl *acl, *default_acl;
	size_t val_size1 = 0, val_size2 = 0;
	struct ceph_pagelist *pagelist = NULL;
	void *tmp_buf1 = NULL, *tmp_buf2 = NULL;
	int err;

	err = posix_acl_create(dir, mode, &default_acl, &acl);
	if (err)
		return err;

	if (acl) {
		err = posix_acl_equiv_mode(acl, mode);
		if (err < 0)
			goto out_err;
		if (err == 0) {
			posix_acl_release(acl);
			acl = NULL;
		}
	}

	if (!default_acl && !acl)
		return 0;

	err = -ENOMEM;
	pagelist = ceph_pagelist_alloc(GFP_KERNEL);
	if (!pagelist)
		goto out_err;

	err = ceph_pagelist_reserve(pagelist, PAGE_SIZE);
	if (err)
		goto out_err;

	ceph_pagelist_encode_32(pagelist, acl && default_acl ? 2 : 1);

	if (acl) {
		size_t len = strlen(XATTR_NAME_POSIX_ACL_ACCESS);

		err = -ENOMEM;
		tmp_buf1 = posix_acl_to_xattr(&init_user_ns, acl,
					      &val_size1, GFP_KERNEL);
		if (!tmp_buf1)
			goto out_err;
		err = ceph_pagelist_reserve(pagelist, len + val_size1 + 8);
		if (err)
			goto out_err;
		ceph_pagelist_encode_string(pagelist, XATTR_NAME_POSIX_ACL_ACCESS,
					    len);
		ceph_pagelist_encode_32(pagelist, val_size1);
		ceph_pagelist_append(pagelist, tmp_buf1, val_size1);
	}
	if (default_acl) {
		size_t len = strlen(XATTR_NAME_POSIX_ACL_DEFAULT);

		err = -ENOMEM;
		tmp_buf2 = posix_acl_to_xattr(&init_user_ns, default_acl,
					      &val_size2, GFP_KERNEL);
		if (!tmp_buf2)
			goto out_err;
		err = ceph_pagelist_reserve(pagelist, len + val_size2 + 8);
		if (err)
			goto out_err;
		ceph_pagelist_encode_string(pagelist,
					  XATTR_NAME_POSIX_ACL_DEFAULT, len);
		ceph_pagelist_encode_32(pagelist, val_size2);
		ceph_pagelist_append(pagelist, tmp_buf2, val_size2);
	}

	kfree(tmp_buf1);
	kfree(tmp_buf2);

	as_ctx->acl = acl;
	as_ctx->default_acl = default_acl;
	as_ctx->pagelist = pagelist;
	return 0;

out_err:
	posix_acl_release(acl);
	posix_acl_release(default_acl);
	kfree(tmp_buf1);
	kfree(tmp_buf2);
	if (pagelist)
		ceph_pagelist_release(pagelist);
	return err;
}
