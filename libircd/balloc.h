/*
 *  ircd-ratbox: A slightly useful ircd.
 *  balloc.h: The ircd block allocator header.
 *
 *  Copyright (C) 1990 Jarkko Oikarinen and University of Oulu, Co Center
 *  Copyright (C) 1996-2002 Hybrid Development Team
 *  Copyright (C) 2002-2005 ircd-ratbox development team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
 *  USA
 *
 *  $Id$
 */

#ifndef IRCD_LIB_H
# error "Do not use balloc.h directly"
#endif

#ifndef INCLUDED_balloc_h
#define INCLUDED_balloc_h

#ifdef NOBALLOC 
	 
typedef struct ircd_bh ircd_bh; 	 
#define ircd_init_bh() 	 
#define ircd_bh_create(es, epb) ((ircd_blockheap*)(es)) 	 
#define ircd_bh_destroy(x) 	 
#define ircd_bh_alloc(x) ircd_malloc((int)x) 	 
#define ircd_bh_free(x,y) ircd_free(y) 	 
#define ircd_bh_usage(bh, bused, bfree, bmemusage) do { (*(size_t *)bused) = 0; *((size_t *)bfree) = 0; *((size_t *)bmemusage) = 0; } while(0)
 
#else

#undef DEBUG_BALLOC

#ifdef DEBUG_BALLOC
#define BALLOC_MAGIC 0x3d3a3c3d
#define BALLOC_FREE_MAGIC 0xafafafaf
#endif

/* status information for an allocated block in heap */
struct ircd_heap_block
{
	size_t alloc_size;
	struct ircd_heap_block *next;	/* Next in our chain of blocks */
	void *elems;		/* Points to allocated memory */
	dlink_list free_list;
	dlink_list used_list;
};
typedef struct ircd_heap_block ircd_heap_block;

struct ircd_heap_memblock
{
#ifdef DEBUG_BALLOC
	unsigned long magic;
#endif
	dlink_node self;
	ircd_heap_block *block;		/* Which block we belong to */
};

typedef struct ircd_heap_memblock ircd_heap_memblock;

/* information for the root node of the heap */
struct ircd_bh
{
	dlink_node hlist;
	size_t elemSize;	/* Size of each element to be stored */
	unsigned long elemsPerBlock;	/* Number of elements per block */
	unsigned long blocksAllocated;	/* Number of blocks allocated */
	unsigned long freeElems;		/* Number of free elements */
	ircd_heap_block *base;		/* Pointer to first block */
};
typedef struct ircd_bh ircd_bh;

int ircd_bh_free(ircd_bh *, void *);
void *ircd_bh_alloc(ircd_bh *);

ircd_bh *ircd_bh_create(size_t elemsize, int elemsperblock);
int ircd_bh_destroy(ircd_bh * bh);

void ircd_init_bh(void);
void ircd_bh_usage(ircd_bh * bh, size_t * bused, size_t * bfree, size_t * bmemusage);



#endif /* NOBALLOC */

#endif /* INCLUDED_balloc_h */
