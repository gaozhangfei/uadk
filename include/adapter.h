/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright 2023-2024 Huawei Technologies Co.,Ltd. All rights reserved.
 * Copyright 2023-2024 Linaro ltd.
 */

#ifndef __ADAPTER_H
#define __ADAPTER_H

#include "wd_alg.h"

#ifndef UADK_MAX_NB_WORKERS
#define UADK_MAX_NB_WORKERS  (2)
#endif

enum uadk_adapter_mode {
	UADK_ADAPT_MODE_PRIMARY,
	UADK_ADAPT_MODE_ROUNDROBIN,
};

struct uadk_adapter_ops {
	struct uadk_adapter_worker *(*choose_worker)(struct uadk_adapter *adapter);
	void (*switch_worker)(struct uadk_adapter *adapter, );
};

struct uadk_adapter_worker {
	struct wd_alg_driver *driver;
	struct wd_ctx_config_internal config;
	struct wd_sched sched;
	struct wd_async_msg_pool pool;
	bool valid;
};

struct uadk_adapter {
	unsigned int workers_nb;
	enum uadk_adapter_mode mode;
	struct uadk_adapter_worker workers[UADK_MAX_NB_WORKERS];
	struct uadk_adapter_ops ops;
};

struct wd_alg_driver *uadk_adapter_alloc(void);
void uadk_adapter_free(struct wd_alg_driver *adapter);
int uadk_adapter_set_mode(struct wd_alg_driver *adapter, enum uadk_adapter_mode mode);
int uadk_adapter_config(struct wd_alg_driver *adapter, void *cfg);
int uadk_adapter_attach_worker(struct wd_alg_driver *adapter,
			       struct wd_alg_driver *drv, void *dlhandle);
int uadk_adapter_parse(struct wd_alg_driver *adapter, char *lib_path,
		       char *drv_name, char *alg_name);
#endif
