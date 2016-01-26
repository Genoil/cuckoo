// Cuckoo Cycle, a memory-hard proof-of-work
// Copyright (c) 2013-2016 John Tromp
// The edge-trimming time-memory trade-off is due to Dave Anderson:
// The use of prefetching was suggested by Alexander Peslyak (aka Solar Designer)
// http://da-data.blogspot.com/2014/03/a-public-review-of-cuckoo-cycle.html

#include "cuckoo.h"
#ifdef __APPLE__
#include "osx_barrier.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <assert.h>
#include <vector>
#ifdef ATOMIC
#include <atomic>
typedef std::atomic<u32> au32;
typedef std::atomic<u64> au64;
#else
typedef u32 au32;
typedef u64 au64;
#endif
#if SIZESHIFT <= 32
typedef u32 nonce_t;
typedef u32 node_t;
#else
typedef u64 nonce_t;
typedef u64 node_t;
#endif
#include <set>

// algorithm parameters
#ifndef PART_BITS
// #bits used to partition edge set processing to save memory
// a value of 0 does no partitioning and is fastest
// a value of 1 partitions in two, making twice_set the
// same size as shrinkingset at about 33% slowdown
// higher values are not that interesting
#define PART_BITS 0
#endif

#ifndef IDXSHIFT
// we want sizeof(cuckoo_hash) == sizeof(twice_set), so
// CUCKOO_SIZE * sizeof(u64) == TWICE_WORDS * sizeof(u32)
// CUCKOO_SIZE * 2 == TWICE_WORDS
// (SIZE >> IDXSHIFT) * 2 == 2 * ONCE_BITS / 32
// SIZE >> IDXSHIFT == HALFSIZE >> PART_BITS >> 5
// IDXSHIFT == 1 + PART_BITS + 5
#define IDXSHIFT (PART_BITS + 6)
#endif
// grow with cube root of size, hardly affected by trimming
#define MAXPATHLEN (8 << (SIZESHIFT/3))

// set that starts out full and gets reset by threads on disjoint words
class shrinkingset {
public:
  std::vector<u64> bits;
  std::vector<u64> cnt;

  shrinkingset(u32 nthreads) {
    nonce_t nwords = HALFSIZE/64;
    bits.resize(nwords);
    cnt.resize(nthreads);
    cnt[0] = HALFSIZE;
  }
  u64 count() const {
    u64 sum = 0LL;
    for (u32 i=0; i<cnt.size(); i++)
      sum += cnt[i];
    return sum;
  }
  void reset(nonce_t n, u32 thread) {
    bits[n/64] |= 1LL << (n%64);
    cnt[thread]--;
  }
  bool test(node_t n) const {
    return !((bits[n/64] >> (n%64)) & 1LL);
  }
  u64 block(node_t n) const {
    return ~bits[n/64];
  }
};

#define PART_MASK ((1 << PART_BITS) - 1)
#define ONCE_BITS (HALFSIZE >> PART_BITS)
#define TWICE_WORDS ((2 * ONCE_BITS) / 32)

class twice_set {
public:
  au32 *bits;

  twice_set() {
    bits = (au32 *)calloc(TWICE_WORDS, sizeof(au32));
    assert(bits != 0);
  }
  void reset() {
    memset(bits, 0, TWICE_WORDS*sizeof(au32));
  }
  void prefetch(node_t u) const {
#ifdef PREFETCH
    __builtin_prefetch((const void *)(&bits[u/16]), /*READ=*/0, /*TEMPORAL=*/0);
#endif
  }
  void set(node_t u) {
    node_t idx = u/16;
    u32 bit = 1 << (2 * (u%16));
#ifdef ATOMIC
    u32 old = std::atomic_fetch_or_explicit(&bits[idx], bit, std::memory_order_relaxed);
    if (old & bit) std::atomic_fetch_or_explicit(&bits[idx], bit<<1, std::memory_order_relaxed);
  }
  u32 test(node_t u) const {
    return (bits[u/16].load(std::memory_order_relaxed) >> (2 * (u%16))) & 2;
  }
#else
    u32 old = bits[idx];
    bits[idx] = old | (bit + (old & bit));
  }
  u32 test(node_t u) const {
    return bits[u/16] >> (2 * (u%16)) & 2;
  }
#endif
  ~twice_set() {
    free(bits);
  }
};

#define CUCKOO_SIZE (SIZE >> IDXSHIFT)
#define CUCKOO_MASK (CUCKOO_SIZE - 1)
// number of (least significant) key bits that survives leftshift by SIZESHIFT
#define KEYBITS (64-SIZESHIFT)
#define KEYMASK ((1LL << KEYBITS) - 1)
#define MAXDRIFT (1LL << (KEYBITS - IDXSHIFT))

class cuckoo_hash {
public:
  au64 *cuckoo;

  cuckoo_hash() {
    cuckoo = (au64 *)calloc(CUCKOO_SIZE, sizeof(au64));
    assert(cuckoo != 0);
  }
  ~cuckoo_hash() {
    free(cuckoo);
  }
  void set(node_t u, node_t v) {
    u64 niew = (u64)u << SIZESHIFT | v;
    for (node_t ui = u >> IDXSHIFT; ; ui = (ui+1) & CUCKOO_MASK) {
#ifdef ATOMIC
      u64 old = 0;
      if (cuckoo[ui].compare_exchange_strong(old, niew, std::memory_order_relaxed))
        return;
      if ((old >> SIZESHIFT) == (u & KEYMASK)) {
        cuckoo[ui].store(niew, std::memory_order_relaxed);
#else
      u64 old = cuckoo[ui];
      if (old == 0 || (old >> SIZESHIFT) == (u & KEYMASK)) {
        cuckoo[ui] = niew;
#endif
        return;
      }
    }
  }
  node_t operator[](node_t u) const {
    for (node_t ui = u >> IDXSHIFT; ; ui = (ui+1) & CUCKOO_MASK) {
#ifdef ATOMIC
      u64 cu = cuckoo[ui].load(std::memory_order_relaxed);
#else
      u64 cu = cuckoo[ui];
#endif
      if (!cu)
        return 0;
      if ((cu >> SIZESHIFT) == (u & KEYMASK)) {
        assert(((ui - (u >> IDXSHIFT)) & CUCKOO_MASK) < MAXDRIFT);
        return (node_t)(cu & (SIZE-1));
      }
    }
  }
};

class cuckoo_ctx {
public:
  siphash_ctx sip_ctx;
  shrinkingset *alive;
  twice_set *nonleaf;
  cuckoo_hash *cuckoo;
  nonce_t (*sols)[PROOFSIZE];
  u32 maxsols;
  au32 nsols;
  u32 nthreads;
  u32 ntrims;
  pthread_barrier_t barry;

  cuckoo_ctx(const char* header, u32 n_threads, u32 n_trims, u32 max_sols) {
    setheader(&sip_ctx, header);
    nthreads = n_threads;
    alive = new shrinkingset(nthreads);
    cuckoo = 0;
    nonleaf = new twice_set;
    ntrims = n_trims;
    int err = pthread_barrier_init(&barry, NULL, nthreads);
    assert(err == 0);
    sols = (nonce_t (*)[PROOFSIZE])calloc(maxsols = max_sols, PROOFSIZE*sizeof(nonce_t));
    assert(sols != 0);
    nsols = 0;
  }
  ~cuckoo_ctx() {
    delete alive;
    if (nonleaf)
      delete nonleaf;
    if (cuckoo)
      delete cuckoo;
  }
};

typedef struct {
  u32 id;
  pthread_t thread;
  cuckoo_ctx *ctx;
} thread_ctx;

void barrier(pthread_barrier_t *barry) {
  int rc = pthread_barrier_wait(barry);
  if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD) {
    printf("Could not wait on barrier\n");
    pthread_exit(NULL);
  }
}

#define NONCESHIFT	(SIZESHIFT-1 - PART_BITS)
#define NODEPARTMASK	(NODEMASK >> PART_BITS)
#define NONCETRUNC	(1LL << (64 - NONCESHIFT))

void count_node_deg(thread_ctx *tp, u32 uorv, u32 part) {
  cuckoo_ctx *ctx = tp->ctx;
  shrinkingset *alive = ctx->alive;
  twice_set *nonleaf = ctx->nonleaf;
  u64 buffer[64];

  for (nonce_t block = tp->id*64; block < HALFSIZE; block += ctx->nthreads*64) {
    u32 bsize = 0;
    u64 alive64 = alive->block(block);
    for (nonce_t nonce = block-1; alive64; ) { // -1 compensates for 1-based ffs
      u32 ffs = __builtin_ffsll(alive64);
      nonce += ffs; alive64 >>= ffs;
      node_t u = _sipnode(&ctx->sip_ctx, nonce, uorv);
      if ((u & PART_MASK) == part) {
        buffer[bsize++] = u >> PART_BITS;
        nonleaf->prefetch(u >> PART_BITS);
      }
      if (ffs & 64) break; // can't shift by 64
    }
    for (u32 i=0; i<bsize; i++)
      nonleaf->set(buffer[i]);
  }
}

void kill_leaf_edges(thread_ctx *tp, u32 uorv, u32 part) {
  cuckoo_ctx *ctx = tp->ctx;
  shrinkingset *alive = ctx->alive;
  twice_set *nonleaf = ctx->nonleaf;
  u64 buffer[64];

  for (nonce_t block = tp->id*64; block < HALFSIZE; block += ctx->nthreads*64) {
    u32 bsize = 0;
    u64 alive64 = alive->block(block);
    for (nonce_t nonce = block-1; alive64; ) { // -1 compensates for 1-based ffs
      u32 ffs = __builtin_ffsll(alive64);
      nonce += ffs; alive64 >>= ffs;
      node_t u = _sipnode(&ctx->sip_ctx, nonce, uorv);
      if ((u & PART_MASK) == part) {
        buffer[bsize++] = ((u64)nonce << NONCESHIFT) | (u >> PART_BITS);
        nonleaf->prefetch(u >> PART_BITS);
      }
      if (ffs & 64) break; // can't shift by 64
    }
    for (u32 i=0; i<bsize; i++) {
      u64 bi = buffer[i];
      if (!nonleaf->test(bi & NODEPARTMASK)) {
        nonce_t n = block | (bi >> NONCESHIFT);
        alive->reset(n, tp->id);
      }
    }
  }
}

u32 path(cuckoo_hash &cuckoo, node_t u, node_t *us) {
  u32 nu;
  for (nu = 0; u; u = cuckoo[u]) {
    if (++nu >= MAXPATHLEN) {
      while (nu-- && us[nu] != u) ;
      if (!~nu)
        printf("maximum path length exceeded\n");
      else printf("illegal % 4d-cycle\n", MAXPATHLEN-nu);
      pthread_exit(NULL);
    }
    us[nu] = u;
  }
  return nu;
}

typedef std::pair<node_t,node_t> edge;

void solution(cuckoo_ctx *ctx, node_t *us, u32 nu, node_t *vs, u32 nv) {
  std::set<edge> cycle;
  u32 n = 0;
  cycle.insert(edge(*us, *vs));
  while (nu--)
    cycle.insert(edge(us[(nu+1)&~1], us[nu|1])); // u's in even position; v's in odd
  while (nv--)
    cycle.insert(edge(vs[nv|1], vs[(nv+1)&~1])); // u's in odd position; v's in even
#ifdef ATOMIC
  u32 soli = std::atomic_fetch_add_explicit(&ctx->nsols, 1U, std::memory_order_relaxed);
#else
  u32 soli = ctx->nsols++;
#endif
  for (nonce_t block = 0; block < HALFSIZE; block += 64) {
    u64 alive64 = ctx->alive->block(block);
    for (nonce_t nonce = block-1; alive64; ) { // -1 compensates for 1-based ffs
      u32 ffs = __builtin_ffsll(alive64);
      nonce += ffs; alive64 >>= ffs;
      edge e(sipnode(&ctx->sip_ctx, nonce, 0), sipnode(&ctx->sip_ctx, nonce, 1));
      if (cycle.find(e) != cycle.end()) {
        ctx->sols[soli][n++] = nonce;
#ifdef SHOWSOL
        printf("e(%x)=(%x,%x)%c", nonce, e.first, e.second, n==PROOFSIZE?'\n':' ');
#endif
        if (PROOFSIZE > 2)
          cycle.erase(e);
      }
      if (ffs & 64) break; // can't shift by 64
    }
  }
  assert(n==PROOFSIZE);
}

void *worker(void *vp) {
  thread_ctx *tp = (thread_ctx *)vp;
  cuckoo_ctx *ctx = tp->ctx;

  shrinkingset *alive = ctx->alive;
  u32 load = 100LL * HALFSIZE / CUCKOO_SIZE;
  if (tp->id == 0)
    printf("initial load %d%%\n", load);
  for (u32 round=1; round <= ctx->ntrims; round++) {
    for (u32 uorv = 0; uorv < 2; uorv++) {
      for (u32 part = 0; part <= PART_MASK; part++) {
        if (tp->id == 0)
          ctx->nonleaf->reset();
        barrier(&ctx->barry);
        count_node_deg(tp,uorv,part);
        barrier(&ctx->barry);
        kill_leaf_edges(tp,uorv,part);
        barrier(&ctx->barry);
        if (tp->id == 0) {
          u32 load = (u32)(100LL * alive->count() / CUCKOO_SIZE);
          printf("round %d part %c%d load %d%%\n", round, "UV"[uorv], part, load);
        }
      }
    }
  }
  if (tp->id == 0) {
    load = (u32)(100LL * alive->count() / CUCKOO_SIZE);
    if (load >= 90) {
      printf("overloaded! exiting...");
      exit(0);
    }
    delete ctx->nonleaf;
    ctx->nonleaf = 0;
    ctx->cuckoo = new cuckoo_hash();
  }
  barrier(&ctx->barry);
  cuckoo_hash &cuckoo = *ctx->cuckoo;
  node_t us[MAXPATHLEN], vs[MAXPATHLEN];
  for (nonce_t block = tp->id*64; block < HALFSIZE; block += ctx->nthreads*64) {
    u64 alive64 = alive->block(block);
    for (nonce_t nonce = block-1; alive64; ) { // -1 compensates for 1-based ffs
      u32 ffs = __builtin_ffsll(alive64);
      nonce += ffs; alive64 >>= ffs;
      node_t u0=sipnode(&ctx->sip_ctx, nonce, 0), v0=sipnode(&ctx->sip_ctx, nonce, 1);
      if (u0 == 0) // ignore vertex 0 so it can be used as nil for cuckoo[]
        continue;
      node_t u = cuckoo[us[0] = u0], v = cuckoo[vs[0] = v0];
      u32 nu = path(cuckoo, u, us), nv = path(cuckoo, v, vs);
      if (us[nu] == vs[nv]) {
        u32 min = nu < nv ? nu : nv;
        for (nu -= min, nv -= min; us[nu] != vs[nv]; nu++, nv++) ;
        u32 len = nu + nv + 1;
        printf("% 4d-cycle found at %d:%d%%\n", len, tp->id, (u32)(nonce*100LL/HALFSIZE));
        if (len == PROOFSIZE && ctx->nsols < ctx->maxsols)
          solution(ctx, us, nu, vs, nv);
        continue;
      }
      if (nu < nv) {
        while (nu--)
          cuckoo.set(us[nu+1], us[nu]);
        cuckoo.set(u0, v0);
      } else {
        while (nv--)
          cuckoo.set(vs[nv+1], vs[nv]);
        cuckoo.set(v0, u0);
      }
      if (ffs & 64) break; // can't shift by 64
    }
  }
  pthread_exit(NULL);
  return 0;
}
