/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright 2020-2021 Huawei Technologies Co.,Ltd. All rights reserved.
 * Copyright 2020-2021 Linaro ltd.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <numa.h>
#include <sched.h>

#include "wd.h"
#include "wd_alg_common.h"

#define SYS_CLASS_DIR			"/sys/class/uacce"

const char *WD_VERSION = UADK_VERSION_NUMBER;

struct wd_ctx_h {
	int fd;
	char dev_path[MAX_DEV_NAME_LEN];
	char *dev_name;
	char *drv_name;
	unsigned long qfrs_offs[UACCE_QFRT_MAX];
	void *qfrs_base[UACCE_QFRT_MAX];
	struct uacce_dev *dev;
	void *priv;
};

static int get_raw_attr(const char *dev_root, const char *attr, char *buf,
			size_t sz)
{
	char attr_file[PATH_STR_SIZE];
	char attr_path[PATH_MAX];
	char *ptrRet = NULL;
	ssize_t size;
	int fd;

	if (!dev_root || !attr || !buf || !sz)
		return -WD_EINVAL;

	size = snprintf(attr_file, PATH_STR_SIZE, "%s/%s", dev_root, attr);
	if (size < 0)
		return -WD_EINVAL;

	ptrRet = realpath(attr_file, attr_path);
	if (ptrRet == NULL)
		return -WD_ENODEV;

	fd = open(attr_path, O_RDONLY, 0);
	if (fd < 0) {
		WD_ERR("open %s fail (%d)!\n", attr_path, -errno);
		return -WD_ENODEV;
	}

	memset(buf, 0, sz);
	size = read(fd, buf, sz);
	if (size <= 0) {
		WD_ERR("read nothing at %s!\n", attr_path);
		size = -WD_EIO;
	}

	close(fd);

	return size;
}

static int get_int_attr(struct uacce_dev *dev, const char *attr, int *val)
{
	char buf[MAX_ATTR_STR_SIZE] = {0};
	int ret;

	if (!dev || !val)
		return -WD_EINVAL;

	ret = get_raw_attr(dev->dev_root, attr, buf, MAX_ATTR_STR_SIZE - 1);
	if (ret < 0)
		return ret;

	*val = strtol(buf, NULL, 10);
	if (errno == ERANGE)
		return -errno;

	return 0;
}

static int get_str_attr(struct uacce_dev *dev, const char *attr, char *buf,
			size_t buf_sz)
{
	int ret;

	if (!dev)
		return -WD_EINVAL;

	ret = get_raw_attr(dev->dev_root, attr, buf, buf_sz);
	if (ret < 0) {
		buf[0] = '\0';
		return ret;
	}

	if (ret == buf_sz)
		ret--;

	buf[ret] = '\0';
	while ((ret > 1) && (buf[ret - 1] == '\n')) {
		buf[ret-- - 1] = '\0';
	}

	return ret;
}

static int access_attr(const char *dev_root, const char *attr, int mode)
{
	char attr_file[PATH_STR_SIZE];
	ssize_t size;

	if (!dev_root || !attr)
		return -WD_EINVAL;

	size = snprintf(attr_file, PATH_STR_SIZE, "%s/%s", dev_root, attr);
	if (size < 0)
		return -WD_EINVAL;

	return access(attr_file, mode);
}

static int get_dev_info(struct uacce_dev *dev)
{
	int value = 0;
	int ret;

	get_int_attr(dev, "flags", &dev->flags);
	get_str_attr(dev, "api", dev->api, WD_NAME_SIZE);

	/* hardware err isolate flag */
	ret = access_attr(dev->dev_root, "isolate", F_OK);
	if (!ret) {
		get_int_attr(dev, "isolate", &value);
		if (value == 1)
			return -ENODEV;
	}

	get_str_attr(dev, "algorithms", dev->algs, MAX_ATTR_STR_SIZE);
	get_int_attr(dev, "region_mmio_size", &value);
	dev->qfrs_offs[UACCE_QFRT_MMIO] = value;
	get_int_attr(dev, "region_dus_size", &value);
	dev->qfrs_offs[UACCE_QFRT_DUS] = value;
	get_int_attr(dev, "device/numa_node", &dev->numa_id);

	return 0;
}

static struct uacce_dev *read_uacce_sysfs(const char *dev_name)
{
	struct uacce_dev *dev = NULL;
	struct dirent *dev_dir = NULL;
	DIR *class = NULL;
	char *name = NULL;
	int ret;

	if (!dev_name)
		return NULL;

	dev = calloc(1, sizeof(struct uacce_dev));
	if (!dev)
		return NULL;

	class = opendir(SYS_CLASS_DIR);
	if (!class) {
		WD_ERR("WD framework is not enabled on the system!\n");
		goto out;
	}

	while ((dev_dir = readdir(class)) != NULL) {
		name = dev_dir->d_name;
		if (strncmp(dev_name, name, strlen(dev_name)))
			continue;

		ret = snprintf(dev->dev_root, MAX_DEV_NAME_LEN, "%s/%s",
			       SYS_CLASS_DIR, dev_name);
		if (ret < 0)
			goto out_dir;

		ret = snprintf(dev->char_dev_path, MAX_DEV_NAME_LEN,
			       "/dev/%s", dev_name);
		if (ret < 0)
			goto out_dir;

		ret = get_dev_info(dev);
		if (ret)
			goto out_dir;
		break;
	}
	if (!dev_dir)
		goto out_dir;

	closedir(class);

	return dev;

out_dir:
	closedir(class);
out:
	free(dev);
	return NULL;
}

char *wd_get_accel_name(char *dev_path, int no_apdx)
{
	int i, appendix, dash_len, len;
	char *dash = NULL;
	char *name;

	if (!dev_path || (no_apdx != 0 && no_apdx != 1))
		return NULL;

	if (!index(dev_path, '-'))
		return NULL;

	/* find '/' index in the string and keep the last level */
	name = rindex(dev_path, '/');
	if (name) {
		/* absolute path */
		if (strlen(name) == 1)
			return NULL;
		name++;
	} else {
		/* relative path */
		name = dev_path;
	}

	if (!strlen(name))
		return NULL;

	if (no_apdx) {
		/* find '-' index in the name string */
		appendix = 1;
		dash = rindex(name, '-');
		if (!dash)
			goto out;

		dash_len = strlen(dash);
		for (i = 1; i < dash_len; i++) {
			if (!isdigit(dash[i])) {
				appendix = 0;
				break;
			}
		}
		/* treat dash as a part of name if there's no digit */
		if (i == 1)
			appendix = 0;
	} else {
		appendix = 0;
	}

out:
	/* remove '-' and digits */
	len = (dash && appendix) ? strlen(name) - strlen(dash) : strlen(name);

	return strndup(name, len);
}

static struct uacce_dev *clone_uacce_dev(struct uacce_dev *dev)
{
	struct uacce_dev *new;

	new = calloc(1, sizeof(*dev));
	if (!new)
		return NULL;

	memcpy(new, dev, sizeof(*dev));

	return new;
}

static void free_uacce_dev(struct uacce_dev *dev)
{
	free(dev);
}

static void wd_ctx_init_qfrs_offs(struct wd_ctx_h *ctx)
{
	memcpy(&ctx->qfrs_offs, &ctx->dev->qfrs_offs,
	       sizeof(ctx->qfrs_offs));
}

handle_t wd_request_ctx(struct uacce_dev *dev)
{
	struct wd_ctx_h	*ctx;
	char char_dev_path[PATH_MAX];
	char *ptrRet = NULL;

	if (!dev || !strlen(dev->dev_root))
		return 0;

	ptrRet = realpath(dev->char_dev_path, char_dev_path);
	if (ptrRet == NULL)
		return 0;

	ctx = calloc(1, sizeof(struct wd_ctx_h));
	if (!ctx)
		return 0;

	ctx->dev_name = wd_get_accel_name(char_dev_path, 0);
	if (!ctx->dev_name)
		goto free_ctx;

	ctx->drv_name = wd_get_accel_name(char_dev_path, 1);
	if (!ctx->drv_name)
		goto free_dev_name;

	ctx->dev = clone_uacce_dev(dev);
	if (!ctx->dev)
		goto free_drv_name;

	wd_ctx_init_qfrs_offs(ctx);

	strncpy(ctx->dev_path, char_dev_path, MAX_DEV_NAME_LEN);
	ctx->dev_path[MAX_DEV_NAME_LEN - 1] = '\0';

	ctx->fd = open(char_dev_path, O_RDWR | O_CLOEXEC);
	if (ctx->fd < 0) {
		WD_ERR("failed to open %s (%d).\n", char_dev_path, -errno);
		goto free_dev;
	}

	return (handle_t)ctx;

free_dev:
	free_uacce_dev(ctx->dev);
free_drv_name:
	free(ctx->drv_name);
free_dev_name:
	free(ctx->dev_name);
free_ctx:
	free(ctx);
	return 0;
}

void wd_release_ctx(handle_t h_ctx)
{
	struct wd_ctx_h	*ctx = (struct wd_ctx_h *)h_ctx;

	if (!ctx)
		return;

	close(ctx->fd);
	free(ctx->dev);
	free(ctx->drv_name);
	free(ctx->dev_name);
	free(ctx);
}

int wd_ctx_start(handle_t h_ctx)
{
	struct wd_ctx_h	*ctx = (struct wd_ctx_h *)h_ctx;
	int ret;

	if (!ctx)
		return -WD_EINVAL;

	ret = wd_ctx_set_io_cmd(h_ctx, UACCE_CMD_START, NULL);
	if (ret)
		WD_ERR("Fail to start on %s (%d).\n", ctx->dev_path, -errno);

	return ret;
}

int wd_release_ctx_force(handle_t h_ctx)
{
	struct wd_ctx_h	*ctx = (struct wd_ctx_h *)h_ctx;
	int ret;

	if (!ctx)
		return -WD_EINVAL;

	ret = wd_ctx_set_io_cmd(h_ctx, UACCE_CMD_PUT_Q, NULL);
	if (ret)
		WD_ERR("Fail to stop on %s (%d).\n", ctx->dev_path, -errno);

	return ret;
}

void *wd_ctx_mmap_qfr(handle_t h_ctx, enum uacce_qfrt qfrt)
{
	struct wd_ctx_h	*ctx = (struct wd_ctx_h *)h_ctx;
	off_t off = qfrt * getpagesize();
	size_t size;
	void *addr;

	if (!ctx || qfrt >= UACCE_QFRT_MAX || !ctx->qfrs_offs[qfrt])
		return NULL;

	size = ctx->qfrs_offs[qfrt];

	addr = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, ctx->fd, off);
	if (addr == MAP_FAILED)
		return NULL;

	ctx->qfrs_base[qfrt] = addr;

	return addr;
}

void wd_ctx_unmap_qfr(handle_t h_ctx, enum uacce_qfrt qfrt)
{
	struct wd_ctx_h	*ctx = (struct wd_ctx_h *)h_ctx;

	if (!ctx || qfrt >= UACCE_QFRT_MAX)
		return;

	if (ctx->qfrs_offs[qfrt] != 0)
		munmap(ctx->qfrs_base[qfrt], ctx->qfrs_offs[qfrt]);
}

unsigned long wd_ctx_get_region_size(handle_t h_ctx, enum uacce_qfrt qfrt)
{
	struct wd_ctx_h *ctx = (struct wd_ctx_h *)h_ctx;
	if (!ctx || qfrt >= UACCE_QFRT_MAX)
			return 0;
	return ctx->qfrs_offs[qfrt];
}

void *wd_ctx_get_priv(handle_t h_ctx)
{
	struct wd_ctx_h	*ctx = (struct wd_ctx_h *)h_ctx;

	if (!ctx)
		return NULL;

	return ctx->priv;
}

int wd_ctx_set_priv(handle_t h_ctx, void *priv)
{
	struct wd_ctx_h	*ctx = (struct wd_ctx_h *)h_ctx;

	if (!ctx)
		return -WD_EINVAL;

	ctx->priv = priv;

	return 0;
}

char *wd_ctx_get_api(handle_t h_ctx)
{
	struct wd_ctx_h	*ctx = (struct wd_ctx_h *)h_ctx;

	if (!ctx)
		return NULL;

	return ctx->dev->api;
}

int wd_ctx_wait(handle_t h_ctx, __u16 ms)
{
	struct wd_ctx_h	*ctx = (struct wd_ctx_h *)h_ctx;
	struct pollfd fds[1];
	int ret;

	if (!ctx)
		return -WD_EINVAL;

	fds[0].fd = ctx->fd;
	fds[0].events = POLLIN;
	ret = poll(fds, 1, ms);
	if (ret == -1)
		return -errno;

	return ret;
}

int wd_is_sva(handle_t h_ctx)
{
	struct wd_ctx_h	*ctx = (struct wd_ctx_h *)h_ctx;

	if (!ctx)
		return -WD_EINVAL;

	if ((unsigned int)ctx->dev->flags & UACCE_DEV_SVA)
		return 1;

	return 0;
}

int wd_get_numa_id(handle_t h_ctx)
{
	struct wd_ctx_h	*ctx = (struct wd_ctx_h *)h_ctx;

	if (!ctx)
		return -WD_EINVAL;

	return ctx->dev->numa_id;
}

int wd_get_avail_ctx(struct uacce_dev *dev)
{
	int avail_ctx, ret;

	ret = get_int_attr(dev, "available_instances", &avail_ctx);
	if (ret < 0)
		return ret;

	return avail_ctx;
}

static int get_dev_alg_name(const char *d_name, char *dev_alg_name, size_t sz)
{
	char dev_path[MAX_DEV_NAME_LEN] = {0};
	int ret;

	ret = snprintf(dev_path, MAX_DEV_NAME_LEN, "%s/%s",
		       SYS_CLASS_DIR, d_name);
	if (ret < 0)
		return ret;
	else if (ret > MAX_DEV_NAME_LEN)
		return -WD_EINVAL;

	ret = get_raw_attr(dev_path, "algorithms", dev_alg_name, sz);
	if (ret < 0) {
		dev_alg_name[0] = '\0';
		WD_ERR("failed to get alg for %s, ret = %d\n",
		       dev_path, ret);
		return ret;
	}

	if (ret == sz)
		dev_alg_name[sz - 1] = '\0';

	return 0;
}

static bool dev_has_alg(const char *dev_alg_name, const char *alg_name)
{
	char *str;

	str = strstr(dev_alg_name, alg_name);
	if (!str)
		return false;

	if (*(str + strlen(alg_name)) == '\n' &&
	    ((str > dev_alg_name && *(str - 1) == '\n') || str == dev_alg_name))
		return true;

	return false;
}

static void add_uacce_dev_to_list(struct uacce_dev_list *head,
				  struct uacce_dev_list *node)
{
	struct uacce_dev_list *tmp = head;

	while (tmp->next)
		tmp = tmp->next;

	tmp->next = node;
}

static int check_alg_name(const char *alg_name)
{
	int i = 0;

	if (!alg_name)
		return -WD_EINVAL;

	while (alg_name[i] != '\0') {
		i++;
		if (i >= MAX_ATTR_STR_SIZE) {
			WD_ERR("get list failed, alg name is too long!\n");
			return -WD_EINVAL;
		}
	}

	return 0;
}

struct uacce_dev_list *wd_get_accel_list(const char *alg_name)
{
	struct uacce_dev_list *node, *head = NULL;
	char dev_alg_name[MAX_ATTR_STR_SIZE];
	struct dirent *dev_dir;
	DIR *wd_class;
	int ret;

	if (check_alg_name(alg_name))
		return NULL;

	wd_class = opendir(SYS_CLASS_DIR);
	if (!wd_class) {
		WD_ERR("UADK framework isn't enabled in system!\n");
		return NULL;
	}

	while ((dev_dir = readdir(wd_class)) != NULL) {
		if (!strncmp(dev_dir->d_name, ".", LINUX_CRTDIR_SIZE) ||
		    !strncmp(dev_dir->d_name, "..", LINUX_PRTDIR_SIZE))
			continue;

		ret = get_dev_alg_name(dev_dir->d_name, dev_alg_name,
				       sizeof(dev_alg_name));
		if (ret < 0)
			goto free_list;

		if (!dev_has_alg(dev_alg_name, alg_name))
			continue;

		node = calloc(1, sizeof(*node));
		if (!node)
			goto free_list;

		node->dev = read_uacce_sysfs(dev_dir->d_name);
		if (!node->dev) {
			free(node);
			node = NULL;
			continue;
		}

		if (!head)
			head = node;
		else
			add_uacce_dev_to_list(head, node);
	}

	closedir(wd_class);

	return head;

free_list:
	closedir(wd_class);
	wd_free_list_accels(head);
	return NULL;
}

void wd_free_list_accels(struct uacce_dev_list *list)
{
	struct uacce_dev_list *curr, *next;

	if (!list)
		return;

	curr = list;
	while (curr) {
		next = curr->next;
		if (curr->dev)
			free(curr->dev);
		free(curr);
		curr = next;
	}
}

struct uacce_dev *wd_get_accel_dev(const char *alg_name)
{
	struct uacce_dev_list *list, *head;
	struct uacce_dev *dev = NULL, *target = NULL;
	int cpu = sched_getcpu();
	int node = numa_node_of_cpu(cpu);
	int ctx_num, tmp;
	int dis = 1024;
	int max = 0;

	head = wd_get_accel_list(alg_name);
	if (!head)
		return NULL;

	list = head;
	while (list) {
		tmp = numa_distance(node, list->dev->numa_id);
		ctx_num = wd_get_avail_ctx(list->dev);
		if ((dis > tmp && ctx_num) || (dis == tmp && ctx_num > max)) {
			dev = list->dev;
			dis = tmp;
			max = ctx_num;
		}

		list = list->next;
	}

	if (dev)
		target = clone_uacce_dev(dev);

	wd_free_list_accels(head);

	return target;
}

int wd_ctx_set_io_cmd(handle_t h_ctx, unsigned long cmd, void *arg)
{
	struct wd_ctx_h	*ctx = (struct wd_ctx_h *)h_ctx;

	if (!ctx)
		return -WD_EINVAL;

	if (!arg)
		return ioctl(ctx->fd, cmd);

	return ioctl(ctx->fd, cmd, arg);
}
