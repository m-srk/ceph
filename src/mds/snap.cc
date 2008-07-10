// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004- Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#include "snap.h"
#include "MDCache.h"
#include "MDS.h"

#include "messages/MClientSnap.h"

/*
 * SnapRealm
 */

#define dout(x) if (x <= g_conf.debug_mds) *_dout << dbeginl << g_clock.now() \
						  << " mds" << mdcache->mds->get_nodeid() \
						  << ".cache.snaprealm(" << inode->ino() \
						  << " " << this << ") "

bool SnapRealm::open_parents(MDRequest *mdr)
{
  dout(10) << "open_parents" << dendl;

  // make sure my current parents' parents are open...
  if (parent) {
    dout(10) << " parent is " << *parent
	     << " on " << *parent->inode << dendl;
    if (!parent->open_parents(mdr))
      return false;
  }

  // and my past parents too!
  for (map<snapid_t, snaplink_t>::iterator p = past_parents.begin();
       p != past_parents.end();
       p++) {    
    CInode *parent = mdcache->get_inode(p->second.dirino);
    if (parent)
      continue;
    mdcache->open_remote_ino(p->second.dirino, mdr, 
			     new C_MDS_RetryRequest(mdcache, mdr));
    return false;
  }
  return true;
}

/*
 * get list of snaps for this realm.  we must include parents' snaps
 * for the intervals during which they were our parent.
 */
void SnapRealm::get_snap_set(set<snapid_t> &s, snapid_t first, snapid_t last)
{
  dout(10) << "get_snap_set [" << first << "," << last << "] on " << *this << dendl;

  // include my snaps within interval [first,last]
  for (map<snapid_t, SnapInfo>::iterator p = snaps.lower_bound(first); // first element >= first
       p != snaps.end() && p->first <= last;
       p++)
    s.insert(p->first);

  // include snaps for parents during intervals that intersect [first,last]
  snapid_t thru = first;
  for (map<snapid_t, snaplink_t>::iterator p = past_parents.lower_bound(first);
       p != past_parents.end() && p->first >= first && p->second.first <= last;
       p++) {
    CInode *oldparent = mdcache->get_inode(p->second.dirino);
    assert(oldparent);  // call open_parents first!
    assert(oldparent->snaprealm);
    
    thru = MIN(last, p->first);
    oldparent->snaprealm->get_snap_set(s, 
				       MAX(first, p->second.first),
				       thru);
    thru++;
  }
  if (thru <= last && parent)
    parent->get_snap_set(s, thru, last);
}

/*
 * build vector in reverse sorted order
 */
vector<snapid_t> *SnapRealm::get_snap_vector()
{
  if (!cached_snaps.size()) {
    dout(10) << "get_snap_vector " << cached_snaps << " (cached)" << dendl;
    return &cached_snaps;
  }

  set<snapid_t> s;
  get_snap_set(s, 0, CEPH_NOSNAP);
  cached_snaps.resize(s.size());
  int i = 0;
  for (set<snapid_t>::reverse_iterator p = s.rbegin(); p != s.rend(); p++)
    cached_snaps[i++] = *p;
  
  dout(10) << "get_snap_vector " << cached_snaps
	   << " (highwater " << snap_highwater << ")" << dendl;
  return &cached_snaps;
}

vector<snapid_t> *SnapRealm::update_snap_vector(snapid_t creating)
{
  if (!snap_highwater) {
    assert(cached_snaps.empty());
    get_snap_vector();
  }
  snap_highwater = creating;
  cached_snaps.insert(cached_snaps.begin(), creating); // FIXME.. we should store this in reverse!
  return &cached_snaps;
}


void SnapRealm::split_at(SnapRealm *child)
{
  dout(10) << "split_at " << *child 
	   << " on " << *child->inode << dendl;

  // split open_children
  dout(10) << " open_children are " << open_children << dendl;
  for (set<SnapRealm*>::iterator p = open_children.begin();
       p != open_children.end(); ) {
    SnapRealm *realm = *p;
    if (realm != child &&
	child->inode->is_ancestor_of(realm->inode)) {
      dout(20) << " child gets child realm " << *realm << " on " << *realm->inode << dendl;
      realm->parent = child;
      child->open_children.insert(realm);
      open_children.erase(p++);
    } else {
      dout(20) << "    keeping child realm " << *realm << " on " << *realm->inode << dendl;
      p++;
    }
  }

  // split inodes_with_caps
  xlist<CInode*>::iterator p = inodes_with_caps.begin();
  while (!p.end()) {
    CInode *in = *p;
    ++p;

    // does inode fall within the child realm?
    CInode *t = in;
    bool under_child = false;
    while (t) {
      t = t->get_parent_dn()->get_dir()->get_inode();
      if (t == child->inode) {
	under_child = true;
	break;
      }
      if (t == in)
	break;
    }
    if (under_child) {
      dout(20) << " child gets " << *in << dendl;
      in->move_to_containing_realm(child);
    } else {
      dout(20) << "    keeping " << *in << dendl;
    }
  }

}
