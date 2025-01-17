/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2020-2021 Huawei Technologies Co.,Ltd. All rights reserved. */

#ifndef __WD_CIPHER_DRV_H
#define __WD_CIPHER_DRV_H

#include "../wd_cipher.h"
#include "../wd_alg_common.h"

/* fixme wd_cipher_msg */
struct wd_cipher_msg {
	struct wd_cipher_req req;
	/* request identifier */
	__u32 tag;
	/* Denoted by enum wcrypto_type */
	__u8 alg_type;
	/* Denoted by enum wcrypto_cipher_type */
	__u8 alg;
	/* Denoted by enum wcrypto_cipher_op_type */
	__u8 op_type;
	/* Denoted by enum wcrypto_cipher_mode_type */
	__u8 mode;
	/* Data format, include pbuffer and sgl */
	__u8 data_fmt;
	/* Operation result, denoted by WD error code */
	__u8 result;
	/* epoll flag */
	__u8 is_polled;

	/* Key bytes */
	__u16 key_bytes;
	/* iv bytes */
	__u16 iv_bytes;
	/* in bytes */
	__u32 in_bytes;
	/* out_bytes */
	__u32 out_bytes;

	/* input key pointer */
	__u8 *key;
	/* input iv pointer */
	__u8 *iv;
	/* input data pointer */
	__u8 *in;
	/* output data pointer */
	__u8 *out;
};

struct wd_cipher_driver {
	const char	*drv_name;
	const char	*alg_name;
	__u32	drv_ctx_size;
	int	(*init)(struct wd_ctx_config_internal *config, void *priv);
	void	(*exit)(void *priv);
	int	(*cipher_send)(handle_t ctx, struct wd_cipher_msg *msg);
	int	(*cipher_recv)(handle_t ctx, struct wd_cipher_msg *msg);
};

void wd_cipher_set_driver(struct wd_cipher_driver *drv);
struct wd_cipher_driver *wd_cipher_get_driver(void);

#ifdef WD_STATIC_DRV
#define WD_CIPHER_SET_DRIVER(drv)					      \
struct wd_cipher_driver *wd_cipher_get_driver(void)			      \
{									      \
	return &drv;							      \
}
#else
#define WD_CIPHER_SET_DRIVER(drv)				              \
static void __attribute__((constructor)) set_driver(void)		      \
{									      \
	wd_cipher_set_driver(&drv);					      \
}
#endif
#endif /* __WD_CIPHER_DRV_H */
