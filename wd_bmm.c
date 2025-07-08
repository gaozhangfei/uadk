/*
 * Copyright 2019 Huawei Technologies Co.,Ltd.All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Block Memory Management (lib): A block memory algorithm */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/queue.h>
#include <sys/mman.h>

#include "wd.h"
#include "wd_bmm.h"

#define __ALIGN_MASK(x, mask)  (((x) + (mask)) & ~(mask))
#define ALIGN(x, a) __ALIGN_MASK(x, (typeof(x))(a)-1)

#define TAG_FREE	0x12345678	/* block is free */
#define TAG_USED	0x87654321	/* block is busy */
#define MAX_ALIGN_SIZE	0x1000		/* 4KB */
#define MAX_BLOCK_SIZE	0x10000000	/* 256MB */
#define BLK_BALANCE_SZ	0x100000ul
#define NUM_TIMES(x)	(87 * (x) / 100)

struct wd_lock {
	__u8 lock;
};

void wd_blkpool_spinlock(struct wd_lock *lock)
{
	int val = 0;

	if (__atomic_compare_exchange_n(&lock->lock, &val, 1, 1,
					__ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		return;

	do {
		do {
			val = __atomic_load_n(&lock->lock, __ATOMIC_RELAXED);
		} while (val != 0);
	} while (!__atomic_compare_exchange_n(&lock->lock, &val, 1, 1,
					      __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));
}

void wd_blkpool_unspinlock(struct wd_lock *lock)
{
	__atomic_store_n(&lock->lock, 0, __ATOMIC_RELEASE);
}

struct wd_blk_hd {
	unsigned int blk_tag;
	void *blk_dma;
	void *blk;
	TAILQ_ENTRY(wd_blk_hd) next;
};

TAILQ_HEAD(wd_blk_list, wd_blk_hd);

struct wd_ss_region {
	void *va;
	unsigned long long pa;
	size_t size;

	TAILQ_ENTRY(wd_ss_region) next;
};

TAILQ_HEAD(wd_ss_region_list, wd_ss_region);

struct wd_blkpool {
	struct wd_lock pool_lock;
	unsigned int free_blk_num;
	unsigned int blk_num;
	unsigned int alloc_failures;
	struct wd_blk_list head;
	void *act_start;
	unsigned int hd_sz;
	unsigned int blk_sz;
	struct wd_blkpool_setup setup;

	handle_t ctx;
	void *mem;
	unsigned long size;
	struct wd_ss_region_list ss_list;
	struct wd_ss_region_list *ss_head;
};

static struct wd_blk_hd *wd_blk_head(struct wd_blkpool *pool, void *blk)
{
	unsigned long offset = (unsigned long)((uintptr_t)blk -
					       (uintptr_t)pool->act_start);
	unsigned long sz = pool->hd_sz + pool->blk_sz;
	unsigned long blk_idx = offset / sz;

	return (struct wd_blk_hd *)((uintptr_t)pool->act_start + blk_idx * sz);
}

static int pool_params_check(struct wd_blkpool_setup *setup)
{
	if (!setup->block_size ||
	    setup->block_size > MAX_BLOCK_SIZE) {
		WD_ERR("Invalid block_size (%x)!\n",
			setup->block_size);
		return -WD_EINVAL;
	}

	/* check parameters, and align_size must be 2^N */
	if (setup->align_size == 0x1 || setup->align_size > MAX_ALIGN_SIZE ||
	    setup->align_size & (setup->align_size - 0x1)) {
		WD_ERR("Invalid align_size.\n");
		return -WD_EINVAL;
	}

	return WD_SUCCESS;
}

#define WD_UACCE_GRAN_SIZE		0x10000ull
static int wd_pool_pre_layout(struct wd_blkpool *p,
			      struct wd_blkpool_setup *sp)
{
	unsigned int asz;
	int ret;

	ret = pool_params_check(sp);
	if (ret)
		return ret;

	asz = sp->align_size;

	/* Get actual value by align */
	p->hd_sz = ALIGN(sizeof(struct wd_blk_hd), asz);
	p->blk_sz = ALIGN(sp->block_size, asz);
	if (p->size == 0 && !p->mem) {
		p->size = (p->hd_sz + p->blk_sz) *
			  (unsigned long)sp->block_num + asz;

		/* Make sure memory map granularity size align */
		if (wd_is_noiommu(p->ctx))
			p->size = ALIGN(p->size, WD_UACCE_GRAN_SIZE);
	}

	return WD_SUCCESS;
}

static void *wd_get_phys(struct wd_blkpool *pool, void *va)
{
	struct wd_ss_region *rgn;

	TAILQ_FOREACH(rgn, pool->ss_head, next) {
		if (rgn->va <= va && va < rgn->va + rgn->size)
			return (void *)(uintptr_t)(rgn->pa +
				((uintptr_t)va - (uintptr_t)rgn->va));
	}

	return NULL;
}

static int wd_pool_init(struct wd_blkpool *p)
{
	__u32 blk_size = p->setup.block_size;
	void *dma_start, *dma_end, *va;
	struct wd_blk_hd *hd = NULL;
	unsigned int dma_num = 0;
	unsigned int i, act_num;
	unsigned long loss;

	p->act_start = (void *)ALIGN((uintptr_t)p->mem,
				     p->setup.align_size);
	loss = p->act_start - p->mem;

	act_num = (p->size - loss) / (p->hd_sz + p->blk_sz);

	/* get dma address and initialize blocks */
	for (i = 0; i < act_num; i++) {
		va = (void *)((uintptr_t)p->act_start + p->hd_sz +
			      (unsigned long)(p->hd_sz +
			       p->blk_sz) * i);
		dma_start = wd_get_phys(p, va);
		dma_end = wd_get_phys(p, va + blk_size - 1);
		if (!dma_start || !dma_end) {
			WD_ERR("wd_get_phys err.\n");
			return -WD_ENOMEM;
		}

		if ((uintptr_t)dma_end - (uintptr_t)dma_start != blk_size - 1)
			continue;

		hd = (void *)((uintptr_t)va - p->hd_sz);
		hd->blk_dma = dma_start;
		hd->blk = va;
		hd->blk_tag = TAG_FREE;
		TAILQ_INSERT_TAIL(&p->head, hd, next);

		dma_num++;
	}

	p->free_blk_num = dma_num;
	p->blk_num = dma_num;

	return WD_SUCCESS;
}

static int usr_pool_init(struct wd_blkpool *p)
{
	struct wd_blkpool_setup *sp = &p->setup;
	__u32 blk_size = sp->block_size;
	struct wd_blk_hd *hd = NULL;
	unsigned long loss;
	unsigned int i, act_num;

	p->act_start = (void *)ALIGN((uintptr_t)p->mem,
				     sp->align_size);
	loss = p->act_start - p->mem;
	act_num = (p->size - loss) / (p->hd_sz + p->blk_sz);

	for (i = 0; i < act_num; i++) {
		hd = (void *)((uintptr_t)p->act_start + (p->hd_sz + p->blk_sz) * i);
		hd->blk = (void *)((uintptr_t)hd + p->hd_sz);
		hd->blk_dma = sp->br.iova_map(sp->br.usr, hd->blk, blk_size);
		if (!hd->blk_dma) {
			WD_ERR("failed to map usr blk.\n");
			return -WD_ENOMEM;
		}
		hd->blk_tag = TAG_FREE;
		TAILQ_INSERT_TAIL(&p->head, hd, next);
	}

	p->free_blk_num = act_num;
	p->blk_num = p->free_blk_num;

	return WD_SUCCESS;
}

static void drv_free_slice(struct wd_blkpool *p)
{
	struct wd_ss_region *rgn;

	while (true) {
		rgn = TAILQ_FIRST(&p->ss_list);
		if (!rgn)
			break;
		TAILQ_REMOVE(&p->ss_list, rgn, next);
		free(rgn);
	}
}

static void drv_add_slice(struct wd_blkpool *p, struct wd_ss_region *rgn)
{
	struct wd_ss_region *rg;

	rg = TAILQ_LAST(&p->ss_list, wd_ss_region_list);
	if (rg) {
		if (rg->pa + rg->size == rgn->pa) {
			rg->size += rgn->size;
			free(rgn);
			return;
		}
	}

	TAILQ_INSERT_TAIL(&p->ss_list, rgn, next);
}

#define WD_UACCE_GRAN_SHIFT		16
#define WD_UACCE_GRAN_NUM_MASK		0xfffull
static void *pool_reserve_mem(struct wd_blkpool *p, size_t size)
{
	struct wd_ss_region *rgn = NULL;
	unsigned long info = 0;
	size_t tmp = 0;
	unsigned long i = 0;
	void *ptr = NULL;
	int ret = 1;

	if (!p->ctx)
		return NULL;

	if (p->mem)
		return NULL;

	ptr = wd_reserve_mem(p->ctx, size);
	if (!ptr)
		return NULL;

	p->ss_head = &p->ss_list;
	TAILQ_INIT(&p->ss_list);

	while (ret > 0) {
		info = i;
		ret = wd_ctx_set_io_cmd(p->ctx, UACCE_CMD_GET_SS_DMA, &info);
		if (ret < 0) {
			WD_ERR("get DMA fail!\n");
			goto err_out;
		}
		rgn = malloc(sizeof(*rgn));
		if (!rgn) {
			WD_ERR("alloc ss region fail!\n");
			goto err_out;
		}
		memset(rgn, 0, sizeof(*rgn));

		if (wd_is_noiommu(p->ctx))
			rgn->size = (info & WD_UACCE_GRAN_NUM_MASK) <<
				WD_UACCE_GRAN_SHIFT;
		else
			rgn->size = p->size;
		rgn->pa = info & (~WD_UACCE_GRAN_NUM_MASK);
		rgn->va = ptr + tmp;
		tmp += rgn->size;
		drv_add_slice(p, rgn);
		i++;
	}

	return ptr;

err_out:
	drv_free_slice(p);
	munmap(p->mem, size);

	return NULL;
}

static int pool_init(struct wd_blkpool *pool,
		       struct wd_blkpool_setup *setup)
{
	void *addr = NULL;

	/* use user's memory, and its br alloc function */
	if (setup->br.alloc && setup->br.free) {
		if (!pool->mem) {
			addr = setup->br.alloc(setup->br.usr, pool->size);
			if (!addr) {
				WD_ERR("failed to allocate memory in user pool.\n");
				return -EINVAL;
			}
			pool->mem = addr;
		}
		if (usr_pool_init(pool)) {
			WD_ERR("failed to initialize user pool.\n");
			setup->br.free(setup->br.usr, addr);
			return -EINVAL;
		}
	} else {
		if (!pool->mem) {
			/* use wd to reserve memory */
			addr = pool_reserve_mem(pool, pool->size);
			if (!addr) {
				WD_ERR("wd pool failed to reserve memory.\n");
				return -EINVAL;
			}
			pool->mem = addr;
		}

		if (wd_pool_init(pool)) {
			WD_ERR("failed to initialize wd pool.\n");
			wd_blkpool_destroy_mem(pool);
			return -EINVAL;
		}
	}

	return 0;
}

void *wd_blkpool_new(handle_t h_ctx)
{
	struct wd_blkpool *pool;

	if(wd_is_sva(h_ctx))
		return NULL;

	pool = calloc(1, sizeof(*pool));
	if (!pool) {
		WD_ERR("failed to malloc pool.\n");
		return NULL;
	}
	pool->ctx = h_ctx;

	return pool;
}

int wd_blkpool_setup(void *pool, struct wd_blkpool_setup *setup)
{
	struct wd_blkpool *p = pool;
	int ret = 0;

	if (!p || !setup)
		return -EINVAL;

	wd_blkpool_spinlock(&p->pool_lock);
	if (p->mem && p->size != 0)
		if (p->blk_sz == ALIGN(setup->block_size, setup->align_size)) {
			goto out;

		/* re-org blk_size, no need reserve mem */
		if (p->free_blk_num != p->blk_num) {
			WD_ERR("Can not reset blk pool, as it's in use.\n");
			ret = -EINVAL;
			goto out;
		}
	}

	memcpy(&p->setup, setup, sizeof(p->setup));

	ret = wd_pool_pre_layout(p, setup);
	if (ret)
		goto out;

	TAILQ_INIT(&p->head);

	ret = pool_init(p, setup);

out:
	wd_blkpool_unspinlock(&p->pool_lock);
	return ret;
}

void *wd_blkpool_alloc(void *pool, size_t size)
{
	struct wd_blkpool *p = pool;
	struct wd_blk_hd *hd;
	int ret;

	if (unlikely(!p)) {
		WD_ERR("blk alloc pool is null!\n");
		return NULL;
	}

	if (!p->mem || size > p->blk_sz) {
		struct wd_blkpool_setup setup;
		/*
		 * if empty pool, will reserve mem and init pool
		 * if size > blk_size, will re-org as align 4K if free pool
		 */

		memset(&setup, 0, sizeof(setup));
		setup.block_size = ALIGN(size, DEFAULT_BLK_ALIGN);
		setup.block_num = DEFAULT_BLOCK_NM;
		setup.align_size = DEFAULT_ALIGN_SIZE;
		ret = wd_blkpool_setup(p, &setup);
		if (ret)
			return NULL;
	}

	wd_blkpool_spinlock(&p->pool_lock);
	hd = TAILQ_LAST(&p->head, wd_blk_list);
	if (unlikely(!hd || hd->blk_tag != TAG_FREE)) {
		p->alloc_failures++;
		goto out;
	}

	/* Delete the block buffer from free list */
	TAILQ_REMOVE(&p->head, hd, next);
	p->free_blk_num--;
	hd->blk_tag = TAG_USED;
	wd_blkpool_unspinlock(&p->pool_lock);

	return hd->blk;

out:
	wd_blkpool_unspinlock(&p->pool_lock);
	WD_ERR("Failed to malloc blk.\n");

	return NULL;
}

void wd_blkpool_free(void *pool, void *va)
{
	struct wd_blkpool *p = pool;
	struct wd_blk_hd *hd;

	if (unlikely(!p || !va)) {
		WD_ERR("free blk parameters err!\n");
		return;
	}

	hd = wd_blk_head(p, va);
	if (unlikely(hd->blk_tag != TAG_USED)) {
		WD_ERR("free block fail!\n");
		return;
	}

	wd_blkpool_spinlock(&p->pool_lock);
	TAILQ_INSERT_TAIL(&p->head, hd, next);
	p->free_blk_num++;
	hd->blk_tag = TAG_FREE;
	wd_blkpool_unspinlock(&p->pool_lock);
}

void *wd_blkpool_phys(void *pool, void *va, size_t sz)
{
	struct wd_blk_hd *hd;

	if (unlikely(!pool || !va)) {
		WD_ERR("blk map err, pool is NULL!\n");
		return NULL;
	}

	hd = wd_blk_head(pool, va);
	if (unlikely(hd->blk_tag != TAG_USED ||
	    (uintptr_t)va < (uintptr_t)hd->blk)) {
		WD_ERR("dma map fail!\n");
		return NULL;
	}

	return (void *)((uintptr_t)hd->blk_dma + ((uintptr_t)va -
			(uintptr_t)hd->blk));
}

int wd_blkpool_get_free_blk_num(void *pool, __u32 *free_num)
{
	struct wd_blkpool *p = pool;

	if (!p || !free_num) {
		WD_ERR("get_free_blk_num err, parameter err!\n");
		return -WD_EINVAL;
	}

	*free_num = __atomic_load_n(&p->free_blk_num, __ATOMIC_RELAXED);

	return WD_SUCCESS;
}

int wd_blkpool_alloc_failures(void *pool, __u32 *fail_num)
{
	struct wd_blkpool *p = pool;

	if (!p || !fail_num) {
		WD_ERR("get_blk_alloc_failure err, pool is NULL!\n");
		return -WD_EINVAL;
	}

	*fail_num = __atomic_load_n(&p->alloc_failures, __ATOMIC_RELAXED);

	return WD_SUCCESS;
}

__u32 wd_blkpool_blksize(void *pool)
{
	struct wd_blkpool *p = pool;

	if (!p) {
		WD_ERR("get blk_size pool is null!\n");
		return 0;
	}

	return p->blk_sz;
}

void wd_blkpool_destroy_mem(void *pool)
{
	struct wd_blkpool_setup *setup;
	struct wd_blkpool *p = pool;

	if (!p) {
		WD_ERR("pool destroy err, pool is NULL.\n");
		return;
	}

	wd_blkpool_spinlock(&p->pool_lock);
	if (p->mem) {
		setup = &p->setup;
		if (setup->br.free) {
			setup->br.free(setup->br.usr, p->mem);
		} else {
			drv_free_slice(p);
			munmap(p->mem, p->size);
		}
		p->mem = NULL;
		p->size = 0;
	}
	wd_blkpool_unspinlock(&p->pool_lock);
}

