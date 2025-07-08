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

#ifndef _WD_BMM_H
#define _WD_BMM_H

#include <asm/types.h>
#include "wd.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEFAULT_BLK_ALIGN 0x1000
#define DEFAULT_BLOCK_NM 16384
#define DEFAULT_ALIGN_SIZE 0x40

/* memory APIs for Algorithm Layer */
typedef void *(*wd_alloc)(void *usr, size_t size);
typedef void (*wd_free)(void *usr, void *va);

 /* memory VA to DMA address map */
typedef void *(*wd_map)(void *usr, void *va, size_t sz);
typedef __u32 (*wd_bufsize)(void *usr);

/* Memory from user, it is given at ctx creating. */
struct wd_mm_br {
	wd_alloc alloc; /* Memory allocation */
	wd_free free; /* Memory free */
	wd_map iova_map; /* get iova from user space VA */
	void *usr; /* data for the above operations */
	wd_bufsize get_bufsize; /* optional */
};

enum wd_blkpool_flag {
	WD_BLKPOOL_FLAT_MEMCPY = 1,
	WD_BLKPOOL_FLAT_USER,
	WD_BLKPOOL_FLAG_MAX,
};

/* Memory pool creating parameters */
struct wd_blkpool_setup {
	__u32 block_size;	/* Block buffer size */
	__u32 block_num;	/* Block buffer number */
	__u32 align_size;	/* Block buffer starting address align size */
	enum wd_blkpool_flag flag;
	struct wd_mm_br br;	/* memory from user if don't use WD memory */
};


void *wd_blkpool_new(handle_t h_ctx);
int wd_blkpool_setup(void *pool, struct wd_blkpool_setup *setup);
void wd_blkpool_destroy_mem(void *pool);
void *wd_blkpool_alloc(void *pool, size_t size);
void wd_blkpool_free(void *pool, void *va);
void *wd_blkpool_phys(void *pool, void *va, size_t sz);

#ifdef __cplusplus
}
#endif

#endif
