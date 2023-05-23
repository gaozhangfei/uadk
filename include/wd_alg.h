// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright 2023 Huawei Technologies Co.,Ltd. All rights reserved.
 */

#ifndef __WD_ALG_H
#define __WD_ALG_H
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <asm/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define handle_t uintptr_t
enum alg_priority {
	UADK_ALG_SOFT = 0x0,
	UADK_ALG_CE_INSTR = 0x1,
	UADK_ALG_SVE_INSTR = 0x2,
	UADK_ALG_HW = 0x3
};

/**
 * @drv_name: name of the current device driver
 * @alg_name: name of the algorithm supported by the driver
 * @priority: priority of the type of algorithm supported by the driver
 * @queue_num: number of device queues required by the device to
 *		 execute the algorithm task
 * @op_type_num: number of modes in which the device executes the
 *		 algorithm business and requires queues to be executed separately
 * @priv: pointer of priv ctx
 * @fallback: soft calculation driver handle when performing soft
 *		 calculation supplement
 * @init: callback interface for initializing device drivers
 * @exit: callback interface for destroying device drivers
 * @send: callback interface used to send task packets to
 *	    hardware devices.
 * @recv: callback interface used to retrieve the calculation
 *	    result of the task   packets from the hardware device.
 * @get_usage: callback interface used to obtain the
 *	    utilization rate of devices.
 */
struct wd_alg_driver {
	const char	*drv_name;
	const char	*alg_name;
	int	priority;
	int	queue_num;
	int	op_type_num;
	void	*priv;
	handle_t fallback;

	int (*init)(struct wd_alg_driver *drv, void *conf);
	void (*exit)(struct wd_alg_driver *drv);
	int (*send)(struct wd_alg_driver *drv, handle_t ctx, void *drv_msg);
	int (*recv)(struct wd_alg_driver *drv, handle_t ctx, void *drv_msg);
	int (*get_usage)(void *param);
};

inline int wd_alg_driver_init(struct wd_alg_driver *drv, void *conf)
{
	return drv->init(drv, conf);
}

inline void wd_alg_driver_exit(struct wd_alg_driver *drv)
{
	drv->exit(drv);
}

inline int wd_alg_driver_send(struct wd_alg_driver *drv, handle_t ctx, void *msg)
{
	return drv->send(drv, ctx, msg);
}

inline int wd_alg_driver_recv(struct wd_alg_driver *drv, handle_t ctx, void *msg)
{
	return drv->recv(drv, ctx, msg);
}

int wd_alg_driver_register(struct wd_alg_driver *drv);
void wd_alg_driver_unregister(struct wd_alg_driver *drv);

struct wd_alg_list {
	const char	*alg_name;
	const char	*drv_name;
	bool available;
	int	priority;
	int	refcnt;

	struct wd_alg_driver *drv;
	struct wd_alg_list *next;
};

struct wd_alg_driver *wd_request_drv(const char	*alg_name, bool hw_mask);
void wd_release_drv(struct wd_alg_driver *drv);

bool wd_drv_alg_support(const char *alg_name,
	struct wd_alg_driver *drv);
void wd_enable_drv(struct wd_alg_driver *drv);
void wd_disable_drv(struct wd_alg_driver *drv);

struct wd_alg_list *wd_get_alg_head(void);
struct wd_alg_driver *wd_find_drv(char *drv_name, char *alg_name, int idx);
#ifdef __cplusplus
}
#endif

#endif
