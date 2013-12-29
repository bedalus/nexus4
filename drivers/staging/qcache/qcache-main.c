/*
 * zcache.c
 *
 * Copyright (c) 2010,2011, Dan Magenheimer, Oracle Corp.
 * Copyright (c) 2010,2011, Nitin Gupta
 *
 * Zcache provides an in-kernel "host implementation" for transcendent memory
 * and, thus indirectly, for cleancache and frontswap.  Zcache includes two
 * page-accessible memory [1] interfaces, both utilizing the crypto compression
 * API:
 * 1) "compression buddies" ("zbud") is used for ephemeral pages
 * 2) zsmalloc is used for persistent pages.
 * Xvmalloc (based on the TLSF allocator) has very low fragmentation
 * so maximizes space efficiency, while zbud allows pairs (and potentially,
 * in the future, more than a pair of) compressed pages to be closely linked
 * so that reclaiming can be done via the kernel's physical-page-oriented
 * "shrinker" interface.
 *
 * [1] For a definition of page-accessible memory (aka PAM), see:
 *   http://marc.info/?l=linux-mm&m=127811271605009
 */
