// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */



// unix-ey fs stuff
#include <unistd.h>
#include <sys/types.h>
#include <time.h>
#include <utime.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <sys/statvfs.h>


#include <iostream>
using namespace std;


// ceph stuff
#include "Client.h"

#include "messages/MStatfs.h"
#include "messages/MStatfsReply.h"

#include "messages/MClientMount.h"
#include "messages/MClientUnmount.h"
#include "messages/MClientSession.h"
#include "messages/MClientReconnect.h"
#include "messages/MClientRequest.h"
#include "messages/MClientRequestForward.h"
#include "messages/MClientReply.h"
#include "messages/MClientFileCaps.h"

#include "messages/MGenericMessage.h"

#include "messages/MMDSGetMap.h"
#include "messages/MMDSMap.h"

#include "osdc/Filer.h"
#include "osdc/Objecter.h"
#include "osdc/ObjectCacher.h"

#include "common/Cond.h"
#include "common/Mutex.h"
#include "common/Logger.h"



#include "config.h"
#undef dout
#define  dout(l)    if (l<=g_conf.debug || l <= g_conf.debug_client) cout << g_clock.now() << " client" << whoami /*<< "." << pthread_self() */ << " "

#define  tout       if (g_conf.client_trace) traceout


// static logger
LogType client_logtype;
Logger  *client_logger = 0;



class C_Client_CloseRelease : public Context {
  Client *cl;
  Inode *in;
public:
  C_Client_CloseRelease(Client *c, Inode *i) : cl(c), in(i) {}
  void finish(int) {
    cl->close_release(in);
  }
};

class C_Client_CloseSafe : public Context {
  Client *cl;
  Inode *in;
public:
  C_Client_CloseSafe(Client *c, Inode *i) : cl(c), in(i) {}
  void finish(int) {
    cl->close_safe(in);
  }
};






// cons/des

Client::Client(Messenger *m, MonMap *mm) : timer(client_lock)
{
  // which client am i?
  whoami = m->get_myname().num();
  monmap = mm;

  mounted = false;
  mount_timeout_event = 0;
  unmounting = false;

  last_tid = 0;
  unsafe_sync_write = 0;

  mdsmap = 0;

  // 
  root = 0;

  lru.lru_set_max(g_conf.client_cache_size);

  // file handles
  free_fd_set.insert(10, 1<<30);

  // set up messengers
  messenger = m;
  messenger->set_dispatcher(this);

  // osd interfaces
  osdmap = new OSDMap();     // initially blank.. see mount()
  objecter = new Objecter(messenger, monmap, osdmap);
  objecter->set_client_incarnation(0);  // client always 0, for now.
  objectcacher = new ObjectCacher(objecter, client_lock);
  filer = new Filer(objecter);

  static int instance_this_process = 0;
  client_instance_this_process = instance_this_process++;
}


Client::~Client() 
{
  tear_down_cache();

  if (objectcacher) { 
    delete objectcacher; 
    objectcacher = 0; 
  }

  if (filer) { delete filer; filer = 0; }
  if (objecter) { delete objecter; objecter = 0; }
  if (osdmap) { delete osdmap; osdmap = 0; }
  if (mdsmap) { delete mdsmap; mdsmap = 0; }

  if (messenger) { delete messenger; messenger = 0; }
}


void Client::tear_down_cache()
{
  // fd's
  for (hash_map<int, Fh*>::iterator it = fd_map.begin();
       it != fd_map.end();
       it++) {
    Fh *fh = it->second;
    dout(1) << "tear_down_cache forcing close of fh " << it->first << " ino " << fh->inode->inode.ino << endl;
    put_inode(fh->inode);
    delete fh;
  }
  fd_map.clear();

  // caps!
  // *** FIXME ***

  // empty lru
  lru.lru_set_max(0);
  trim_cache();
  assert(lru.lru_get_size() == 0);

  // close root ino
  assert(inode_map.size() <= 1);
  if (root && inode_map.size() == 1) {
    delete root;
    root = 0;
    inode_map.clear();
  }

  assert(inode_map.empty());
}



// debug crapola

void Client::dump_inode(Inode *in, set<Inode*>& did)
{
  dout(1) << "dump_inode: inode " << in->ino() << " ref " << in->ref << " dir " << in->dir << endl;

  if (in->dir) {
    dout(1) << "  dir size " << in->dir->dentries.size() << endl;
    //for (hash_map<const char*, Dentry*, hash<const char*>, eqstr>::iterator it = in->dir->dentries.begin();
    for (hash_map<string, Dentry*>::iterator it = in->dir->dentries.begin();
         it != in->dir->dentries.end();
         it++) {
      dout(1) << "    dn " << it->first << " ref " << it->second->ref << endl;
      dump_inode(it->second->inode, did);
    }
  }
}

void Client::dump_cache()
{
  set<Inode*> did;

  if (root) dump_inode(root, did);

  for (hash_map<inodeno_t, Inode*>::iterator it = inode_map.begin();
       it != inode_map.end();
       it++) {
    if (did.count(it->second)) continue;
    
    dout(1) << "dump_cache: inode " << it->first
            << " ref " << it->second->ref 
            << " dir " << it->second->dir << endl;
    if (it->second->dir) {
      dout(1) << "  dir size " << it->second->dir->dentries.size() << endl;
    }
  }
 
}


void Client::init() 
{

  
}

void Client::shutdown() {
  dout(1) << "shutdown" << endl;
  messenger->shutdown();
}




// ===================
// metadata cache stuff

void Client::trim_cache()
{
  unsigned last = 0;
  while (lru.lru_get_size() != last) {
    last = lru.lru_get_size();

    if (lru.lru_get_size() <= lru.lru_get_max())  break;

    // trim!
    Dentry *dn = (Dentry*)lru.lru_expire();
    if (!dn) break;  // done
    
    dout(15) << "trim_cache unlinking dn " << dn->name 
	     << " in dir " << hex << dn->dir->parent_inode->inode.ino 
	     << endl;
    unlink(dn);
  }

  // hose root?
  if (lru.lru_get_size() == 0 && root && root->ref == 0 && inode_map.size() == 1) {
    dout(15) << "trim_cache trimmed root " << root << endl;
    delete root;
    root = 0;
    inode_map.clear();
  }
}

/** insert_inode
 *
 * insert + link a single dentry + inode into the metadata cache.
 */
Inode* Client::insert_inode(Dir *dir, InodeStat *st, const string& dname)
{
  Dentry *dn = NULL;
  if (dir->dentries.count(dname))
    dn = dir->dentries[dname];

  dout(12) << "insert_inode " << dname << " ino " << st->inode.ino 
           << "  size " << st->inode.size
           << "  mtime " << st->inode.mtime
	   << "  mask " << st->mask
	   << " in dir " << dir->parent_inode->inode.ino
           << endl;
  
  if (dn) {
    if (dn->inode->inode.ino == st->inode.ino) {
      touch_dn(dn);
      dout(12) << " had dentry " << dname
               << " with correct ino " << dn->inode->inode.ino
               << endl;
    } else {
      dout(12) << " had dentry " << dname
               << " with WRONG ino " << dn->inode->inode.ino
               << endl;
      unlink(dn);
      dn = NULL;
    }
  }
  
  if (!dn) {
    // have inode linked elsewhere?  -> unlink and relink!
    if (inode_map.count(st->inode.ino)) {
      Inode *in = inode_map[st->inode.ino];
      assert(in);

      if (in->dn) {
        dout(12) << " had ino " << in->inode.ino
                 << " not linked or linked at the right position, relinking"
                 << endl;
        dn = relink(dir, dname, in);
      } else {
        // link
        dout(12) << " had ino " << in->inode.ino
                 << " unlinked, linking" << endl;
        dn = link(dir, dname, in);
      }
    }
  }
  
  if (!dn) {
    Inode *in = new Inode(st->inode, objectcacher);
    inode_map[st->inode.ino] = in;
    dn = link(dir, dname, in);
    dout(12) << " new dentry+node with ino " << st->inode.ino << endl;
  } else {
    // actually update info
    dout(12) << " stat inode mask is " << st->mask << endl;
    if (st->mask & STAT_MASK_BASE) {
      dn->inode->inode = st->inode;
      dn->inode->dirfragtree = st->dirfragtree;  // FIXME look at the mask!
    }

    // ...but don't clobber our mtime, size!
    /* isn't this handled below?
    if ((dn->inode->mask & STAT_MASK_SIZE) == 0 &&
        dn->inode->file_wr_size > dn->inode->inode.size) 
      dn->inode->inode.size = dn->inode->file_wr_size;
    if ((dn->inode->mask & STAT_MASK_MTIME) == 0 &&
        dn->inode->file_wr_mtime > dn->inode->inode.mtime) 
      dn->inode->inode.mtime = dn->inode->file_wr_mtime;
    */
  }

  // OK, we found it!
  assert(dn && dn->inode);

  // save the mask
  dn->inode->mask = st->mask;
  
  // or do we have newer size/mtime from writing?
  if (dn->inode->file_caps() & CAP_FILE_WR) {
    if (dn->inode->file_wr_size > dn->inode->inode.size)
      dn->inode->inode.size = dn->inode->file_wr_size;
    if (dn->inode->file_wr_mtime > dn->inode->inode.mtime)
      dn->inode->inode.mtime = dn->inode->file_wr_mtime;
  }

  // symlink?
  if (dn->inode->inode.is_symlink()) {
    if (!dn->inode->symlink) 
      dn->inode->symlink = new string;
    *(dn->inode->symlink) = st->symlink;
  }

  return dn->inode;
}

/** update_inode_dist
 *
 * update MDS location cache for a single inode
 */
void Client::update_inode_dist(Inode *in, InodeStat *st)
{
  // auth
  in->dir_auth = -1;
  if (!st->dirfrag_auth.empty()) {        // HACK FIXME ******* FIXME FIXME FIXME FIXME dirfrag_t
    in->dir_auth = st->dirfrag_auth.begin()->second; 
  }

  // replicated
  in->dir_replicated = false;
  if (!st->dirfrag_rep.empty())
    in->dir_replicated = true;       // FIXME
  
  // dist
  if (!st->dirfrag_dist.empty()) {   // FIXME
    set<int> dist = st->dirfrag_dist.begin()->second;
    if (dist.empty() && !in->dir_contacts.empty())
      dout(9) << "lost dist spec for " << in->inode.ino 
              << " " << dist << endl;
    if (!dist.empty() && in->dir_contacts.empty()) 
      dout(9) << "got dist spec for " << in->inode.ino 
              << " " << dist << endl;
    in->dir_contacts = dist;
  }
}


/** insert_trace
 *
 * insert a trace from a MDS reply into the cache.
 */
Inode* Client::insert_trace(MClientReply *reply)
{
  Inode *cur = root;
  utime_t now = g_clock.real_now();

  dout(10) << "insert_trace got " << reply->get_trace_in().size() << " inodes" << endl;

  list<string>::const_iterator pdn = reply->get_trace_dn().begin();

  for (list<InodeStat*>::const_iterator pin = reply->get_trace_in().begin();
       pin != reply->get_trace_in().end();
       ++pin) {
    
    if (pin == reply->get_trace_in().begin()) {
      // root
      dout(10) << "insert_trace root" << endl;
      if (!root) {
        // create
        cur = root = new Inode((*pin)->inode, objectcacher);
	dout(10) << "insert_trace new root is " << root << endl;
        inode_map[root->inode.ino] = root;
      }
    } else {
      // not root.
      Dir *dir = cur->open_dir();
      cur = this->insert_inode(dir, *pin, *pdn);
      dout(10) << "insert_trace dn " << *pdn << " ino " << (*pin)->inode.ino << " -> " << cur << endl;
      ++pdn;      

      // move to top of lru!
      if (cur->dn) 
        lru.lru_touch(cur->dn);
    }

    // update dist info
    update_inode_dist(cur, *pin);

    // set cache ttl
    if (g_conf.client_cache_stat_ttl) {
      cur->valid_until = now;
      cur->valid_until += g_conf.client_cache_stat_ttl;
    }
  }

  return cur;
}




Dentry *Client::lookup(filepath& path)
{
  dout(14) << "lookup " << path << endl;

  Inode *cur = root;
  if (!cur) return NULL;

  Dentry *dn = 0;
  for (unsigned i=0; i<path.depth(); i++) {
    dout(14) << " seg " << i << " = " << path[i] << endl;
    if (cur->inode.mode & INODE_MODE_DIR &&
        cur->dir) {
      // dir, we can descend
      Dir *dir = cur->dir;
      if (dir->dentries.count(path[i])) {
        dn = dir->dentries[path[i]];
        dout(14) << " hit dentry " << path[i] << " inode is " << dn->inode << " valid_until " << dn->inode->valid_until << endl;
      } else {
        dout(14) << " dentry " << path[i] << " dne" << endl;
        return NULL;
      }
      cur = dn->inode;
      assert(cur);
    } else {
      return NULL;  // not a dir
    }
  }
  
  if (dn) {
    dout(11) << "lookup '" << path << "' found " << dn->name << " inode " << dn->inode->inode.ino << " valid_until " << dn->inode->valid_until<< endl;
  }

  return dn;
}

// -------

int Client::choose_target_mds(MClientRequest *req) 
{
  int mds = 0;
    
  // find deepest known prefix
  Inode *diri = root;   // the deepest known containing dir
  Inode *item = 0;      // the actual item... if we know it
  int missing_dn = -1;  // which dn we miss on (if we miss)
  
  unsigned depth = req->get_filepath().depth();
  for (unsigned i=0; i<depth; i++) {
    // dir?
    if (diri && diri->inode.mode & INODE_MODE_DIR && diri->dir) {
      Dir *dir = diri->dir;
      
      // do we have the next dentry?
      if (dir->dentries.count( req->get_filepath()[i] ) == 0) {
	missing_dn = i;  // no.
	break;
      }
      
      dout(7) << " have path seg " << i << " on " << diri->dir_auth << " ino " << diri->inode.ino << " " << req->get_filepath()[i] << endl;
      
      if (i == depth-1) {  // last one!
	item = dir->dentries[ req->get_filepath()[i] ]->inode;
	break;
      } 
      
      // continue..
      diri = dir->dentries[ req->get_filepath()[i] ]->inode;
      assert(diri);
    } else {
      missing_dn = i;
      break;
    }
  }
  
  // pick mds
  if (!diri || g_conf.client_use_random_mds) {
    // no root info, pick a random MDS
    mds = mdsmap->get_random_in_mds();
    if (mds < 0) mds = 0;

    if (0) {
      mds = 0;
      dout(0) << "hack: sending all requests to mds" << mds << endl;
    }
  } else {
    if (req->auth_is_best()) {
      // pick the actual auth (as best we can)
      if (item) {
	mds = item->authority(mdsmap);
      } else if (diri->dir_hashed && missing_dn >= 0) {
	mds = diri->dentry_authority(req->get_filepath()[missing_dn].c_str(),
				     mdsmap);
      } else {
	mds = diri->authority(mdsmap);
      }
    } else {
      // balance our traffic!
      if (diri->dir_hashed && missing_dn >= 0) 
	mds = diri->dentry_authority(req->get_filepath()[missing_dn].c_str(),
				     mdsmap);
      else 
	mds = diri->pick_replica(mdsmap);
    }
  }
  dout(20) << "mds is " << mds << endl;
  
  return mds;
}



MClientReply *Client::make_request(MClientRequest *req,
                                   int use_mds)  // this param is purely for debug hacking
{
  // time the call
  utime_t start = g_clock.real_now();
  
  bool nojournal = false;
  int op = req->get_op();
  if (op == MDS_OP_STAT ||
      op == MDS_OP_LSTAT ||
      op == MDS_OP_READDIR ||
      op == MDS_OP_OPEN)
    nojournal = true;


  // -- request --
  // assign a unique tid
  tid_t tid = ++last_tid;
  req->set_tid(tid);

  if (!mds_requests.empty()) 
    req->set_oldest_client_tid(mds_requests.begin()->first);
  else
    req->set_oldest_client_tid(tid); // this one is the oldest.

  // make note
  MetaRequest request(req, tid);
  mds_requests[tid] = &request;

  // encode payload now, in case we have to resend (in case of mds failure)
  req->encode_payload();
  request.request_payload = req->get_payload();

  // note idempotency
  request.idempotent = req->is_idempotent();

  // hack target mds?
  if (use_mds)
    request.resend_mds = use_mds;

  // set up wait cond
  Cond cond;
  request.caller_cond = &cond;
  
  while (1) {
    // choose mds
    int mds;
    // force use of a particular mds?
    if (request.resend_mds >= 0) {
      mds = request.resend_mds;
      request.resend_mds = -1;
      dout(10) << "target resend_mds specified as mds" << mds << endl;
    } else {
      mds = choose_target_mds(req);
      if (mds >= 0) {
	dout(10) << "chose target mds" << mds << " based on hierarchy" << endl;
      } else {
	mds = mdsmap->get_random_in_mds();
	if (mds < 0) mds = 0;  // hrm.
	dout(10) << "chose random target mds" << mds << " for lack of anything better" << endl;
      }
    }
    
    // open a session?
    if (mds_sessions.count(mds) == 0) {
      Cond cond;

      if (!mdsmap->have_inst(mds)) {
	dout(10) << "no address for mds" << mds << ", requesting new mdsmap" << endl;
	int mon = monmap->pick_mon();
	messenger->send_message(new MMDSGetMap(),
				monmap->get_inst(mon));
	waiting_for_mdsmap.push_back(&cond);
	cond.Wait(client_lock);

	if (!mdsmap->have_inst(mds)) {
	  dout(10) << "hmm, still have no address for mds" << mds << ", trying a random mds" << endl;
	  request.resend_mds = mdsmap->get_random_in_mds();
	  continue;
	}
      }	
      
      if (waiting_for_session.count(mds) == 0) {
	dout(10) << "opening session to mds" << mds << endl;
	messenger->send_message(new MClientSession(MClientSession::OP_REQUEST_OPEN),
				mdsmap->get_inst(mds), MDS_PORT_SERVER);
      }
      
      // wait
      waiting_for_session[mds].push_back(&cond);
      while (waiting_for_session.count(mds)) {
	dout(10) << "waiting for session to mds" << mds << " to open" << endl;
	cond.Wait(client_lock);
      }
    }

    // send request.
    send_request(&request, mds);

    // wait for signal
    dout(20) << "awaiting kick on " << &cond << endl;
    cond.Wait(client_lock);
    
    // did we get a reply?
    if (request.reply) 
      break;
  }

  // got it!
  MClientReply *reply = request.reply;
  
  // kick dispatcher (we've got it!)
  assert(request.dispatch_cond);
  request.dispatch_cond->Signal();
  dout(20) << "sendrecv kickback on tid " << tid << " " << request.dispatch_cond << endl;
  
  // clean up.
  mds_requests.erase(tid);


  // -- log times --
  if (client_logger) {
    utime_t lat = g_clock.real_now();
    lat -= start;
    dout(20) << "lat " << lat << endl;
    client_logger->finc("lsum",(double)lat);
    client_logger->inc("lnum");

    if (nojournal) {
      client_logger->finc("lrsum",(double)lat);
      client_logger->inc("lrnum");
    } else {
      client_logger->finc("lwsum",(double)lat);
      client_logger->inc("lwnum");
    }
    
    if (op == MDS_OP_STAT) {
      client_logger->finc("lstatsum",(double)lat);
      client_logger->inc("lstatnum");
    }
    else if (op == MDS_OP_READDIR) {
      client_logger->finc("ldirsum",(double)lat);
      client_logger->inc("ldirnum");
    }

  }

  return reply;
}


void Client::handle_client_session(MClientSession *m) 
{
  dout(10) << "handle_client_session " << *m << endl;
  int from = m->get_source().num();

  switch (m->op) {
  case MClientSession::OP_OPEN:
    assert(mds_sessions.count(from) == 0);
    mds_sessions[from] = 0;
    break;

  case MClientSession::OP_CLOSE:
    mds_sessions.erase(from);
    // FIXME: kick requests (hard) so that they are redirected.  or fail.
    break;

  default:
    assert(0);
  }

  // kick waiting threads
  for (list<Cond*>::iterator p = waiting_for_session[from].begin();
       p != waiting_for_session[from].end();
       ++p)
    (*p)->Signal();
  waiting_for_session.erase(from);

  delete m;
}


void Client::send_request(MetaRequest *request, int mds)
{
  MClientRequest *r = request->request;
  if (!r) {
    // make a new one
    dout(10) << "send_request rebuilding request " << request->tid
	     << " for mds" << mds << endl;
    r = new MClientRequest;
    r->copy_payload(request->request_payload);
    r->decode_payload();
    r->set_retry_attempt(request->retry_attempt);
  }
  request->request = 0;

  dout(10) << "send_request " << *r << " to mds" << mds << endl;
  messenger->send_message(r, mdsmap->get_inst(mds), MDS_PORT_SERVER);
  
  request->mds.insert(mds);
}

void Client::handle_client_request_forward(MClientRequestForward *fwd)
{
  tid_t tid = fwd->get_tid();

  if (mds_requests.count(tid) == 0) {
    dout(10) << "handle_client_request_forward no pending request on tid " << tid << endl;
    delete fwd;
    return;
  }

  MetaRequest *request = mds_requests[tid];
  assert(request);

  // reset retry counter
  request->retry_attempt = 0;

  if (request->idempotent && 
      mds_sessions.count(fwd->get_dest_mds())) {
    // dest mds has a session, and request was forwarded for us.

    // note new mds set.
    if (request->num_fwd < fwd->get_num_fwd()) {
      // there are now exactly two mds's whose failure should trigger a resend
      // of this request.
      request->mds.clear();
      request->mds.insert(fwd->get_source().num());
      request->mds.insert(fwd->get_dest_mds());
      request->num_fwd = fwd->get_num_fwd();
      dout(10) << "handle_client_request tid " << tid
	       << " fwd " << fwd->get_num_fwd() 
	       << " to mds" << fwd->get_dest_mds() 
	       << ", mds set now " << request->mds
	       << endl;
    } else {
      dout(10) << "handle_client_request tid " << tid
	       << " previously forwarded to mds" << fwd->get_dest_mds() 
	       << ", mds still " << request->mds
		 << endl;
    }
  } else {
    // request not forwarded, or dest mds has no session.
    // resend.
    dout(10) << "handle_client_request tid " << tid
	     << " fwd " << fwd->get_num_fwd() 
	     << " to mds" << fwd->get_dest_mds() 
	     << ", non-idempotent, resending to " << fwd->get_dest_mds()
	     << endl;

    request->mds.clear();
    request->num_fwd = fwd->get_num_fwd();
    request->resend_mds = fwd->get_dest_mds();
    request->caller_cond->Signal();
  }

  delete fwd;
}

void Client::handle_client_reply(MClientReply *reply)
{
  tid_t tid = reply->get_tid();

  if (mds_requests.count(tid) == 0) {
    dout(10) << "handle_client_reply no pending request on tid " << tid << endl;
    delete reply;
    return;
  }
  MetaRequest *request = mds_requests[tid];
  assert(request);

  // store reply
  request->reply = reply;

  // wake up waiter
  request->caller_cond->Signal();

  // wake for kick back
  Cond cond;
  request->dispatch_cond = &cond;
  while (mds_requests.count(tid)) {
    dout(20) << "handle_client_reply awaiting kickback on tid " << tid << " " << &cond << endl;
    cond.Wait(client_lock);
  }
}


// ------------------------
// incoming messages

void Client::dispatch(Message *m)
{
  client_lock.Lock();

  switch (m->get_type()) {
    // osd
  case MSG_OSD_OPREPLY:
    objecter->handle_osd_op_reply((MOSDOpReply*)m);
    break;

  case MSG_OSD_MAP:
    objecter->handle_osd_map((class MOSDMap*)m);
    if (!mounted) mount_cond.Signal();
    break;
    
    // mounting and mds sessions
  case MSG_MDS_MAP:
    handle_mds_map((MMDSMap*)m);
    break;
  case MSG_CLIENT_UNMOUNT:
    handle_unmount(m);
    break;
  case MSG_CLIENT_SESSION:
    handle_client_session((MClientSession*)m);
    break;

    // requests
  case MSG_CLIENT_REQUEST_FORWARD:
    handle_client_request_forward((MClientRequestForward*)m);
    break;
  case MSG_CLIENT_REPLY:
    handle_client_reply((MClientReply*)m);
    break;

  case MSG_CLIENT_FILECAPS:
    handle_file_caps((MClientFileCaps*)m);
    break;

  case MSG_STATFS_REPLY:
    handle_statfs_reply((MStatfsReply*)m);
    break;

  default:
    cout << "dispatch doesn't recognize message type " << m->get_type() << endl;
    assert(0);  // fail loudly
    break;
  }

  // unmounting?
  if (unmounting) {
    dout(10) << "unmounting: trim pass, size was " << lru.lru_get_size() 
             << "+" << inode_map.size() << endl;
    trim_cache();
    if (lru.lru_get_size() == 0 && inode_map.empty()) {
      dout(10) << "unmounting: trim pass, cache now empty, waking unmount()" << endl;
      mount_cond.Signal();
    } else {
      dout(10) << "unmounting: trim pass, size still " << lru.lru_get_size() 
               << "+" << inode_map.size() << endl;
      dump_cache();      
    }
  }

  client_lock.Unlock();
}


void Client::handle_mds_map(MMDSMap* m)
{
  int frommds = -1;
  if (m->get_source().is_mds())
    frommds = m->get_source().num();

  if (mdsmap == 0) {
    mdsmap = new MDSMap;

    assert(m->get_source().is_mon());
    whoami = m->get_dest().num();
    dout(1) << "handle_mds_map i am now " << m->get_dest() << endl;
    messenger->reset_myname(m->get_dest());
    
    mount_cond.Signal();  // mount might be waiting for this.
  } 

  dout(1) << "handle_mds_map epoch " << m->get_epoch() << endl;
  epoch_t was = mdsmap->get_epoch();
  mdsmap->decode(m->get_encoded());
  assert(mdsmap->get_epoch() >= was);
  
  // send reconnect?
  if (frommds >= 0 && 
      mdsmap->get_state(frommds) == MDSMap::STATE_RECONNECT) {
    send_reconnect(frommds);
  }

  // kick requests?
  if (frommds >= 0 &&
      mdsmap->get_state(frommds) == MDSMap::STATE_ACTIVE) {
    kick_requests(frommds);
    //failed_mds.erase(from);
  }

  // kick any waiting threads
  list<Cond*> ls;
  ls.swap(waiting_for_mdsmap);
  for (list<Cond*>::iterator p = ls.begin(); p != ls.end(); ++p)
    (*p)->Signal();

  delete m;
}

void Client::send_reconnect(int mds)
{
  dout(10) << "send_reconnect to mds" << mds << endl;

  MClientReconnect *m = new MClientReconnect;

  if (mds_sessions.count(mds)) {
    // i have an open session.
    for (hash_map<inodeno_t, Inode*>::iterator p = inode_map.begin();
	 p != inode_map.end();
	 p++) {
      if (p->second->caps.count(mds)) {
	dout(10) << " caps on " << p->first
		 << " " << cap_string(p->second->caps[mds].caps)
		 << " wants " << cap_string(p->second->file_caps_wanted())
		 << endl;
	p->second->caps[mds].seq = 0;  // reset seq.
	m->add_inode_caps(p->first,    // ino
			  p->second->file_caps_wanted(), // wanted
			  p->second->caps[mds].caps,     // issued
			  p->second->inode.size, p->second->inode.mtime, p->second->inode.atime);
	string path;
	p->second->make_path(path);
	dout(10) << " path on " << p->first << " is " << path << endl;
	m->add_inode_path(p->first, path);
      }
      if (p->second->stale_caps.count(mds)) {
	dout(10) << " clearing stale caps on " << p->first << endl;
	p->second->stale_caps.erase(mds);         // hrm, is this right?
      }
    }

    // reset my cap seq number
    mds_sessions[mds] = 0;
  } else {
    dout(10) << " i had no session with this mds";
    m->closed = true;
  }

  messenger->send_message(m, mdsmap->get_inst(mds), MDS_PORT_SERVER);
}


void Client::kick_requests(int mds)
{
  dout(10) << "kick_requests for mds" << mds << endl;

  for (map<tid_t, MetaRequest*>::iterator p = mds_requests.begin();
       p != mds_requests.end();
       ++p) 
    if (p->second->mds.count(mds)) {
      p->second->retry_attempt++;   // inc retry counter
      send_request(p->second, mds);
    }
}


/****
 * caps
 */


class C_Client_ImplementedCaps : public Context {
  Client *client;
  MClientFileCaps *msg;
  Inode *in;
public:
  C_Client_ImplementedCaps(Client *c, MClientFileCaps *m, Inode *i) : client(c), msg(m), in(i) {}
  void finish(int r) {
    client->implemented_caps(msg,in);
  }
};

/** handle_file_caps
 * handle caps update from mds.  including mds to mds caps transitions.
 * do not block.
 */
void Client::handle_file_caps(MClientFileCaps *m)
{
  int mds = m->get_source().num();
  Inode *in = 0;
  if (inode_map.count(m->get_ino())) in = inode_map[ m->get_ino() ];

  m->clear_payload();  // for if/when we send back to MDS

  // note push seq increment
  assert(mds_sessions.count(mds));
  mds_sessions[mds]++;

  // reap?
  if (m->get_op() == MClientFileCaps::OP_REAP) {
    int other = m->get_mds();

    if (in && in->stale_caps.count(other)) {
      dout(5) << "handle_file_caps on ino " << m->get_ino() << " from mds" << mds << " reap on mds" << other << endl;

      // fresh from new mds?
      if (!in->caps.count(mds)) {
        if (in->caps.empty()) in->get();
        in->caps[mds].seq = m->get_seq();
        in->caps[mds].caps = m->get_caps();
      }
      
      assert(in->stale_caps.count(other));
      in->stale_caps.erase(other);
      if (in->stale_caps.empty()) put_inode(in); // note: this will never delete *in
      
      // fall-thru!
    } else {
      dout(5) << "handle_file_caps on ino " << m->get_ino() << " from mds" << mds << " premature (!!) reap on mds" << other << endl;
      // delay!
      cap_reap_queue[in->ino()][other] = m;
      return;
    }
  }

  assert(in);
  
  // stale?
  if (m->get_op() == MClientFileCaps::OP_STALE) {
    dout(5) << "handle_file_caps on ino " << m->get_ino() << " seq " << m->get_seq() << " from mds" << mds << " now stale" << endl;
    
    // move to stale list
    assert(in->caps.count(mds));
    if (in->stale_caps.empty()) in->get();
    in->stale_caps[mds] = in->caps[mds];

    assert(in->caps.count(mds));
    in->caps.erase(mds);
    if (in->caps.empty()) in->put();

    // delayed reap?
    if (cap_reap_queue.count(in->ino()) &&
        cap_reap_queue[in->ino()].count(mds)) {
      dout(5) << "handle_file_caps on ino " << m->get_ino() << " from mds" << mds << " delayed reap on mds" << m->get_mds() << endl;
      
      // process delayed reap
      handle_file_caps( cap_reap_queue[in->ino()][mds] );

      cap_reap_queue[in->ino()].erase(mds);
      if (cap_reap_queue[in->ino()].empty())
        cap_reap_queue.erase(in->ino());
    }
    delete m;
    return;
  }

  // release?
  if (m->get_op() == MClientFileCaps::OP_RELEASE) {
    dout(5) << "handle_file_caps on ino " << m->get_ino() << " from mds" << mds << " release" << endl;
    assert(in->caps.count(mds));
    in->caps.erase(mds);
    for (map<int,InodeCap>::iterator p = in->caps.begin();
         p != in->caps.end();
         p++)
      dout(20) << " left cap " << p->first << " " 
              << cap_string(p->second.caps) << " " 
              << p->second.seq << endl;
    for (map<int,InodeCap>::iterator p = in->stale_caps.begin();
         p != in->stale_caps.end();
         p++)
      dout(20) << " left stale cap " << p->first << " " 
              << cap_string(p->second.caps) << " " 
              << p->second.seq << endl;

    if (in->caps.empty()) {
      //dout(0) << "did put_inode" << endl;
      put_inode(in);
    } else {
      //dout(0) << "didn't put_inode" << endl;
    }
    delete m;
    return;
  }


  // don't want?
  if (in->file_caps_wanted() == 0) {
    dout(5) << "handle_file_caps on ino " << m->get_ino() 
            << " seq " << m->get_seq() 
            << " " << cap_string(m->get_caps()) 
            << ", which we don't want caps for, releasing." << endl;
    m->set_caps(0);
    m->set_wanted(0);
    messenger->send_message(m, m->get_source_inst(), MDS_PORT_LOCKER);
    return;
  }

  assert(in->caps.count(mds));

  // update per-mds caps
  const int old_caps = in->caps[mds].caps;
  const int new_caps = m->get_caps();
  in->caps[mds].caps = new_caps;
  in->caps[mds].seq = m->get_seq();
  dout(5) << "handle_file_caps on in " << m->get_ino() 
          << " mds" << mds << " seq " << m->get_seq() 
          << " caps now " << cap_string(new_caps) 
          << " was " << cap_string(old_caps) << endl;
  
  // did file size decrease?
  if ((old_caps & (CAP_FILE_RD|CAP_FILE_WR)) == 0 &&
      (new_caps & (CAP_FILE_RD|CAP_FILE_WR)) != 0 &&
      in->inode.size > m->get_inode().size) {
    dout(10) << "*** file size decreased from " << in->inode.size << " to " << m->get_inode().size << endl;
    
    // trim filecache?
    if (g_conf.client_oc)
      in->fc.truncate(in->inode.size, m->get_inode().size);

    in->inode.size = in->file_wr_size = m->get_inode().size; 
  }

  // update inode
  in->inode = m->get_inode();      // might have updated size... FIXME this is overkill!

  // preserve our (possibly newer) file size, mtime
  if (in->file_wr_size > in->inode.size)
    m->get_inode().size = in->inode.size = in->file_wr_size;
  if (in->file_wr_mtime > in->inode.mtime)
    m->get_inode().mtime = in->inode.mtime = in->file_wr_mtime;



  if (g_conf.client_oc) {
    // caching on, use FileCache.
    Context *onimplement = 0;
    if (old_caps & ~new_caps) {     // this mds is revoking caps
      if (in->fc.get_caps() & ~(in->file_caps()))   // net revocation
        onimplement = new C_Client_ImplementedCaps(this, m, in);
      else {
        implemented_caps(m, in);        // ack now.
      }
    }
    in->fc.set_caps(new_caps, onimplement);
  } else {
    // caching off.

    // wake up waiters?
    if (new_caps & CAP_FILE_RD) {
      for (list<Cond*>::iterator it = in->waitfor_read.begin();
           it != in->waitfor_read.end();
           it++) {
        dout(5) << "signaling read waiter " << *it << endl;
        (*it)->Signal();
      }
      in->waitfor_read.clear();
    }
    if (new_caps & CAP_FILE_WR) {
      for (list<Cond*>::iterator it = in->waitfor_write.begin();
           it != in->waitfor_write.end();
           it++) {
        dout(5) << "signaling write waiter " << *it << endl;
        (*it)->Signal();
      }
      in->waitfor_write.clear();
    }
    if (new_caps & CAP_FILE_LAZYIO) {
      for (list<Cond*>::iterator it = in->waitfor_lazy.begin();
           it != in->waitfor_lazy.end();
           it++) {
        dout(5) << "signaling lazy waiter " << *it << endl;
        (*it)->Signal();
      }
      in->waitfor_lazy.clear();
    }

    // ack?
    if (old_caps & ~new_caps) {
      if (in->sync_writes) {
        // wait for sync writes to finish
        dout(5) << "sync writes in progress, will ack on finish" << endl;
        in->waitfor_no_write.push_back(new C_Client_ImplementedCaps(this, m, in));
      } else {
        // ok now
        implemented_caps(m, in);
      }
    } else {
      // discard
      delete m;
    }
  }
}

void Client::implemented_caps(MClientFileCaps *m, Inode *in)
{
  dout(5) << "implemented_caps " << cap_string(m->get_caps()) 
          << ", acking to " << m->get_source() << endl;

  if (in->file_caps() == 0) {
    in->file_wr_mtime = utime_t();
    in->file_wr_size = 0;
  }

  messenger->send_message(m, m->get_source_inst(), MDS_PORT_LOCKER);
}


void Client::release_caps(Inode *in,
                          int retain)
{
  dout(5) << "releasing caps on ino " << in->inode.ino << dec
          << " had " << cap_string(in->file_caps())
          << " retaining " << cap_string(retain) 
	  << " want " << cap_string(in->file_caps_wanted())
          << endl;
  
  for (map<int,InodeCap>::iterator it = in->caps.begin();
       it != in->caps.end();
       it++) {
    //if (it->second.caps & ~retain) {
    if (1) {
      // release (some of?) these caps
      it->second.caps = retain & it->second.caps;
      // note: tell mds _full_ wanted; it'll filter/behave based on what it is allowed to do
      MClientFileCaps *m = new MClientFileCaps(MClientFileCaps::OP_ACK,
					       in->inode, 
                                               it->second.seq,
                                               it->second.caps,
                                               in->file_caps_wanted()); 
      messenger->send_message(m, mdsmap->get_inst(it->first), MDS_PORT_LOCKER);
    }
  }
  
  if (in->file_caps() == 0) {
    in->file_wr_mtime = utime_t();
    in->file_wr_size = 0;
  }
}

void Client::update_caps_wanted(Inode *in)
{
  dout(5) << "updating caps wanted on ino " << in->inode.ino 
          << " to " << cap_string(in->file_caps_wanted())
          << endl;
  
  // FIXME: pick a single mds and let the others off the hook..
  for (map<int,InodeCap>::iterator it = in->caps.begin();
       it != in->caps.end();
       it++) {
    MClientFileCaps *m = new MClientFileCaps(MClientFileCaps::OP_ACK,
					     in->inode, 
                                             it->second.seq,
                                             it->second.caps,
                                             in->file_caps_wanted());
    messenger->send_message(m,
                            mdsmap->get_inst(it->first), MDS_PORT_LOCKER);
  }
}



// -------------------
// MOUNT

void Client::_try_mount()
{
  dout(10) << "_try_mount" << endl;
  int mon = monmap->pick_mon();
  dout(2) << "sending client_mount to mon" << mon << endl;
  messenger->send_message(new MClientMount(messenger->get_myaddr(),
					   client_instance_this_process), 
			  monmap->get_inst(mon));

  // schedule timeout
  assert(mount_timeout_event == 0);
  mount_timeout_event = new C_MountTimeout(this);
  timer.add_event_after(g_conf.client_mount_timeout, mount_timeout_event);
}

void Client::_mount_timeout()
{
  dout(10) << "_mount_timeout" << endl;
  mount_timeout_event = 0;
  _try_mount();
}

int Client::mount()
{
  client_lock.Lock();
  assert(!mounted);  // caller is confused?
  assert(!mdsmap);

  _try_mount();
  
  while (!mdsmap ||
	 !osdmap || 
	 osdmap->get_epoch() == 0)
    mount_cond.Wait(client_lock);

  timer.cancel_event(mount_timeout_event);
  mount_timeout_event = 0;
  
  mounted = true;

  dout(2) << "mounted: have osdmap " << osdmap->get_epoch() 
	  << " and mdsmap " << mdsmap->get_epoch() 
	  << endl;

  /*
  // hack: get+pin root inode
  Inode *root;
  _do_lstat("/", STAT_MASK_ALL, &root);
  _ll_get(root);
  */

  // trace?
  if (g_conf.client_trace) {
    traceout.open(g_conf.client_trace);
    if (traceout.is_open()) {
      dout(1) << "opened trace file '" << g_conf.client_trace << "'" << endl;
    } else {
      dout(1) << "FAILED to open trace file '" << g_conf.client_trace << "'" << endl;
    }
  }

  client_lock.Unlock();

  /*
  dout(3) << "op: // client trace data structs" << endl;
  dout(3) << "op: struct stat st;" << endl;
  dout(3) << "op: struct utimbuf utim;" << endl;
  dout(3) << "op: int readlinkbuf_len = 1000;" << endl;
  dout(3) << "op: char readlinkbuf[readlinkbuf_len];" << endl;
  dout(3) << "op: map<string, inode_t*> dir_contents;" << endl;
  dout(3) << "op: map<int, int> open_files;" << endl;
  dout(3) << "op: int fd;" << endl;
  */
  return 0;
}


// UNMOUNT


int Client::unmount()
{
  client_lock.Lock();

  assert(mounted);  // caller is confused?

  dout(2) << "unmounting" << endl;
  unmounting = true;

  // NOTE: i'm assuming all caches are already flushing (because all files are closed).
  assert(fd_map.empty());

  dout(10) << "a" << endl;

  _ll_drop_pins();
  
  dout(10) << "b" << endl;

  // empty lru cache
  lru.lru_set_max(0);
  trim_cache();

  if (g_conf.client_oc) {
    // release any/all caps
    for (hash_map<inodeno_t, Inode*>::iterator p = inode_map.begin();
         p != inode_map.end();
         p++) {
      Inode *in = p->second;
      if (!in->caps.empty()) {
        in->fc.release_clean();
        if (in->fc.is_dirty()) {
          dout(10) << "unmount residual caps on " << in->ino() << ", flushing" << endl;
          in->fc.empty(new C_Client_CloseRelease(this, in));
        } else {
          dout(10) << "unmount residual caps on " << in->ino()  << ", releasing" << endl;
          release_caps(in);
        }
      }
    }
  }

  while (lru.lru_get_size() > 0 || 
         !inode_map.empty()) {
    dout(2) << "cache still has " << lru.lru_get_size() 
            << "+" << inode_map.size() << " items" 
	    << ", waiting (for caps to release?)"
            << endl;
    dump_cache();
    mount_cond.Wait(client_lock);
  }
  assert(lru.lru_get_size() == 0);
  assert(inode_map.empty());

  // unsafe writes
  if (!g_conf.client_oc) {
    while (unsafe_sync_write > 0) {
      dout(0) << unsafe_sync_write << " unsafe_sync_writes, waiting" 
              << endl;
      mount_cond.Wait(client_lock);
    }
  }

  // stop tracing
  if (g_conf.client_trace) {
    dout(1) << "closing trace file '" << g_conf.client_trace << "'" << endl;
    traceout.close();
  }

  
  // send session closes!
  for (map<int,version_t>::iterator p = mds_sessions.begin();
       p != mds_sessions.end();
       ++p) {
    dout(2) << "sending client_session close to mds" << p->first << " seq " << p->second << endl;
    messenger->send_message(new MClientSession(MClientSession::OP_REQUEST_CLOSE,
					       p->second),
			    mdsmap->get_inst(p->first), MDS_PORT_SERVER);
  }

  // send unmount!
  int mon = monmap->pick_mon();
  dout(2) << "sending client_unmount to mon" << mon << endl;
  messenger->send_message(new MClientUnmount(messenger->get_myinst()), 
			  monmap->get_inst(mon));
  
  while (mounted)
    mount_cond.Wait(client_lock);

  dout(2) << "unmounted." << endl;

  client_lock.Unlock();
  return 0;
}

void Client::handle_unmount(Message* m)
{
  dout(1) << "handle_unmount got ack" << endl;

  mounted = false;

  delete mdsmap;
  mdsmap = 0;

  mount_cond.Signal();

  delete m;
}


// ===============================================================
// high level (POSIXy) interface


// namespace ops

int Client::link(const char *existing, const char *newname) 
{
  Mutex::Locker lock(client_lock);
  tout << "link" << endl;
  tout << existing << endl;
  tout << newname << endl;
  return _link(existing, newname);
}

int Client::_link(const char *existing, const char *newname) 
{
  // main path arg is new link name
  // sarg is target (existing file)

  MClientRequest *req = new MClientRequest(MDS_OP_LINK, messenger->get_myinst());
  req->set_path(newname);
  req->set_sarg(existing);
  
  // FIXME where does FUSE maintain user information
  req->set_caller_uid(getuid());
  req->set_caller_gid(getgid());
  
  MClientReply *reply = make_request(req);
  int res = reply->get_result();
  
  insert_trace(reply);
  delete reply;
  dout(10) << "link result is " << res << endl;

  trim_cache();
  dout(3) << "link(\"" << existing << "\", \"" << newname << "\") = " << res << endl;
  return res;
}


int Client::unlink(const char *relpath)
{
  Mutex::Locker lock(client_lock);
  tout << "unlink" << endl;
  tout << relpath << endl;

  string abspath;
  mkabspath(relpath, abspath);
  return _unlink(abspath.c_str());
}

int Client::_unlink(const char *path)
{

  MClientRequest *req = new MClientRequest(MDS_OP_UNLINK, messenger->get_myinst());
  req->set_path(path);
 
  // FIXME where does FUSE maintain user information
  req->set_caller_uid(getuid());
  req->set_caller_gid(getgid());

  //FIXME enforce caller uid rights?
   
  MClientReply *reply = make_request(req);
  int res = reply->get_result();
  if (res == 0) {
    // remove from local cache
    filepath fp(path);
    Dentry *dn = lookup(fp);
    if (dn) {
      assert(dn->inode);
      unlink(dn);
    }
  }
  insert_trace(reply);
  delete reply;
  dout(10) << "unlink result is " << res << endl;

  trim_cache();
  dout(3) << "unlink(\"" << path << "\") = " << res << endl;
  return res;
}

int Client::rename(const char *relfrom, const char *relto)
{
  Mutex::Locker lock(client_lock);
  tout << "rename" << endl;
  tout << relfrom << endl;
  tout << relto << endl;

  string absfrom, absto;
  mkabspath(relfrom, absfrom);
  mkabspath(relto, absto);
  return _rename(absfrom.c_str(), absto.c_str());
}

int Client::_rename(const char *from, const char *to)
{
  MClientRequest *req = new MClientRequest(MDS_OP_RENAME, messenger->get_myinst());
  req->set_path(from);
  req->set_sarg(to);
 
  // FIXME where does FUSE maintain user information
  req->set_caller_uid(getuid());
  req->set_caller_gid(getgid());

  //FIXME enforce caller uid rights?
   
  MClientReply *reply = make_request(req);
  int res = reply->get_result();
  insert_trace(reply);
  delete reply;
  dout(10) << "rename result is " << res << endl;

  // renamed item from our cache

  trim_cache();
  dout(3) << "rename(\"" << from << "\", \"" << to << "\") = " << res << endl;
  return res;
}

// dirs

int Client::mkdir(const char *relpath, mode_t mode)
{
  Mutex::Locker lock(client_lock);
  tout << "mkdir" << endl;
  tout << relpath << endl;
  tout << mode << endl;

  string abspath;
  mkabspath(relpath, abspath);
  return _mkdir(abspath.c_str(), mode);
}

int Client::_mkdir(const char *path, mode_t mode)
{
  MClientRequest *req = new MClientRequest(MDS_OP_MKDIR, messenger->get_myinst());
  req->set_path(path);
  req->args.mkdir.mode = mode;
 
  // FIXME where does FUSE maintain user information
  req->set_caller_uid(getuid());
  req->set_caller_gid(getgid());

  //FIXME enforce caller uid rights?
   
  MClientReply *reply = make_request(req);
  int res = reply->get_result();
  insert_trace(reply);
  delete reply;
  dout(10) << "mkdir result is " << res << endl;

  trim_cache();

  dout(3) << "mkdir(\"" << path << "\", 0" << oct << mode << dec << ") = " << res << endl;
  return res;
}

int Client::rmdir(const char *relpath)
{
  Mutex::Locker lock(client_lock);
  tout << "rmdir" << endl;
  tout << relpath << endl;

  string abspath;
  mkabspath(relpath, abspath);
  return _rmdir(abspath.c_str());
}

int Client::_rmdir(const char *path)
{
  MClientRequest *req = new MClientRequest(MDS_OP_RMDIR, messenger->get_myinst());
  req->set_path(path);
 
  // FIXME where does FUSE maintain user information
  req->set_caller_uid(getuid());
  req->set_caller_gid(getgid());

  //FIXME enforce caller uid rights?
   
  MClientReply *reply = make_request(req);
  int res = reply->get_result();
  if (res == 0) {
    // remove from local cache
    filepath fp(path);
    Dentry *dn = lookup(fp);
    if (dn) {
      if (dn->inode->dir && dn->inode->dir->is_empty()) 
        close_dir(dn->inode->dir);  // FIXME: maybe i shoudl proactively hose the whole subtree from cache?
      unlink(dn);
    }
  }
  insert_trace(reply);  
  delete reply;

  trim_cache();
  dout(3) << "rmdir(\"" << path << "\") = " << res << endl;
  return res;
}

// symlinks
  
int Client::symlink(const char *reltarget, const char *rellink)
{
  Mutex::Locker lock(client_lock);
  tout << "symlink" << endl;
  tout << reltarget << endl;
  tout << rellink << endl;

  string target, link;
  mkabspath(reltarget, target);
  mkabspath(rellink, link);
  return _symlink(target.c_str(), link.c_str());
}

int Client::_symlink(const char *target, const char *link)
{
  MClientRequest *req = new MClientRequest(MDS_OP_SYMLINK, messenger->get_myinst());
  req->set_path(link);
  req->set_sarg(target);
 
  // FIXME where does FUSE maintain user information
  req->set_caller_uid(getuid());
  req->set_caller_gid(getgid());

  //FIXME enforce caller uid rights?
   
  MClientReply *reply = make_request(req);
  int res = reply->get_result();
  insert_trace(reply);  //FIXME assuming trace of link, not of target
  delete reply;

  trim_cache();
  dout(3) << "symlink(\"" << target << "\", \"" << link << "\") = " << res << endl;
  return res;
}

int Client::readlink(const char *path, char *buf, off_t size) 
{
  Mutex::Locker lock(client_lock);
  tout << "readlink" << endl;
  tout << path << endl;

  string abspath;
  mkabspath(path, abspath);
  return _readlink(abspath.c_str(), buf, size);
}

int Client::_readlink(const char *path, char *buf, off_t size) 
{ 
  Inode *in;
  int r = _do_lstat(path, STAT_MASK_BASE, &in);
  if (r == 0 && !in->inode.is_symlink()) r = -EINVAL;
  if (r == 0) {
    // copy into buf (at most size bytes)
    r = in->symlink->length();
    if (r > size) r = size;
    memcpy(buf, in->symlink->c_str(), r);
  } else {
    buf[0] = 0;
  }
  trim_cache();

  dout(3) << "readlink(\"" << path << "\", \"" << buf << "\", " << size << ") = " << r << endl;
  return r;
}



// inode stuff

int Client::_do_lstat(const char *path, int mask, Inode **in)
{  
  MClientRequest *req = 0;
  filepath fpath(path);
  
  // check whether cache content is fresh enough
  int res = 0;

  Dentry *dn = lookup(fpath);
  inode_t inode;
  utime_t now = g_clock.real_now();

  if (dn && 
      now <= dn->inode->valid_until)
    dout(10) << "_lstat has inode " << path << " with mask " << dn->inode->mask << ", want " << mask << endl;

  if (dn && dn->inode &&
      ((mask & ~STAT_MASK_BASE) || now <= dn->inode->valid_until) &&
      ((dn->inode->mask & mask) == mask)) {
    inode = dn->inode->inode;
    dout(10) << "lstat cache hit w/ sufficient mask, valid until " << dn->inode->valid_until << endl;
    
    if (g_conf.client_cache_stat_ttl == 0)
      dn->inode->valid_until = utime_t();           // only one stat allowed after each readdir

    *in = dn->inode;
  } else {  
    // FIXME where does FUSE maintain user information
    //struct fuse_context *fc = fuse_get_context();
    //req->set_caller_uid(fc->uid);
    //req->set_caller_gid(fc->gid);
    
    req = new MClientRequest(MDS_OP_LSTAT, messenger->get_myinst());
    req->args.stat.mask = mask;
    req->set_path(fpath);

    MClientReply *reply = make_request(req);
    res = reply->get_result();
    dout(10) << "lstat res is " << res << endl;
    if (res == 0) {
      //Transfer information from reply to stbuf
      inode = reply->get_inode();
      
      //Update metadata cache
      *in = insert_trace(reply);
    }

    delete reply;

    if (res != 0) 
      *in = 0;     // not a success.
  }
     
  return res;
}


int Client::fill_stat(Inode *in, struct stat *st) 
{
  dout(10) << "fill_stat on " << in->inode.ino << " mode 0" << oct << in->inode.mode << dec
	   << " mtime " << in->inode.mtime << " ctime " << in->inode.ctime << endl;
  memset(st, 0, sizeof(struct stat));
  st->st_ino = in->inode.ino;
  st->st_mode = in->inode.mode;
  st->st_rdev = in->inode.rdev;
  st->st_nlink = in->inode.nlink;
  st->st_uid = in->inode.uid;
  st->st_gid = in->inode.gid;
  st->st_ctime = MAX(in->inode.ctime, in->inode.mtime);
  st->st_atime = in->inode.atime;
  st->st_mtime = in->inode.mtime;
  st->st_size = in->inode.size;
  st->st_blocks = in->inode.size ? ((in->inode.size - 1) / 4096 + 1):0;
  st->st_blksize = 4096;
  return in->mask;
}

  /*
  S_REQUIREBLKSIZE(st->st_litemask);
  if (inode.mask & INODE_MASK_BASE) S_REQUIRECTIME(st->st_litemask);
  if (inode.mask & INODE_MASK_SIZE) {
    S_REQUIRESIZE(st->st_litemask);
    S_REQUIREBLOCKS(st->st_litemask);
  }
  if (inode.mask & INODE_MASK_MTIME) S_REQUIREMTIME(st->st_litemask);
  if (inode.mask & INODE_MASK_ATIME) S_REQUIREATIME(st->st_litemask);
  */


int Client::lstat(const char *relpath, struct stat *stbuf)
{
  Mutex::Locker lock(client_lock);
  tout << "lstat" << endl;
  tout << relpath << endl;

  string abspath;
  mkabspath(relpath, abspath);
  return _lstat(abspath.c_str(), stbuf);
}

int Client::_lstat(const char *path, struct stat *stbuf)
{
  Inode *in = 0;
  int res = _do_lstat(path, STAT_MASK_ALL, &in);
  if (res == 0) {
    assert(in);
    fill_stat(in, stbuf);
    dout(10) << "stat sez size = " << in->inode.size << " mode = 0" << oct << stbuf->st_mode << dec << " ino = " << stbuf->st_ino << endl;
  }

  trim_cache();
  dout(3) << "lstat(\"" << path << "\", " << stbuf << ") = " << res << endl;
  return res;
}


/*
int Client::lstatlite(const char *relpath, struct statlite *stl)
{
  client_lock.Lock();
   
  string abspath;
  mkabspath(relpath, abspath);
  const char *path = abspath.c_str();

  dout(3) << "op: client->lstatlite(\"" << path << "\", &st);" << endl;
  tout << "lstatlite" << endl;
  tout << path << endl;

  // make mask
  // FIXME.
  int mask = INODE_MASK_BASE | INODE_MASK_AUTH;
  if (S_ISVALIDSIZE(stl->st_litemask) || 
      S_ISVALIDBLOCKS(stl->st_litemask)) mask |= INODE_MASK_SIZE;
  if (S_ISVALIDMTIME(stl->st_litemask)) mask |= INODE_MASK_FILE;
  if (S_ISVALIDATIME(stl->st_litemask)) mask |= INODE_MASK_FILE;
  
  Inode *in = 0;
  int res = _lstat(path, mask, &in);
  
  if (res == 0) {
    fill_statlite(in->inode,stl);
    dout(10) << "stat sez size = " << in->inode.size << " ino = " << in->inode.ino << endl;
  }

  trim_cache();
  client_lock.Unlock();
  return res;
}
*/


int Client::chmod(const char *relpath, mode_t mode)
{
  Mutex::Locker lock(client_lock);
  tout << "chmod" << endl;
  tout << relpath << endl;
  tout << mode << endl;

  string abspath;
  mkabspath(relpath, abspath);
  return _chmod(abspath.c_str(), mode);
}

int Client::_chmod(const char *path, mode_t mode) 
{
  dout(3) << "_chmod(" << path << ", 0" << oct << mode << dec << ")" << endl;
  MClientRequest *req = new MClientRequest(MDS_OP_CHMOD, messenger->get_myinst());
  req->set_path(path); 
  req->args.chmod.mode = mode;

  // FIXME where does FUSE maintain user information
  req->set_caller_uid(getuid());
  req->set_caller_gid(getgid());
  
  MClientReply *reply = make_request(req);
  int res = reply->get_result();
  insert_trace(reply);  
  delete reply;

  trim_cache();
  dout(3) << "_chmod(\"" << path << "\", 0" << oct << mode << dec << ") = " << res << endl;
  return res;
}

int Client::chown(const char *relpath, uid_t uid, gid_t gid)
{
  Mutex::Locker lock(client_lock);
  tout << "chown" << endl;
  tout << relpath << endl;
  tout << uid << endl;
  tout << gid << endl;

  string abspath;
  mkabspath(relpath, abspath);
  return _chown(abspath.c_str(), uid, gid);
}

int Client::_chown(const char *path, uid_t uid, gid_t gid)
{
  dout(3) << "_chown(" << path << ", " << uid << ", " << gid << ")" << endl;
  MClientRequest *req = new MClientRequest(MDS_OP_CHOWN, messenger->get_myinst());
  req->set_path(path); 
  req->args.chown.uid = uid;
  req->args.chown.gid = gid;

  // FIXME where does FUSE maintain user information
  req->set_caller_uid(getuid());
  req->set_caller_gid(getgid());

  //FIXME enforce caller uid rights?

  MClientReply *reply = make_request(req);
  int res = reply->get_result();
  insert_trace(reply);  
  delete reply;
  dout(10) << "chown result is " << res << endl;

  trim_cache();
  dout(3) << "chown(\"" << path << "\", " << uid << ", " << gid << ") = " << res << endl;
  return res;
}

int Client::utime(const char *relpath, struct utimbuf *buf)
{
  Mutex::Locker lock(client_lock);
  tout << "utime" << endl;
  tout << relpath << endl;
  tout << buf->modtime << endl;
  tout << buf->actime << endl;

  string abspath;
  mkabspath(relpath, abspath);
  return _utimes(abspath.c_str(), utime_t(buf->modtime,0), utime_t(buf->actime,0));
}

int Client::_utimes(const char *path, utime_t mtime, utime_t atime)
{
  dout(3) << "_utimes(" << path << ", " << mtime << ", " << atime << ")" << endl;
  MClientRequest *req = new MClientRequest(MDS_OP_UTIME, messenger->get_myinst());
  req->set_path(path); 
  req->args.utime.mtime = mtime.tv_ref();
  req->args.utime.atime = atime.tv_ref();

  // FIXME where does FUSE maintain user information
  req->set_caller_uid(getuid());
  req->set_caller_gid(getgid());

  MClientReply *reply = make_request(req);
  int res = reply->get_result();
  insert_trace(reply);  
  delete reply;

  dout(3) << "utimes(\"" << path << "\", " << mtime << ", " << atime << ") = " << res << endl;
  trim_cache();
  return res;
}



int Client::mknod(const char *relpath, mode_t mode, dev_t rdev) 
{ 
  Mutex::Locker lock(client_lock);
  tout << "mknod" << endl;
  tout << relpath << endl;
  tout << mode << endl;
  tout << rdev << endl;

  string abspath;
  mkabspath(relpath, abspath);
  return _mknod(abspath.c_str(), mode, rdev);
}

int Client::_mknod(const char *path, mode_t mode, dev_t rdev) 
{ 
  dout(3) << "_mknod(" << path << ", 0" << oct << mode << dec << ", " << rdev << ")" << endl;

  MClientRequest *req = new MClientRequest(MDS_OP_MKNOD, messenger->get_myinst());
  req->set_path(path); 
  req->args.mknod.mode = mode;
  req->args.mknod.rdev = rdev;

  // FIXME where does FUSE maintain user information
  req->set_caller_uid(getuid());
  req->set_caller_gid(getgid());

  //FIXME enforce caller uid rights?
   
  MClientReply *reply = make_request(req);
  int res = reply->get_result();
  insert_trace(reply);  

  delete reply;

  trim_cache();

  dout(3) << "mknod(\"" << path << "\", 0" << oct << mode << dec << ") = " << res << endl;
  return res;
}



  
int Client::getdir(const char *relpath, list<string>& contents)
{
  dout(3) << "getdir(" << relpath << ")" << endl;
  {
    Mutex::Locker lock(client_lock);
    tout << "getdir" << endl;
    tout << relpath << endl;
  }

  DIR *d;
  int r = opendir(relpath, &d);
  if (r < 0) return r;

  struct dirent de;
  int n = 0;
  while (readdir_r(d, &de) == 0) {
    contents.push_back(de.d_name);
    n++;
  }
  closedir(d);

  return n;
}

int Client::opendir(const char *name, DIR **dirpp) 
{
  Mutex::Locker lock(client_lock);
  tout << "opendir" << endl;
  tout << name << endl;

  int r = _opendir(name, (DirResult**)dirpp);
  tout << (unsigned long)*dirpp;
  return r;
}

int Client::_opendir(const char *name, DirResult **dirpp) 
{
  *dirpp = new DirResult(name);

  // do we have the inode in our cache?  
  // if so, should be we ask for a different dirfrag?
  filepath path(name);
  Dentry *dn = lookup(path);
  if (dn && dn->inode) {
    (*dirpp)->inode = dn->inode;
    (*dirpp)->inode->get();
    dout(10) << "had inode " << dn->inode << " " << dn->inode->inode.ino << " ref now " << dn->inode->ref << endl;
    (*dirpp)->set_frag(dn->inode->dirfragtree[0]);
    dout(10) << "_opendir " << name << ", our cache says the first dirfrag is " << (*dirpp)->frag() << endl;
  }

  // get the first frag
  int r = _readdir_get_frag(*dirpp);
  if (r < 0) {
    _closedir(*dirpp);
    *dirpp = 0;
  } else {
    r = 0;
  }
  dout(3) << "_opendir(" << name << ") = " << r << " (" << *dirpp << ")" << endl;

  return r;
}

void Client::_readdir_add_dirent(DirResult *dirp, const string& name, Inode *in)
{
  struct stat st;
  int stmask = fill_stat(in, &st);  
  frag_t fg = dirp->frag();
  dirp->buffer[fg].push_back(DirEntry(name, st, stmask));
  dout(10) << "_readdir_add_dirent " << dirp << " added '" << name << "' -> " << in->inode.ino
	   << ", size now " << dirp->buffer[fg].size() << endl;
}

//struct dirent {
//  ino_t          d_ino;       /* inode number */
//  off_t          d_off;       /* offset to the next dirent */
//  unsigned short d_reclen;    /* length of this record */
//  unsigned char  d_type;      /* type of file */
//  char           d_name[256]; /* filename */
//};
void Client::_readdir_fill_dirent(struct dirent *de, DirEntry *entry, off_t off)
{
  de->d_ino = entry->st.st_ino;
  de->d_off = off + 1;
  de->d_reclen = 1;
  de->d_type = MODE_TO_DT(entry->st.st_mode);
  strncpy(de->d_name, entry->d_name.c_str(), 256);
  dout(10) << "_readdir_fill_dirent '" << de->d_name << "' -> " << de->d_ino
	   << " type " << (int)de->d_type << " at off " << off << endl;
}

void Client::_readdir_next_frag(DirResult *dirp)
{
  frag_t fg = dirp->frag();

  // hose old data
  assert(dirp->buffer.count(fg));
  dirp->buffer.erase(fg);

  // advance
  dirp->next_frag();
  if (dirp->at_end()) {
    dout(10) << "_readdir_next_frag advance from " << fg << " to END" << endl;
  } else {
    dout(10) << "_readdir_next_frag advance from " << fg << " to " << dirp->frag() << endl;
    _readdir_rechoose_frag(dirp);
  }
}

void Client::_readdir_rechoose_frag(DirResult *dirp)
{
  assert(dirp->inode);
  frag_t cur = dirp->frag();
  frag_t f = dirp->inode->dirfragtree[cur.value()];
  if (f != cur) {
    dout(10) << "_readdir_rechoose_frag frag " << cur << " maps to " << f << endl;
    dirp->set_frag(f);
  }
}

int Client::_readdir_get_frag(DirResult *dirp)
{
  // get the current frag.
  frag_t fg = dirp->frag();
  assert(dirp->buffer.count(fg) == 0);
  
  dout(10) << "_readdir_get_frag " << dirp << " on " << dirp->path << " fg " << fg << endl;

  MClientRequest *req = new MClientRequest(MDS_OP_READDIR, messenger->get_myinst());
  req->set_path(dirp->path); 
  req->args.readdir.frag = fg;
  
  // FIXME where does FUSE maintain user information
  req->set_caller_uid(getuid());
  req->set_caller_gid(getgid());
  
  MClientReply *reply = make_request(req);
  int res = reply->get_result();
  insert_trace(reply);  
  inodeno_t ino = reply->get_ino();
  
  // did i get directory inode?
  Inode *diri = 0;
  if ((res == -EAGAIN || res == 0) &&
      inode_map.count(ino)) {
    diri = inode_map[ino];
    dout(10) << "_readdir_get_frag got diri " << diri << " " << diri->inode.ino << endl;
    assert(diri);
    assert(diri->inode.mode & INODE_MODE_DIR);
  }
  
  if (!dirp->inode && diri) {
    dout(10) << "_readdir_get_frag attaching inode" << endl;
    dirp->inode = inode_map[ino];
    diri->get();
  }

  if (res == -EAGAIN) {
    dout(10) << "_readdir_get_frag got EAGAIN, retrying" << endl;
    _readdir_rechoose_frag(dirp);
    return _readdir_get_frag(dirp);
  }

  if (res == 0) {
    // stuff dir contents to cache, DirResult
    assert(diri);

    // create empty result vector
    dirp->buffer[fg].clear();

    if (fg.is_leftmost()) {
      // add . and ..?
      string dot(".");
      _readdir_add_dirent(dirp, dot, diri);
      string dotdot("..");
      if (diri->dn)
	_readdir_add_dirent(dirp, dotdot, diri->dn->dir->parent_inode);
      //else
      //_readdir_add_dirent(dirp, dotdot, DT_DIR);
    }
    
    // the rest?
    if (!reply->get_dir_dn().empty()) {
      // only open dir if we're actually adding stuff to it!
      Dir *dir = diri->open_dir();
      assert(dir);
      utime_t now = g_clock.real_now();
      
      list<InodeStat*>::const_iterator pin = reply->get_dir_in().begin();
      for (list<string>::const_iterator pdn = reply->get_dir_dn().begin();
	   pdn != reply->get_dir_dn().end(); 
           ++pdn, ++pin) {
	// count entries
        res++;
	
	// put in cache
	Inode *in = this->insert_inode(dir, *pin, *pdn);
	
	if (g_conf.client_cache_stat_ttl) {
	  in->valid_until = now;
	  in->valid_until += g_conf.client_cache_stat_ttl;
	}
	else if (g_conf.client_cache_readdir_ttl) {
	  in->valid_until = now;
	  in->valid_until += g_conf.client_cache_readdir_ttl;
	}
	
	// contents to caller too!
	dout(15) << "_readdir_get_frag got " << *pdn << " to " << in->inode.ino << endl;
	_readdir_add_dirent(dirp, *pdn, in);
      }

      if (dir->is_empty())
	close_dir(dir);
    }

    // FIXME: remove items in cache that weren't in my readdir?
    // ***
  } else {
    dout(10) << "_readdir_get_frag got error " << res << ", setting end flag" << endl;
    dirp->set_end();
  }

  delete reply;

  return res;
}

int Client::readdir_r(DIR *d, struct dirent *de)
{  
  return readdirplus_r(d, de, 0, 0);
}

int Client::readdirplus_r(DIR *d, struct dirent *de, struct stat *st, int *stmask)
{  
  DirResult *dirp = (DirResult*)d;
  
  while (1) {
    if (dirp->at_end()) return -1;

    if (dirp->buffer.count(dirp->frag()) == 0) {
      Mutex::Locker lock(client_lock);
      _readdir_get_frag(dirp);
      if (dirp->at_end()) return -1;
    }

    frag_t fg = dirp->frag();
    uint32_t pos = dirp->fragpos();
    assert(dirp->buffer.count(fg));   
    vector<DirEntry> &ent = dirp->buffer[fg];

    if (ent.empty()) {
      dout(10) << "empty frag " << fg << ", moving on to next" << endl;
      _readdir_next_frag(dirp);
      continue;
    }

    assert(pos < ent.size());
    _readdir_fill_dirent(de, &ent[pos], dirp->offset);
    if (st) *st = ent[pos].st;
    if (stmask) *stmask = ent[pos].stmask;
    pos++;
    dirp->offset++;

    if (pos == ent.size()) 
      _readdir_next_frag(dirp);

    break;
  }

  return 0;
}


int Client::closedir(DIR *dir) 
{
  Mutex::Locker lock(client_lock);
  tout << "closedir" << endl;
  tout << (unsigned long)dir << endl;

  dout(3) << "closedir(" << dir << ") = 0" << endl;
  _closedir((DirResult*)dir);
  return 0;
}

void Client::_closedir(DirResult *dirp)
{
  dout(10) << "_closedir(" << dirp << ")" << endl;
  if (dirp->inode) {
    dout(10) << "_closedir detaching inode " << dirp->inode << endl;
    put_inode(dirp->inode);
    dirp->inode = 0;
  }
  delete dirp;
}

void Client::rewinddir(DIR *dirp)
{
  dout(3) << "rewinddir(" << dirp << ")" << endl;
  DirResult *d = (DirResult*)dirp;
  d->offset = 0;
  d->buffer.clear();
}
 
off_t Client::telldir(DIR *dirp)
{
  DirResult *d = (DirResult*)dirp;
  dout(3) << "telldir(" << dirp << ") = " << d->offset << endl;
  return d->offset;
}

void Client::seekdir(DIR *dirp, off_t offset)
{
  dout(3) << "seekdir(" << dirp << ", " << offset << ")" << endl;
  DirResult *d = (DirResult*)dirp;
  d->offset = offset;
}







/****** file i/o **********/

int Client::open(const char *relpath, int flags, mode_t mode) 
{
  Mutex::Locker lock(client_lock);
  tout << "open" << endl;
  tout << relpath << endl;
  tout << flags << endl;

  string abspath;
  mkabspath(relpath, abspath);

  Fh *fh;
  int r = _open(abspath.c_str(), flags, mode, &fh);
  if (r >= 0) {
    // allocate a integer file descriptor
    assert(fh);
    r = get_fd();
    assert(fd_map.count(r) == 0);
    fd_map[r] = fh;
  }
  
  tout << r << endl;
  dout(3) << "open(" << relpath << ", " << flags << ") = " << r << endl;
  return r;
}

int Client::_open(const char *path, int flags, mode_t mode, Fh **fhp) 
{
  // go
  MClientRequest *req = new MClientRequest(MDS_OP_OPEN, messenger->get_myinst());
  req->set_path(path); 
  req->args.open.flags = flags;
  req->args.open.mode = mode;

  int cmode = req->get_open_file_mode();

  // FIXME where does FUSE maintain user information
  req->set_caller_uid(getuid());
  req->set_caller_gid(getgid());

  // do i have the inode?
  Dentry *dn = lookup(req->get_filepath());
  Inode *in = 0;
  if (dn) {
    in = dn->inode;
    in->add_open(cmode);  // make note of pending open, since it effects _wanted_ caps.
  }
  
  MClientReply *reply = make_request(req);
  assert(reply);

  insert_trace(reply);  
  int result = reply->get_result();

  // success?
  if (result >= 0) {
    // yay
    Fh *f = new Fh;
    if (fhp) *fhp = f;
    f->mode = cmode;

    // inode
    f->inode = inode_map[reply->get_ino()];
    assert(f->inode);
    f->inode->get();

    if (!in) {
      in = f->inode;
      in->add_open(f->mode);
    }

    // caps included?
    int mds = reply->get_source().num();

    if (in->caps.empty()) {// first caps?
      dout(7) << " first caps on " << in->inode.ino << endl;
      in->get();
    }

    int new_caps = reply->get_file_caps();

    assert(reply->get_file_caps_seq() >= in->caps[mds].seq);
    if (reply->get_file_caps_seq() > in->caps[mds].seq) {   
      int old_caps = in->caps[mds].caps;

      dout(7) << "open got caps " << cap_string(new_caps)
	      << " (had " << cap_string(old_caps) << ")"
              << " for " << in->ino() 
              << " seq " << reply->get_file_caps_seq() 
              << " from mds" << mds 
	      << endl;

      in->caps[mds].caps = new_caps;
      in->caps[mds].seq = reply->get_file_caps_seq();

      // we shouldn't ever lose caps at this point.
      // actually, we might...?
      assert((old_caps & ~in->caps[mds].caps) == 0);

      if (g_conf.client_oc)
        in->fc.set_caps(new_caps);

    } else {
      dout(7) << "open got SAME caps " << cap_string(new_caps) 
              << " for " << in->ino() 
              << " seq " << reply->get_file_caps_seq() 
              << " from mds" << mds 
	      << endl;
    }
    
    dout(5) << "open success, fh is " << f << " combined caps " << cap_string(in->file_caps()) << endl;
  }

  delete reply;

  trim_cache();

  return result;
}





void Client::close_release(Inode *in)
{
  dout(10) << "close_release on " << in->ino() << endl;
  dout(10) << " wr " << in->num_open_wr << " rd " << in->num_open_rd
	   << " dirty " << in->fc.is_dirty() << " cached " << in->fc.is_cached() << endl;

  if (!in->num_open_rd) 
    in->fc.release_clean();

  int retain = 0;
  if (in->num_open_wr || in->fc.is_dirty()) retain |= CAP_FILE_WR | CAP_FILE_WRBUFFER | CAP_FILE_WREXTEND;
  if (in->num_open_rd || in->fc.is_cached()) retain |= CAP_FILE_RD | CAP_FILE_RDCACHE;

  release_caps(in, retain);              // release caps now.
}

void Client::close_safe(Inode *in)
{
  dout(10) << "close_safe on " << in->ino() << endl;
  put_inode(in);
  if (unmounting) 
    mount_cond.Signal();
}


int Client::close(int fd)
{
  Mutex::Locker lock(client_lock);
  tout << "close" << endl;
  tout << fd << endl;

  dout(3) << "close(" << fd << ")" << endl;
  assert(fd_map.count(fd));
  Fh *fh = fd_map[fd];
  _release(fh);
  fd_map.erase(fd);
  return 0;
}

int Client::_release(Fh *f)
{
  //dout(3) << "op: client->close(open_files[ " << fh << " ]);" << endl;
  //dout(3) << "op: open_files.erase( " << fh << " );" << endl;
  dout(5) << "_release " << f << endl;
  Inode *in = f->inode;

  // update inode rd/wr counts
  int before = in->file_caps_wanted();
  in->sub_open(f->mode);
  int after = in->file_caps_wanted();

  // does this change what caps we want?
  if (before != after && after)
    update_caps_wanted(in);

  // release caps right away?
  dout(10) << "num_open_rd " << in->num_open_rd << "  num_open_wr " << in->num_open_wr << endl;

  if (g_conf.client_oc) {
    // caching on.
    if (in->num_open_rd == 0 && in->num_open_wr == 0) {
      in->fc.empty(new C_Client_CloseRelease(this, in));
    } 
    else if (in->num_open_rd == 0) {
      in->fc.release_clean();
      close_release(in);
    } 
    else if (in->num_open_wr == 0) {
      in->fc.flush_dirty(new C_Client_CloseRelease(this,in));
    }

    // pin until safe?
    if (in->num_open_wr == 0 && !in->fc.all_safe()) {
      dout(10) << "pinning ino " << in->ino() << " until safe" << endl;
      in->get();
      in->fc.add_safe_waiter(new C_Client_CloseSafe(this, in));
    }
  } else {
    // caching off.
    if (in->num_open_rd == 0 && in->num_open_wr == 0) {
      dout(10) << "  releasing caps on " << in->ino() << endl;
      release_caps(in);              // release caps now.
    }
  }
  
  put_inode( in );
  return 0;
}



// ------------
// read, write


off_t Client::lseek(int fd, off_t offset, int whence)
{
  Mutex::Locker lock(client_lock);
  tout << "lseek" << endl;
  tout << fd << endl;
  tout << offset << endl;
  tout << whence << endl;

  assert(fd_map.count(fd));
  Fh *f = fd_map[fd];
  Inode *in = f->inode;

  switch (whence) {
  case SEEK_SET: 
    f->pos = offset;
    break;

  case SEEK_CUR:
    f->pos += offset;
    break;

  case SEEK_END:
    f->pos = in->inode.size + offset;
    break;

  default:
    assert(0);
  }
  
  off_t pos = f->pos;

  dout(3) << "lseek(" << fd << ", " << offset << ", " << whence << ") = " << pos << endl;
  return pos;
}



void Client::lock_fh_pos(Fh *f)
{
  dout(10) << "lock_fh_pos " << f << endl;

  if (f->pos_locked || !f->pos_waiters.empty()) {
    Cond cond;
    f->pos_waiters.push_back(&cond);
    dout(10) << "lock_fh_pos BLOCKING on " << f << endl;
    while (f->pos_locked || f->pos_waiters.front() != &cond)
      cond.Wait(client_lock);
    dout(10) << "lock_fh_pos UNBLOCKING on " << f << endl;
    assert(f->pos_waiters.front() == &cond);
    f->pos_waiters.pop_front();
  }
  
  f->pos_locked = true;
}

void Client::unlock_fh_pos(Fh *f)
{
  dout(10) << "unlock_fh_pos " << f << endl;
  f->pos_locked = false;
}



//char *hackbuf = 0;


// blocking osd interface

int Client::read(int fd, char *buf, off_t size, off_t offset) 
{
  Mutex::Locker lock(client_lock);
  tout << "read" << endl;
  tout << fd << endl;
  tout << size << endl;
  tout << offset << endl;

  assert(fd_map.count(fd));
  Fh *f = fd_map[fd];
  bufferlist bl;
  int r = _read(f, offset, size, &bl);
  dout(3) << "read(" << fd << ", " << buf << ", " << size << ", " << offset << ") = " << r << endl;
  if (r >= 0) {
    bl.copy(0, bl.length(), buf);
    r = bl.length();
  }
  return r;
}

int Client::_read(Fh *f, off_t offset, off_t size, bufferlist *bl)
{
  Inode *in = f->inode;

  bool movepos = false;
  if (offset < 0) {
    lock_fh_pos(f);
    offset = f->pos;
    movepos = true;
  }

  bool lazy = f->mode == FILE_MODE_LAZY;
  
  // determine whether read range overlaps with file
  // ...ONLY if we're doing async io
  if (!lazy && (in->file_caps() & (CAP_FILE_WRBUFFER|CAP_FILE_RDCACHE))) {
    // we're doing buffered i/o.  make sure we're inside the file.
    // we can trust size info bc we get accurate info when buffering/caching caps are issued.
    dout(10) << "file size: " << in->inode.size << endl;
    if (offset > 0 && offset >= in->inode.size) {
      if (movepos) unlock_fh_pos(f);
      return 0;
    }
    if (offset + size > (off_t)in->inode.size) 
      size = (off_t)in->inode.size - offset;
    
    if (size == 0) {
      dout(10) << "read is size=0, returning 0" << endl;
      if (movepos) unlock_fh_pos(f);
      return 0;
    }
  } else {
    // unbuffered, synchronous file i/o.  
    // or lazy.
    // defer to OSDs for file bounds.
  }
  
  int r = 0;
  int rvalue = 0;

  if (g_conf.client_oc) {
    // object cache ON
    rvalue = r = in->fc.read(offset, size, *bl, client_lock);  // may block.

    /*
    if (in->inode.ino == 0x10000000075 && hackbuf) {
      int s = MIN(size, bl->length());
      char *v = bl->c_str();
      for (int a=0; a<s; a++) 
	if (v[a] != hackbuf[offset+a]) 
	  dout(1) << "** hackbuf differs from read value at offset " << a 
		  << " hackbuf[a] = " << (int)hackbuf[a] << ", read got " << (int)v[a]
		  << endl;
    }
    */

  } else {
    // object cache OFF -- legacy inconsistent way.

    // do we have read file cap?
    while (!lazy && (in->file_caps() & CAP_FILE_RD) == 0) {
      dout(7) << " don't have read cap, waiting" << endl;
      Cond cond;
      in->waitfor_read.push_back(&cond);
      cond.Wait(client_lock);
    }  
    // lazy cap?
    while (lazy && (in->file_caps() & CAP_FILE_LAZYIO) == 0) {
      dout(7) << " don't have lazy cap, waiting" << endl;
      Cond cond;
      in->waitfor_lazy.push_back(&cond);
      cond.Wait(client_lock);
    }
    
    // do sync read
    Cond cond;
    bool done = false;
    C_Cond *onfinish = new C_Cond(&cond, &done, &rvalue);

    Objecter::OSDRead *rd = filer->prepare_read(in->inode, offset, size, bl);
    if (in->hack_balance_reads ||
	g_conf.client_hack_balance_reads)
      rd->balance_reads = true;
    r = objecter->readx(rd, onfinish);
    assert(r >= 0);

    // wait!
    while (!done)
      cond.Wait(client_lock);
  }
  
  if (movepos) {
    // adjust fd pos
    f->pos = offset+bl->length();
    unlock_fh_pos(f);
  }

  // done!
  return rvalue;
}



/*
 * hack -- 
 *  until we properly implement synchronous writes wrt buffer cache,
 *  make sure we delay shutdown until they're all safe on disk!
 */
class C_Client_HackUnsafe : public Context {
  Client *cl;
public:
  C_Client_HackUnsafe(Client *c) : cl(c) {}
  void finish(int) {
    cl->hack_sync_write_safe();
  }
};

void Client::hack_sync_write_safe()
{
  client_lock.Lock();
  assert(unsafe_sync_write > 0);
  unsafe_sync_write--;
  if (unsafe_sync_write == 0 && unmounting) {
    dout(10) << "hack_sync_write_safe -- no more unsafe writes, unmount can proceed" << endl;
    mount_cond.Signal();
  }
  client_lock.Unlock();
}

int Client::write(int fd, const char *buf, off_t size, off_t offset) 
{
  Mutex::Locker lock(client_lock);
  tout << "write" << endl;
  tout << fd << endl;
  tout << size << endl;
  tout << offset << endl;

  assert(fd_map.count(fd));
  Fh *fh = fd_map[fd];
  int r = _write(fh, offset, size, buf);
  dout(3) << "write(" << fd << ", \"...\", " << size << ", " << offset << ") = " << r << endl;
  return r;
}


int Client::_write(Fh *f, off_t offset, off_t size, const char *buf)
{
  //dout(7) << "write fh " << fh << " size " << size << " offset " << offset << endl;
  Inode *in = f->inode;

  // use/adjust fd pos?
  if (offset < 0) {
    lock_fh_pos(f);
    offset = f->pos;
    f->pos = offset+size;    
    unlock_fh_pos(f);
  }

  bool lazy = f->mode == FILE_MODE_LAZY;

  dout(10) << "cur file size is " << in->inode.size << "    wr size " << in->file_wr_size << endl;

  // time it.
  utime_t start = g_clock.real_now();
    
  // copy into fresh buffer (since our write may be resub, async)
  bufferptr bp = buffer::copy(buf, size);
  bufferlist blist;
  blist.push_back( bp );

  if (g_conf.client_oc) { // buffer cache ON?
    assert(objectcacher);

    /*
    if (f->inode->inode.ino == 0x10000000075) {
      if (!hackbuf) {
	dout(7) << "alloc and zero new hackbuf" << endl;
	hackbuf = new char[16384];
	memset(hackbuf, 0, 16384);
      }
      dout(7) << "hackbuf copying " << offset << "~" << size << " first is " << (int)buf[0] << endl;
      memcpy(hackbuf+offset, buf, size);
      for (int a=0; a<size; a++) 
	dout(10) << "hackbuf[" << (a+offset) << " = " << (int)hackbuf[a+offset] << " = " << (int)buf[a] << endl;
    }
    */

    // write (this may block!)
    in->fc.write(offset, size, blist, client_lock);

  } else {
    // legacy, inconsistent synchronous write.
    dout(7) << "synchronous write" << endl;

    // do we have write file cap?
    while (!lazy && (in->file_caps() & CAP_FILE_WR) == 0) {
      dout(7) << " don't have write cap, waiting" << endl;
      Cond cond;
      in->waitfor_write.push_back(&cond);
      cond.Wait(client_lock);
    }
    while (lazy && (in->file_caps() & CAP_FILE_LAZYIO) == 0) {
      dout(7) << " don't have lazy cap, waiting" << endl;
      Cond cond;
      in->waitfor_lazy.push_back(&cond);
      cond.Wait(client_lock);
    }

    // prepare write
    Cond cond;
    bool done = false;
    C_Cond *onfinish = new C_Cond(&cond, &done);
    C_Client_HackUnsafe *onsafe = new C_Client_HackUnsafe(this);
    unsafe_sync_write++;
    in->sync_writes++;
    
    dout(20) << " sync write start " << onfinish << endl;
    
    filer->write(in->inode, offset, size, blist, 0, 
                 onfinish, onsafe
		 //, 1+((int)g_clock.now()) / 10 //f->pos // hack hack test osd revision snapshots
		 ); 
    
    while (!done) {
      cond.Wait(client_lock);
      dout(20) << " sync write bump " << onfinish << endl;
    }

    in->sync_writes--;
    if (in->sync_writes == 0 &&
        !in->waitfor_no_write.empty()) {
      for (list<Context*>::iterator i = in->waitfor_no_write.begin();
           i != in->waitfor_no_write.end();
           i++)
        (*i)->finish(0);
      in->waitfor_no_write.clear();
    }

    dout(20) << " sync write done " << onfinish << endl;
  }

  // time
  utime_t lat = g_clock.real_now();
  lat -= start;
  if (client_logger) {
    client_logger->finc("wrlsum",(double)lat);
    client_logger->inc("wrlnum");
  }
    
  // assume success for now.  FIXME.
  off_t totalwritten = size;
  
  // extend file?
  if (totalwritten + offset > in->inode.size) {
    in->inode.size = in->file_wr_size = totalwritten + offset;
    dout(7) << "wrote to " << totalwritten+offset << ", extending file size" << endl;
  } else {
    dout(7) << "wrote to " << totalwritten+offset << ", leaving file size at " << in->inode.size << endl;
  }

  // mtime
  in->file_wr_mtime = in->inode.mtime = g_clock.real_now();

  // ok!
  return totalwritten;  
}


int Client::truncate(const char *relpath, off_t length) 
{
  Mutex::Locker lock(client_lock);
  tout << "truncate" << endl;
  tout << relpath << endl;
  tout << length << endl;

  string path;
  mkabspath(relpath, path);
  return _truncate(path.c_str(), length);
}

int Client::_truncate(const char *file, off_t length) 
{
  MClientRequest *req = new MClientRequest(MDS_OP_TRUNCATE, messenger->get_myinst());
  req->set_path(file); 
  req->args.truncate.length = length;

  // FIXME where does FUSE maintain user information
  req->set_caller_uid(getuid());
  req->set_caller_gid(getgid());
  
  MClientReply *reply = make_request(req);
  int res = reply->get_result();
  insert_trace(reply);  
  delete reply;

  dout(3) << "truncate(\"" << file << "\", " << length << ") = " << res << endl;
  return res;
}

int Client::ftruncate(int fd, off_t length) 
{
  Mutex::Locker lock(client_lock);
  tout << "ftruncate" << endl;
  tout << fd << endl;
  tout << length << endl;

  assert(fd_map.count(fd));
  Fh *f = fd_map[fd];
  return _ftruncate(f, length);
}

int Client::_ftruncate(Fh *fh, off_t length) 
{
  MClientRequest *req = new MClientRequest(MDS_OP_TRUNCATE, messenger->get_myinst());
  req->args.truncate.ino = fh->inode->inode.ino;
  req->args.truncate.length = length;

  // FIXME where does FUSE maintain user information
  req->set_caller_uid(getuid());
  req->set_caller_gid(getgid());
  
  MClientReply *reply = make_request(req);
  int res = reply->get_result();
  insert_trace(reply);  
  delete reply;

  dout(3) << "ftruncate(\"" << fh << "\", " << length << ") = " << res << endl;
  return res;
}


int Client::fsync(int fd, bool syncdataonly) 
{
  Mutex::Locker lock(client_lock);
  tout << "fsync" << endl;
  tout << fd << endl;
  tout << syncdataonly << endl;

  assert(fd_map.count(fd));
  Fh *f = fd_map[fd];
  int r = _fsync(f, syncdataonly);
  dout(3) << "fsync(" << fd << ", " << syncdataonly << ") = " << r << endl;
  return r;
}

int Client::_fsync(Fh *f, bool syncdataonly)
{
  int r = 0;

  Inode *in = f->inode;

  // metadata?
  if (!syncdataonly) {
    dout(0) << "fsync - not syncing metadata yet.. implement me" << endl;
  }

  // data?
  Cond cond;
  bool done = false;
  if (!objectcacher->commit_set(in->ino(),
                                new C_Cond(&cond, &done))) {
    // wait for callback
    while (!done) cond.Wait(client_lock);
  }
  return r;
}


// not written yet, but i want to link!

int Client::chdir(const char *path)
{
  Mutex::Locker lock(client_lock);
  tout << "chdir" << endl;
  tout << path << endl;
  
  // fake it for now!
  string abs;
  mkabspath(path, abs);
  dout(3) << "chdir " << path << " -> cwd now " << abs << endl;
  cwd = abs;
  return 0;
}

int Client::statfs(const char *path, struct statvfs *stbuf)
{
  Mutex::Locker lock(client_lock);
  tout << "statfs" << endl;
  return _statfs(stbuf);
}

int Client::ll_statfs(inodeno_t ino, struct statvfs *stbuf)
{
  Mutex::Locker lock(client_lock);
  tout << "ll_statfs" << endl;
  return _statfs(stbuf);
}

int Client::_statfs(struct statvfs *stbuf)
{
  dout(3) << "_statfs" << endl;

  Cond cond;
  tid_t tid = ++last_tid;
  StatfsRequest *req = new StatfsRequest(tid, &cond);
  statfs_requests[tid] = req;

  int mon = monmap->pick_mon();
  messenger->send_message(new MStatfs(req->tid), monmap->get_inst(mon));

  while (req->reply == 0)
    cond.Wait(client_lock);

  // yay
  memcpy(stbuf, &req->reply->stfs, sizeof(*stbuf));

  statfs_requests.erase(req->tid);
  delete req->reply;
  delete req;

  int r = 0;
  dout(3) << "_statfs = " << r << endl;
  return r;
}

void Client::handle_statfs_reply(MStatfsReply *reply)
{
  if (statfs_requests.count(reply->tid) &&
      statfs_requests[reply->tid]->reply == 0) {
    dout(10) << "handle_statfs_reply " << *reply << ", kicking waiter" << endl;
    statfs_requests[reply->tid]->reply = reply;
    statfs_requests[reply->tid]->caller_cond->Signal();
  } else {
    dout(10) << "handle_statfs_reply " << *reply << ", dup or old, dropping" << endl;
    delete reply;
  }
}


int Client::lazyio_propogate(int fd, off_t offset, size_t count)
{
  client_lock.Lock();
  dout(3) << "op: client->lazyio_propogate(" << fd
          << ", " << offset << ", " << count << ")" << endl;
  
  assert(fd_map.count(fd));
  Fh *f = fd_map[fd];
  Inode *in = f->inode;

  if (f->mode & FILE_MODE_LAZY) {
    // wait for lazy cap
    while ((in->file_caps() & CAP_FILE_LAZYIO) == 0) {
      dout(7) << " don't have lazy cap, waiting" << endl;
      Cond cond;
      in->waitfor_lazy.push_back(&cond);
      cond.Wait(client_lock);
    }

    if (g_conf.client_oc) {
      Cond cond;
      bool done = false;
      in->fc.flush_dirty(new C_SafeCond(&client_lock, &cond, &done));
      
      while (!done)
        cond.Wait(client_lock);
      
    } else {
      // mmm, nothin to do.
    }
  }

  client_lock.Unlock();
  return 0;
}

int Client::lazyio_synchronize(int fd, off_t offset, size_t count)
{
  client_lock.Lock();
  dout(3) << "op: client->lazyio_synchronize(" << fd
          << ", " << offset << ", " << count << ")" << endl;
  
  assert(fd_map.count(fd));
  Fh *f = fd_map[fd];
  Inode *in = f->inode;
  
  if (f->mode & FILE_MODE_LAZY) {
    // wait for lazy cap
    while ((in->file_caps() & CAP_FILE_LAZYIO) == 0) {
      dout(7) << " don't have lazy cap, waiting" << endl;
      Cond cond;
      in->waitfor_lazy.push_back(&cond);
      cond.Wait(client_lock);
    }
    
    if (g_conf.client_oc) {
      in->fc.flush_dirty(0);       // flush to invalidate.
      in->fc.release_clean();
    } else {
      // mm, nothin to do.
    }
  }
  
  client_lock.Unlock();
  return 0;
}




// =========================================
// low level

// ugly hack for ll
#define FUSE_SET_ATTR_MODE	(1 << 0)
#define FUSE_SET_ATTR_UID	(1 << 1)
#define FUSE_SET_ATTR_GID	(1 << 2)
#define FUSE_SET_ATTR_SIZE	(1 << 3)
#define FUSE_SET_ATTR_ATIME	(1 << 4)
#define FUSE_SET_ATTR_MTIME	(1 << 5)

int Client::ll_lookup(inodeno_t parent, const char *name, struct stat *attr)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_lookup " << parent << " " << name << endl;
  tout << "ll_lookup" << endl;
  tout << parent.val << endl;
  tout << name << endl;

  string dname = name;
  Inode *diri = 0;
  int r = 0;

  if (inode_map.count(parent) == 0) {
    tout << 0 << endl;
    dout(1) << "ll_lookup " << parent << " " << name << " -> ENOENT (parent DNE... WTF)" << endl;
    r = -ENOENT;
    goto out;
  }
  diri = inode_map[parent];
  if (!diri->inode.is_dir()) {
    tout << 0 << endl;
    dout(1) << "ll_lookup " << parent << " " << name << " -> ENOTDIR (parent not a dir... WTF)" << endl;
    r = -ENOTDIR;
    goto out;
  }


  // refresh the dir?
  //  FIXME: this is the hackish way.
  if (!diri->dir ||
      diri->dir->dentries.count(dname) == 0) {
    string path;
    diri->make_path(path);
    DirResult *dirp = new DirResult(path, diri);

    while (1) {
      hash<string> H;
      dirp->set_frag(diri->dirfragtree[H(dname)]);

      dout(10) << "ll_lookup fetching frag " << dirp->frag() << " for " << name << endl;
      int r = _readdir_get_frag(dirp);
      if (r < 0) return r;

      if (dirp->buffer.count(diri->dirfragtree[H(dname)])) break;
      dirp->buffer.clear();
    }

    _closedir(dirp);
  }

  // do we have it?
  if (diri->dir &&
      diri->dir->dentries.count(dname)) {
    Inode *in = diri->dir->dentries[dname]->inode;
    fill_stat(in, attr);
    _ll_get(in);
    assert(inode_map[in->inode.ino] == in);
  } else {
    r = -ENOENT;
  }

 out:
  dout(3) << "ll_lookup " << parent << " " << name
	  << " -> " << r << " (" << hex << attr->st_ino << dec << ")" << endl;
  tout << attr->st_ino << endl;
  return r;
}

void Client::_ll_get(Inode *in)
{
  if (in->ll_ref == 0) 
    in->get();
  in->ll_get();
  dout(20) << "_ll_get " << in << " " << in->inode.ino << " -> " << in->ll_ref << endl;
}

int Client::_ll_put(Inode *in, int num)
{
  in->ll_put(num);
  dout(20) << "_ll_put " << in << " " << in->inode.ino << " " << num << " -> " << in->ll_ref << endl;
  if (in->ll_ref == 0) {
    put_inode(in);
    return 0;
  } else {
    return in->ll_ref;
  }
}

void Client::_ll_drop_pins()
{
  dout(10) << "_ll_drop_pins" << endl;
  hash_map<inodeno_t, Inode*>::iterator next;
  for (hash_map<inodeno_t, Inode*>::iterator it = inode_map.begin();
       it != inode_map.end();
       it = next) {
    Inode *in = it->second;
    next = it;
    next++;
    if (in->ll_ref)
      _ll_put(in, in->ll_ref);
  }
}

bool Client::ll_forget(inodeno_t ino, int num)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_forget " << ino << " " << num << endl;
  tout << "ll_forget" << endl;
  tout << ino.val << endl;
  tout << num << endl;

  if (ino == 1) return true;  // ignore forget on root.

  bool last = false;
  if (inode_map.count(ino) == 0) {
    dout(1) << "WARNING: ll_forget on " << ino << " " << num 
	    << ", which I don't have" << endl;
  } else {
    Inode *in = inode_map[ino];
    assert(in);
    if (_ll_put(in, num) == 0)
      last = true;
  }
  return last;
}

Inode *Client::_ll_get_inode(inodeno_t ino)
{
  if (inode_map.count(ino) == 0) {
    assert(ino == 1);  // must be the root inode.
    Inode *in;
    int r = _do_lstat("/", 0, &in);
    assert(r >= 0);
    return in;
  } else {
    return inode_map[ino];
  }
}


int Client::ll_getattr(inodeno_t ino, struct stat *attr)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_getattr " << ino << endl;
  tout << "ll_getattr" << endl;
  tout << ino.val << endl;

  Inode *in = _ll_get_inode(ino);
  fill_stat(in, attr);
  return 0;
}

int Client::ll_setattr(inodeno_t ino, struct stat *attr, int mask)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_setattr " << ino << " mask " << hex << mask << dec << endl;
  tout << "ll_setattr" << endl;
  tout << ino.val << endl;
  tout << attr->st_mode << endl;
  tout << attr->st_uid << endl;
  tout << attr->st_gid << endl;
  tout << attr->st_size << endl;
  tout << attr->st_mtime << endl;
  tout << attr->st_atime << endl;
  tout << mask << endl;

  Inode *in = _ll_get_inode(ino);

  string path;
  in->make_path(path);

  int r;
  if ((mask & FUSE_SET_ATTR_MODE) &&
      ((r = _chmod(path.c_str(), attr->st_mode)) < 0)) return r;

  if ((mask & FUSE_SET_ATTR_UID) && (mask & FUSE_SET_ATTR_GID) &&
      ((r = _chown(path.c_str(), attr->st_uid, attr->st_gid)) < 0)) return r;
  //if ((mask & FUSE_SET_ATTR_GID) &&
  //(r = client->_chgrp(path.c_str(), attr->st_gid) < 0)) return r;

  if ((mask & FUSE_SET_ATTR_SIZE) &&
      ((r = _truncate(path.c_str(), attr->st_size)) < 0)) return r;
  
  if ((mask & FUSE_SET_ATTR_MTIME) && (mask & FUSE_SET_ATTR_ATIME)) {
    if ((r = _utimes(path.c_str(), utime_t(attr->st_mtime,0), utime_t(attr->st_atime,0))) < 0) return r;
  } else if (mask & FUSE_SET_ATTR_MTIME) {
    if ((r = _utimes(path.c_str(), utime_t(attr->st_mtime,0), utime_t())) < 0) return r;
  } else if (mask & FUSE_SET_ATTR_ATIME) {
    if ((r = _utimes(path.c_str(), utime_t(), utime_t(attr->st_atime,0))) < 0) return r;
  }
  
  assert(r == 0);
  fill_stat(in, attr);

  dout(3) << "ll_setattr " << ino << " = " << r << endl;
  return 0;
}

int Client::ll_readlink(inodeno_t ino, const char **value)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_readlink " << ino << endl;
  tout << "ll_readlink" << endl;
  tout << ino.val << endl;

  Inode *in = _ll_get_inode(ino);
  int r = 0;
  if (in->inode.is_symlink()) {
    *value = in->symlink->c_str();
  } else {
    *value = "";
    r = -EINVAL;
  }
  dout(3) << "ll_readlink " << ino << " = " << r << " (" << *value << ")" << endl;
  return r;
}

int Client::ll_mknod(inodeno_t parent, const char *name, mode_t mode, dev_t rdev, struct stat *attr)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_mknod " << parent << " " << name << endl;
  tout << "ll_mknod" << endl;
  tout << parent.val << endl;
  tout << name << endl;
  tout << mode << endl;
  tout << rdev << endl;

  Inode *diri = _ll_get_inode(parent);

  string path;
  diri->make_path(path);
  path += "/";
  path += name;
  int r = _mknod(path.c_str(), mode, rdev);
  if (r == 0) {
    string dname(name);
    Inode *in = diri->dir->dentries[dname]->inode;
    fill_stat(in, attr);
    _ll_get(in);
  }
  tout << attr->st_ino << endl;
  dout(3) << "ll_mknod " << parent << " " << name
	  << " = " << r << " (" << hex << attr->st_ino << dec << ")" << endl;
  return r;
}

int Client::ll_mkdir(inodeno_t parent, const char *name, mode_t mode, struct stat *attr)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_mkdir " << parent << " " << name << endl;
  tout << "ll_mkdir" << endl;
  tout << parent.val << endl;
  tout << name << endl;
  tout << mode << endl;

  Inode *diri = _ll_get_inode(parent);

  string path;
  diri->make_path(path);
  path += "/";
  path += name;
  int r = _mkdir(path.c_str(), mode);
  if (r == 0) {
    string dname(name);
    Inode *in = diri->dir->dentries[dname]->inode;
    fill_stat(in, attr);
    _ll_get(in);
  }
  tout << attr->st_ino << endl;
  dout(3) << "ll_mkdir " << parent << " " << name
	  << " = " << r << " (" << hex << attr->st_ino << dec << ")" << endl;
  return r;
}

int Client::ll_symlink(inodeno_t parent, const char *name, const char *value, struct stat *attr)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_symlink " << parent << " " << name << " -> " << value << endl;
  tout << "ll_symlink" << endl;
  tout << parent.val << endl;
  tout << name << endl;
  tout << value << endl;

  Inode *diri = _ll_get_inode(parent);

  string path;
  diri->make_path(path);
  path += "/";
  path += name;
  int r = _symlink(value, path.c_str());
  if (r == 0) {
    string dname(name);
    Inode *in = diri->dir->dentries[dname]->inode;
    fill_stat(in, attr);
    _ll_get(in);
  }
  tout << attr->st_ino << endl;
  dout(3) << "ll_symlink " << parent << " " << name
	  << " = " << r << " (" << hex << attr->st_ino << dec << ")" << endl;
  return r;
}

int Client::ll_unlink(inodeno_t ino, const char *name)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_unlink " << ino << " " << name << endl;
  tout << "ll_unlink" << endl;
  tout << ino.val << endl;
  tout << name << endl;

  Inode *diri = _ll_get_inode(ino);

  string path;
  diri->make_path(path);
  path += "/";
  path += name;
  return _unlink(path.c_str());
}

int Client::ll_rmdir(inodeno_t ino, const char *name)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_rmdir " << ino << " " << name << endl;
  tout << "ll_rmdir" << endl;
  tout << ino.val << endl;
  tout << name << endl;

  Inode *diri = _ll_get_inode(ino);

  string path;
  diri->make_path(path);
  path += "/";
  path += name;
  return _rmdir(path.c_str());
}

int Client::ll_rename(inodeno_t parent, const char *name, inodeno_t newparent, const char *newname)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_rename " << parent << " " << name << " to "
	  << newparent << " " << newname << endl;
  tout << "ll_rename" << endl;
  tout << parent.val << endl;
  tout << name << endl;
  tout << newparent.val << endl;
  tout << newname << endl;

  Inode *diri = _ll_get_inode(parent);
  string path;
  diri->make_path(path);
  path += "/";
  path += name;

  Inode *newdiri = _ll_get_inode(newparent);
  string newpath;
  newdiri->make_path(newpath);
  newpath += "/";
  newpath += newname;

  return _rename(path.c_str(), newpath.c_str());
}

int Client::ll_link(inodeno_t ino, inodeno_t newparent, const char *newname, struct stat *attr)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_link " << ino << " to " << newparent << " " << newname << endl;
  tout << "ll_link" << endl;
  tout << ino.val << endl;
  tout << newparent << endl;
  tout << newname << endl;

  Inode *old = _ll_get_inode(ino);
  Inode *diri = _ll_get_inode(newparent);

  string path;
  old->make_path(path);

  string newpath;
  diri->make_path(newpath);
  newpath += "/";
  newpath += newname;

  int r = _link(path.c_str(), newpath.c_str());
  if (r == 0) {
    string dname(newname);
    Inode *in = diri->dir->dentries[dname]->inode;
    fill_stat(in, attr);
    _ll_get(in);
  }
  return r;
}

int Client::ll_opendir(inodeno_t ino, void **dirpp)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_opendir " << ino << endl;
  tout << "ll_opendir" << endl;
  tout << ino.val << endl;
  
  Inode *diri = inode_map[ino];
  assert(diri);
  string path;
  diri->make_path(path);

  int r = _opendir(path.c_str(), (DirResult**)dirpp);

  tout << (unsigned long)*dirpp << endl;

  dout(3) << "ll_opendir " << ino << " = " << r << " (" << *dirpp << ")" << endl;
  return r;
}

void Client::ll_releasedir(void *dirp)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_releasedir " << dirp << endl;
  tout << "ll_releasedir" << endl;
  tout << (unsigned long)dirp << endl;
  _closedir((DirResult*)dirp);
}

int Client::ll_open(inodeno_t ino, int flags, Fh **fhp)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_open " << ino << " " << flags << endl;
  tout << "ll_open" << endl;
  tout << ino.val << endl;
  tout << flags << endl;

  Inode *in = _ll_get_inode(ino);
  string path;
  in->make_path(path);

  int r = _open(path.c_str(), flags, 0, fhp);

  tout << (unsigned long)*fhp << endl;
  dout(3) << "ll_open " << ino << " " << flags << " = " << r << " (" << *fhp << ")" << endl;
  return r;
}

int Client::ll_create(inodeno_t parent, const char *name, mode_t mode, int flags, 
		      struct stat *attr, Fh **fhp)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_create " << parent << " " << name << " 0" << oct << mode << dec << " " << flags << endl;
  tout << "ll_create" << endl;
  tout << parent.val << endl;
  tout << name << endl;
  tout << mode << endl;
  tout << flags << endl;

  Inode *pin = _ll_get_inode(parent);
  string path;
  pin->make_path(path);
  path += "/";
  path += name;

  int r = _open(path.c_str(), flags|O_CREAT, mode, fhp);
  if (r >= 0) {
    Inode *in = (*fhp)->inode;
    fill_stat(in, attr);
    _ll_get(in);
  }
  tout << (unsigned long)*fhp << endl;
  tout << attr->st_ino << endl;
  dout(3) << "ll_create " << parent << " " << name << " 0" << oct << mode << dec << " " << flags
	  << " = " << r << " (" << *fhp << " " << hex << attr->st_ino << dec << ")" << endl;
  return 0;
}

int Client::ll_read(Fh *fh, off_t off, off_t len, bufferlist *bl)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_read " << fh << " " << off << "~" << len << endl;
  tout << "ll_read" << endl;
  tout << (unsigned long)fh << endl;
  tout << off << endl;
  tout << len << endl;

  return _read(fh, off, len, bl);
}

int Client::ll_write(Fh *fh, off_t off, off_t len, const char *data)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_write " << fh << " " << off << "~" << len << endl;
  tout << "ll_write" << endl;
  tout << (unsigned long)fh << endl;
  tout << off << endl;
  tout << len << endl;

  return _write(fh, off, len, data);
}

int Client::ll_release(Fh *fh)
{
  Mutex::Locker lock(client_lock);
  dout(3) << "ll_release " << fh << endl;
  tout << "ll_release" << endl;
  tout << (unsigned long)fh << endl;

  _release(fh);
  return 0;
}






// =========================================
// layout


int Client::describe_layout(int fd, FileLayout *lp)
{
  Mutex::Locker lock(client_lock);

  assert(fd_map.count(fd));
  Fh *f = fd_map[fd];
  Inode *in = f->inode;

  *lp = in->inode.layout;

  dout(3) << "describe_layout(" << fd << ") = 0" << endl;
  return 0;
}

int Client::get_stripe_unit(int fd)
{
  FileLayout layout;
  describe_layout(fd, &layout);
  return layout.stripe_unit;
}

int Client::get_stripe_width(int fd)
{
  FileLayout layout;
  describe_layout(fd, &layout);
  return layout.stripe_width();
}

int Client::get_stripe_period(int fd)
{
  FileLayout layout;
  describe_layout(fd, &layout);
  return layout.period();
}

int Client::enumerate_layout(int fd, list<ObjectExtent>& result,
			     off_t length, off_t offset)
{
  Mutex::Locker lock(client_lock);

  assert(fd_map.count(fd));
  Fh *f = fd_map[fd];
  Inode *in = f->inode;

  // map to a list of extents
  filer->file_to_extents(in->inode, offset, length, result);

  dout(3) << "enumerate_layout(" << fd << ", " << length << ", " << offset << ") = 0" << endl;
  return 0;
}


void Client::ms_handle_failure(Message *m, const entity_inst_t& inst)
{
  entity_name_t dest = inst.name;

  if (dest.is_mon()) {
    // resend to a different monitor.
    int mon = monmap->pick_mon(true);
    dout(0) << "ms_handle_failure " << *m << " to " << inst 
            << ", resending to mon" << mon 
            << endl;
    messenger->send_message(m, monmap->get_inst(mon));
  }
  else if (dest.is_osd()) {
    objecter->ms_handle_failure(m, dest, inst);
  } 
  else if (dest.is_mds()) {
    dout(0) << "ms_handle_failure " << *m << " to " << inst << endl;
    //failed_mds.insert(dest.num());
  }
  else {
    // client?
    dout(0) << "ms_handle_failure " << *m << " to " << inst << ", dropping" << endl;
    delete m;
  }
}

