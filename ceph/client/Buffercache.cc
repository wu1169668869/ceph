#include "Buffercache.h"

#include "config.h"
#undef dout
#define  dout(l)    if (l<=g_conf.debug) cout << "client" << "." << pthread_self() << " "

// -- Bufferhead methods

Bufferhead::Bufferhead(inodeno_t ino, off_t off, Buffercache *bc) : 
  ref(0) {
  dout(10) << "bc: new bufferhead ino: " << ino << " offset: " << off << endl;
  this->ino = ino;
  offset = off;
  state = BUFHD_STATE_CLEAN;
  this->bc = bc;
  fc = bc->get_fc(ino);
  bc->lru.lru_insert_top(this); //FIXME: parameterize whether top or mid
  assert(!fc->buffer_map.count(offset)); // fail loudly if offset already exists!
  fc->insert(offset, this); 
  dirty_since = 0; // meaningless when clean or inflight
  // buffers are allocated later
}

Bufferhead::~Bufferhead()
{
  dout(10) << "bc: destroying bufferhead ino: " << ino << " size: " << bl.length() << " offset: " << offset << endl;
  assert(state == BUFHD_STATE_CLEAN);
  assert(ref == 0);
  assert(lru_is_expireable());
  assert(read_waiters.empty());
  assert(write_waiters.empty());
  assert(!fc->buffer_map.count(offset));
  bc->lru.lru_remove(this);   
  // debug segmentation fault
  if (bl.buffers().empty()) {
    dout(10) << "bc: bufferlist is empty" << endl;
#if 0
  } else {
    for (list<bufferptr>::iterator it = bl.buffers().begin();
         it != bl.buffers().end();
	 it++) {
      //dout(10) << "bc: bufferptr len: " << it->length() << " off: " << it->offset() << endl;
      dout(10) << "bc: bufferptr: " << *it << endl; 
    }
    dout(10) <<"bc: listed all bufferptrs" << endl;
#endif
  }   
}

void Bufferhead::alloc_buffers(size_t size)
{
  dout(10) << "bc: allocating buffers size: " << size << endl;
  assert(size > 0);
  while (size > 0) {
    if (size <= (unsigned)g_conf.client_bcache_alloc_maxsize) {
          size_t k = g_conf.client_bcache_alloc_minsize;
          size_t asize = size - size % k + (size % k > 0) * k;
	  buffer *b = new buffer(asize);
	  b->set_length(size);
	  bl.push_back(b);
	  bc->increase_size(size);
          dout(10) << "bc: new buffer(" << asize << "), total: " << bl.length() << endl;
	  break;
	}
        buffer *b = new buffer(g_conf.client_bcache_alloc_maxsize);
	b->set_length(g_conf.client_bcache_alloc_maxsize);
	bl.push_back(b);
        dout(10) << "bc: new buffer(" << g_conf.client_bcache_alloc_maxsize << "), total: " << bl.length() << endl;
	size -= g_conf.client_bcache_alloc_maxsize;
	bc->increase_size(g_conf.client_bcache_alloc_maxsize);
  }
  dout(6) << "bc: allocated " << bl.buffers().size() << " buffers (" << bl.length() << " bytes) " << endl;
  assert(bl.length() == size);
}

void Bufferhead::miss_start(size_t miss_len) 
{
  assert(state == BUFHD_STATE_CLEAN);
  get();
  state = BUFHD_STATE_RX;
  this->miss_len = miss_len;
  bc->lru.lru_touch(this);
}

void Bufferhead::miss_finish() 
{
  assert(state == BUFHD_STATE_RX);
  state = BUFHD_STATE_CLEAN;
  if (bl.length() == 0) {
    alloc_buffers(miss_len); 
    bl.zero();
    dout(6) << "bc: miss_finish: allocated zeroed buffer len: " << bl.length() << endl;
  } else {
    bc->increase_size(bl.length());
  }
  dout(6) << "bc: miss_finish: clean_size: " << bc->get_clean_size() << " dirty_size: " << bc->get_dirty_size() << " rx_size: " << bc->get_rx_size() << " tx_size: " << bc->get_tx_size() << " age: " << bc->dirty_buffers.get_age() << endl;
  //assert(bl.length() == miss_len);
  wakeup_read_waiters();
  wakeup_write_waiters();
  put();
}

void Bufferhead::dirty() 
{
  if (state == BUFHD_STATE_CLEAN) {
    get();
    dout(6) << "bc: dirtying clean buffer size: " << bl.length() << endl;
    state = BUFHD_STATE_DIRTY;
    dirty_since = time(NULL); // start clock for dirty buffer here
    bc->lru.lru_touch(this);
    dout(6) << "bc: dirty before: clean_size: " << bc->get_clean_size() << " dirty_size: " << bc->get_dirty_size() << " rx_size: " << bc->get_rx_size() << " tx_size: " << bc->get_tx_size() << " age: " << bc->dirty_buffers.get_age() << endl;
    bc->clean_to_dirty(bl.length());
    dout(6) << "bc: dirty after: clean_size: " << bc->get_clean_size() << " dirty_size: " << bc->get_dirty_size() << " rx_size: " << bc->get_rx_size() << " tx_size: " << bc->get_tx_size() << " age: " << bc->dirty_buffers.get_age() << endl;
    assert(!bc->dirty_buffers.exist(this));
    bc->dirty_buffers.insert(this); 
    assert(!fc->dirty_buffers.count(this));
    fc->dirty_buffers.insert(this);
  } else {
    dout(10) << "bc: dirtying dirty buffer size: " << bl.length() << endl;
  }
}

void Bufferhead::dirtybuffers_erase() 
{
  dout(10) << "bc: erase in dirtybuffers size: " << bl.length() << " in state " << state << endl;
  assert(bc->dirty_buffers.exist(this));
  bc->dirty_buffers.erase(this);
  assert(fc->dirty_buffers.count(this));
  fc->dirty_buffers.erase(this);
  put();
}

void Bufferhead::flush_start() 
{
  dout(10) << "bc: flush_start" << endl;
  assert(state == BUFHD_STATE_DIRTY);
  get();
  state = BUFHD_STATE_TX;
  dirtybuffers_erase();
  assert(!bc->inflight_buffers.count(this));
  bc->inflight_buffers.insert(this);
  bc->dirty_to_tx(bl.length());
  dout(6) << "bc: flush_start: clean_size: " << bc->get_clean_size() << " dirty_size: " << bc->get_dirty_size() << " rx_size: " << bc->get_rx_size() << " tx_size: " << bc->get_tx_size() << " age: " << bc->dirty_buffers.get_age() << endl;
  assert(!fc->inflight_buffers.count(this));
  fc->inflight_buffers.insert(this);
}

void Bufferhead::flush_finish() 
{
  dout(10) << "bc: flush_finish" << endl;
  assert(state == BUFHD_STATE_TX);
  state = BUFHD_STATE_CLEAN;
  assert(bc->inflight_buffers.count(this));
  bc->inflight_buffers.erase(this);
  bc->tx_to_clean(bl.length());
  dout(6) << "bc: flush_finish: clean_size: " << bc->get_clean_size() << " dirty_size: " << bc->get_dirty_size() << " rx_size: " << bc->get_rx_size() << " tx_size: " << bc->get_tx_size() << " age: " << bc->dirty_buffers.get_age() << endl;
  assert(fc->inflight_buffers.count(this));
  fc->inflight_buffers.erase(this);
  wakeup_write_waiters(); // readers never wait on flushes
  put();
}

void Bufferhead::claim_append(Bufferhead *other) 
{
  dout(10) << "bc: claim_append old bl size: " << bl.buffers().size() << " length " << bl.length() << endl;
  bl.claim_append(other->bl);
  dout(10) << "bc: claim_append new bl size: " << bl.buffers().size() << " length: " << bl.length() << endl;
  // keep older time stamp
  if (other->dirty_since < dirty_since) dirty_since = other->dirty_since;
  other->bl.clear();
}

// -- Dirtybuffers methods

void Dirtybuffers::erase(Bufferhead* bh) 
{
  dout(7) << "dirtybuffer: erase bh->ino: " << bh->ino << " offset: " << bh->offset << endl;
  unsigned osize = _dbufs.size();
  for (multimap<time_t, Bufferhead*>::iterator it = _dbufs.lower_bound(bh->dirty_since);
       it != _dbufs.upper_bound(bh->dirty_since);
       it++) {
   if (it->second == bh) {
      _dbufs.erase(it);
      break;
    }
  }
  assert(_dbufs.size() == osize - 1);
}

void Dirtybuffers::insert(Bufferhead* bh) 
{
  dout(7) << "dirtybuffer: insert bh->ino: " << bh->ino << " offset: " << bh->offset << endl;
  _dbufs.insert(pair<time_t, Bufferhead*>(bh->dirty_since, bh));
}

bool Dirtybuffers::exist(Bufferhead* bh) 
{
  for (multimap<time_t, Bufferhead*>::iterator it = _dbufs.lower_bound(bh->dirty_since);
      it != _dbufs.upper_bound(bh->dirty_since);
      it++ ) {
    if (it->second == bh) {
      dout(10) << "dirtybuffer: found bh->ino: " << bh->ino << " offset: " << bh->offset << endl;
      return true;
    }
  }
  return false;
}


void Dirtybuffers::get_expired(time_t ttl, int left_dirty, set<Bufferhead*>& to_flush) 
{
  dout(6) << "bc: get_expired ttl: " << ttl << " left_dirty: " << left_dirty << endl;
  if (left_dirty > 0) {
    time_t now = time(NULL);
    for (multimap<time_t, Bufferhead*>::iterator it = _dbufs.begin();
      it != _dbufs.end() && left_dirty > 0;
      it++) {
      if (ttl > now - it->second->dirty_since &&
	  left_dirty >= (int)it->second->bc->get_dirty_size()) break;
      to_flush.insert(it->second);
      left_dirty -= it->second->length();
    }
    dout(6) << "bc: get_expired to_flush.size(): " << to_flush.size() << endl;
  }
}
										    
  // -- Filecache methods

void Filecache::insert(off_t offset, Bufferhead* bh)
{
  pair<map<off_t, Bufferhead*>::iterator, bool> rvalue;
  rvalue = buffer_map.insert(pair<off_t, Bufferhead*> (offset, bh));

  // The following is just to get the pieces for the last two assertions 
  map<off_t, Bufferhead*>::iterator next_buf = buffer_map.upper_bound(offset);

  map<off_t, Bufferhead*>::iterator prev_buf = rvalue.first;
  if (prev_buf != buffer_map.begin()) {
    prev_buf--;
  } else {
    prev_buf = buffer_map.end();
  }

  // raise if there is any overlap!
  assert(next_buf == buffer_map.end() || next_buf->first >= offset + bh->length());
  assert(prev_buf == buffer_map.end() || prev_buf->first + prev_buf->second->length() <= offset);
}


map<off_t, Bufferhead*>::iterator Filecache::overlap(size_t len, off_t off)
{
  // returns iterator to buffer overlapping specified extent or end() if no overlap exists
  dout(7) << "bc: overlap " << len << " " << off << endl;

  if (buffer_map.empty()) return buffer_map.end();

  // find first buffer with offset >= off
  map<off_t, Bufferhead*>::iterator it = buffer_map.lower_bound(off);

  // Found buffer with exact offset
  if (it != buffer_map.end() && it->first == off) {
    dout(6) << "bc: overlap -- found buffer with exact offset" << endl;
    return it;
  }

  // examine previous buffer (< off) first in case of two overlaps
  if (it != buffer_map.begin()) {
    it--;
    if (it->first + it->second->length() > off) {
      dout(6) << "bc: overlap -- found overlap with previous buffer" << endl;
      return it;
    } else {
      dout(6) << "bc: overlap -- no overlap with previous buffer" << endl;
      it++;
    }
  }

  // then examine current buffer (> off)
  if (it != buffer_map.end() && it->first < off + len) {
    dout(6) << "bc: overlap -- overlap found" << endl;
    return it;
  } 

  // give up
  dout(6) << "bc: overlap -- no overlap found" << endl;
  return buffer_map.end();
}

map<off_t, Bufferhead*>::iterator 
Filecache::map_existing(size_t len, 
			off_t start_off,
			map<off_t, Bufferhead*>& hits, 
			map<off_t, Bufferhead*>& rx, 
			map<off_t, Bufferhead*>& tx, 
			map<off_t, size_t>& holes)
{
  dout(7) << "bc: map_existing len: " << len << " off: " << start_off << endl;
  off_t need_off = start_off;
  off_t actual_off = start_off;
  map<off_t, Bufferhead*>::iterator existing, rvalue = overlap(len, start_off);
  for (existing = rvalue;
       existing != buffer_map.end() && existing->first < start_off + len;
       existing++) {
    dout(7) << "bc: map: found overlap at offset " << actual_off << endl;
    actual_off = existing->first;
    Bufferhead *bh = existing->second;

    if (actual_off > need_off) {
      assert(buffer_map.count(need_off) == 0);
      holes[need_off] = (size_t) (actual_off - need_off);
      dout(6) << "bc: map: hole " << need_off << " " << holes[need_off] << endl;
      need_off = actual_off;
    } 

    // raise if this buffer overlaps with previous buffer
    assert(existing == rvalue || actual_off == need_off);

    // sort buffer into maps
    if (bh->state == BUFHD_STATE_RX) {
      rx[actual_off] = bh;
      dout(6) << "bc: map: rx " << actual_off << " " << rx[actual_off]->length() << endl;
    } else if (bh->state == BUFHD_STATE_TX) {
      tx[actual_off] = bh;
      dout(6) << "bc: map: tx " << actual_off << " " << tx[actual_off]->length() << endl;
    } else if (bh->state == BUFHD_STATE_CLEAN || bh->state == BUFHD_STATE_DIRTY) {
      hits[actual_off] = bh;
      dout(6) << "bc: map: hits " << actual_off << " " << hits[actual_off]->length() << endl;
    } else {
      dout(1) << "map_existing: Unknown state!" << endl;
      assert(0);
    }
    need_off = actual_off + bh->length();
    assert(bh->length() > 0);
  }

  // no buffers or no buffers at tail
  if (need_off < start_off + len) {
    holes[need_off] = (size_t) (start_off + len - need_off);
    dout(6) << "bc: map: last hole " << need_off << " " << holes[need_off] << endl;
    assert(buffer_map.count(need_off) == 0);
  }
  return rvalue;
}

void Filecache::simplify()
{
  dout(7) << "bc: simplify" << endl;
  list<Bufferhead*> removed;
  map<off_t, Bufferhead*>::iterator start, next;
  start = buffer_map.begin();
  next = buffer_map.begin();
  int count = 0;
  while (start != buffer_map.end()) {
    next++;
    while (next != buffer_map.end() &&
           start->second->state != BUFHD_STATE_RX &&
           start->second->state != BUFHD_STATE_TX &&
	   start->second->state == next->second->state &&
	   start->second->offset + start->second->bl.length() == next->second->offset &&
	   next->second->read_waiters.empty() &&
	   next->second->write_waiters.empty()) {
      dout(10) << "bc: simplify start: " << start->first << " next: " << next->first << endl;
      Bufferhead *bh = next->second;
      start->second->claim_append(bh);
      if (bh->state == BUFHD_STATE_DIRTY) {
        bh->dirtybuffers_erase(); 
	bh->state = BUFHD_STATE_CLEAN; 
      }
      removed.push_back(bh);
      count++;
      next++;
    }
    if (next != buffer_map.end()) {
      dout(10) << "bc: simplify failed, start state: " << start->second->state << " next state: " << next->second->state << endl;
      dout(10) << "bc: simplify failed, start offset + len " << start->second->offset + start->second->bl.length() << " next offset: " << next->second->offset << endl;
      dout(10) << "bc: simplify failed, " << next->second->read_waiters.size() << " read waiters" << endl;
      dout(10) << "bc: simplify failed, " << next->second->write_waiters.size() << " write waiters" << endl;
    }
    start = next;
  }
  dout(7) << "bc: simplified " << count << " buffers" << endl;
  for (list<Bufferhead*>::iterator it = removed.begin();
       it != removed.end();
       it++) {
    buffer_map.erase((*it)->offset);
    delete *it;
  }
  assert(!buffer_map.empty());
}

int Filecache::copy_out(size_t size, off_t offset, char *dst) 
{
  dout(7) << "bc: copy_out size: " << size << " offset: " << offset << endl;
  assert(offset >= 0);
  //assert(offset + size <= length()); doesn't hold after trim_bcache
  int rvalue = size;
  
  map<off_t, Bufferhead*>::iterator curbuf = buffer_map.lower_bound(offset);
  if (curbuf == buffer_map.end() || curbuf->first > offset) {
    if (curbuf == buffer_map.begin()) {
      return -1;
    } else {
      curbuf--;
    }
  }
  offset -= curbuf->first;
  dout(6) << "bc: copy_out: curbuf offset: " << curbuf->first << " offset: " << offset << endl;
  assert(offset >= 0);
  
  while (size > 0) {
    Bufferhead *bh = curbuf->second;
    if (offset + size <= bh->length()) {
      dout(6) << "bc: copy_out bh len: " << bh->length() << " size: " << size << endl;
      dout(10) << "bc: want to copy off: " << offset << " size: " << size << endl;
      bh->bl.copy(offset, size, dst);
      size = 0;
      break;
    }
    
    int howmuch = bh->length() - offset;
    dout(6) << "bc: copy_out bh len: " << bh->length() << " size: " << size << endl;
    dout(10) << "bc: want to copy off: " << offset << " size: " << howmuch << endl;
    bh->bl.copy(offset, howmuch, dst);
    
    dst += howmuch;
    size -= howmuch;
    offset = 0;
    curbuf++;
    if (curbuf == buffer_map.end()) {
      dout(5) << "bc: copy_out size: " << size << endl;
      assert(curbuf != buffer_map.end());
    }
  }
  return rvalue - size;
}

// -- Buffercache methods

void Buffercache::dirty(inodeno_t ino, size_t size, off_t offset, const char *src) 
{
  dout(6) << "bc: dirty ino: " << ino << " size: " << size << " offset: " << offset << endl;
  assert(bcache_map.count(ino)); // filecache has to be already allocated!!
  Filecache *fc = get_fc(ino);
  assert(offset >= 0);
  
  map<off_t, Bufferhead*>::iterator curbuf = fc->buffer_map.lower_bound(offset);
  if (curbuf == fc->buffer_map.end() || curbuf->first > offset) {
    assert(curbuf != fc->buffer_map.begin());
    curbuf--;
  }
  offset -= curbuf->first;
  assert(offset >= 0);
  
  while (size > 0) {
    Bufferhead *bh = curbuf->second;
    if (offset + size <= bh->length()) {
      bh->bl.copy_in(offset, size, src); // last bit
      bh->dirty();
      break;
    }
    
    int howmuch = bh->length() - offset;
    bh->bl.copy_in(offset, howmuch, src);
    bh->dirty();    
    src += howmuch;
    size -= howmuch;
    offset = 0;
    curbuf++;
    assert(curbuf != fc->buffer_map.end());
  }
}


size_t Buffercache::touch_continuous(map<off_t, Bufferhead*>& hits, size_t size, off_t offset)
{
  dout(7) << "bc: touch_continuous size: " << size << " offset: " << offset << endl;
  off_t next_off = offset;
  if (hits.begin()->first > offset ||
      hits.begin()->first + hits.begin()->second->length() <= offset) {
    return 0;
  }
  for (map<off_t, Bufferhead*>::iterator curbuf = hits.begin(); 
       curbuf != hits.end();
       curbuf++) {
    if (curbuf == hits.begin()) {
      next_off = curbuf->first;
    } else if (curbuf->first != next_off) {
      break;
    }
    lru.lru_touch(curbuf->second);
    next_off += curbuf->second->length();
  }
  return (size_t)(next_off - offset) >= size ? size : (next_off - offset);
}

void Buffercache::map_or_alloc(inodeno_t ino, size_t size, off_t offset, 
                               map<off_t, Bufferhead*>& buffers, 
                               map<off_t, Bufferhead*>& rx,
			       map<off_t, Bufferhead*>& tx)
{
  dout(7) << "bc: map_or_alloc len: " << size << " off: " << offset << endl;
  Filecache *fc = get_fc(ino);
  map<off_t, size_t> holes;
  holes.clear();
  fc->map_existing(size, offset, buffers, rx, tx, holes);
  // stuff buffers into holes
  for (map<off_t, size_t>::iterator hole = holes.begin();
       hole != holes.end();
       hole++) {
    dout(10) << "bc: allocate hole " << hole->first << " " << hole->second << endl;
    assert(buffers.count(hole->first) == 0);
    Bufferhead *bh = new Bufferhead(ino, hole->first, this);
    buffers[hole->first] = bh;
    bh->alloc_buffers(hole->second);
  }
  // split buffers
  // FIXME: not implemented yet
}

void Buffercache::release_file(inodeno_t ino) 
{
  dout(7) << "bc: release_file ino: " << ino << endl;
  assert(bcache_map.count(ino));
  Filecache *fc = bcache_map[ino];
  map<off_t, Bufferhead*> to_release = fc->buffer_map;
  fc->buffer_map.clear();
  for (map<off_t, Bufferhead*>::iterator it = to_release.begin();
       it != to_release.end();
       it++) {
    decrease_size(it->second->length());
    dout(6) << "bc: release_file: clean_size: " << get_clean_size() << " dirty_size: " << get_dirty_size() << " rx_size: " << get_rx_size() << " tx_size: " << get_tx_size() << " age: " << dirty_buffers.get_age() << endl;
    delete it->second;    
  }
  bcache_map.erase(ino);
  delete fc;  
}

void Buffercache::get_reclaimable(size_t min_size, list<Bufferhead*>& reclaimed)
{
  while (min_size > 0) {
    if (Bufferhead *bh = (Bufferhead*)lru.lru_expire()) {
      reclaimed.push_back(bh);
      min_size -= bh->length();
    } else {
      break;
    }
  }
}


size_t Buffercache::reclaim(size_t min_size)
{
  dout(7) << "bc: reclaim min_size: " << min_size << endl;
  size_t freed_size = 0;
  while (freed_size < min_size) {
    Bufferhead *bh = (Bufferhead*)lru.lru_expire();
    if (!bh) {
      dout(6) << "bc: nothing more to reclaim -- freed_size: " << freed_size << endl;
      break; // nothing more to reclaim
    } else {
      dout(6) << "bc: reclaim: offset: " << bh->offset << " len: " << bh->length() << endl;
      assert(bh->state == BUFHD_STATE_CLEAN);
      freed_size += bh->length();

      decrease_size(bh->length());

      dout(6) << "bc: reclaim: clean_size: " << get_clean_size() << " dirty_size: " << get_dirty_size() << " rx_size: " << get_rx_size() << " tx_size: " << get_tx_size() << " age: " << dirty_buffers.get_age() << endl;
      assert(clean_size >= 0);
      bh->fc->buffer_map.erase(bh->offset);
      if (bh->fc->buffer_map.empty()) {
        bcache_map.erase(bh->ino);
	delete bh->fc;
      }
      delete bh;
    }
  }
  return freed_size;
}

