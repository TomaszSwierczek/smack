/*
 * This file is part of libsmack
 *
 * Copyright (C) 2010, 2011 Nokia Corporation
 * Copyright (C) 2011, 2012, 2013 Intel Corporation
 * Copyright (C) 2012, 2013 Samsung Electronics Co.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#include "sys/smack.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/xattr.h>

#define SELF_LABEL_FILE "/proc/self/attr/current"

#define SHORT_LABEL_LEN 23
#define ACC_LEN 6
#define LOAD_LEN (2 * (SMACK_LABEL_LEN + 1) + 2 * ACC_LEN + 1)
#define KERNEL_LONG_FORMAT "%s %s %s"
#define KERNEL_SHORT_FORMAT "%-23s %-23s %5.5s"
#define KERNEL_MODIFY_FORMAT "%s %s %s %s"

#define LEVEL_MAX 255
#define NUM_LEN 4
#define BUF_SIZE 512
#define CAT_MAX_COUNT 240
#define CAT_MAX_VALUE 63
#define CIPSO_POS(i)   (SMACK_LABEL_LEN + 1 + NUM_LEN + NUM_LEN + i * NUM_LEN)
#define CIPSO_MAX_SIZE CIPSO_POS(CAT_MAX_COUNT)
#define CIPSO_NUM_LEN_STR "%-4d"

#define ACCESS_TYPE_R 0x01
#define ACCESS_TYPE_W 0x02
#define ACCESS_TYPE_X 0x04
#define ACCESS_TYPE_A 0x08
#define ACCESS_TYPE_T 0x10
#define ACCESS_TYPE_L 0x20

#define ACCESS_TYPE_ALL ((1 << ACC_LEN) - 1)

#define DICT_HASH_SIZE 4096
#define MAX_LABELS_CNT (UINT16_MAX + 1)

extern char *smackfs_mnt;
extern int smackfs_mnt_dirfd;

extern int init_smackfs_mnt(void);

struct smack_rule {
	int8_t allow_code;
	int8_t deny_code;
	uint16_t object_id;
	struct smack_rule *next_rule;
};

struct smack_label {
	uint8_t len;
	uint16_t id;
	char *label;
	struct smack_rule *first_rule;
	struct smack_rule *last_rule;
	struct smack_label *next_label;
};

struct smack_hash_entry {
	struct smack_label *first;
	struct smack_label *last;
};

struct smack_accesses {
	int has_long;
	int labels_cnt;
	int labels_alloc;
	struct smack_label **labels;
	struct smack_hash_entry *label_hash;
};

struct cipso_mapping {
	char label[SMACK_LABEL_LEN + 1];
	int cats[CAT_MAX_VALUE];
	int ncats;
	int level;
	struct cipso_mapping *next;
};

struct smack_cipso {
	int has_long;
	struct cipso_mapping *first;
	struct cipso_mapping *last;
};

static int open_smackfs_file(const char *long_name, const char *short_name,
			     int *use_long);
static int accesses_apply(struct smack_accesses *handle, int clear);
static int accesses_print(struct smack_accesses *handle, int clear,
			  int load_fd, int change_fd, int use_long, int add_lf);
static inline ssize_t get_label(char *dest, const char *src, unsigned int *hash);
static inline int str_to_access_code(const char *str);
static inline void access_code_to_str(unsigned code, char *str);
static struct smack_label *label_add(struct smack_accesses *handle, const char *src);

int smack_accesses_new(struct smack_accesses **accesses)
{
	struct smack_accesses *result;

	result = calloc(1, sizeof(struct smack_accesses));
	if (result == NULL)
		return -1;
	result->labels_alloc = 128;
	result->labels = malloc(128 * sizeof(struct smack_label *));
	if (result->labels == NULL)
		goto err_out;
	result->label_hash = calloc(DICT_HASH_SIZE, sizeof(struct smack_hash_entry));
	if (result->label_hash == NULL)
		goto err_out;
	*accesses = result;
	return 0;

err_out:
	free(result->labels);
	free(result);
	return -1;
}

void smack_accesses_free(struct smack_accesses *handle)
{
	struct smack_rule *rule;
	struct smack_rule *next_rule;
	int i;

	if (handle == NULL)
		return;

	for (i = 0; i < handle->labels_cnt; ++i) {
		rule = handle->labels[i]->first_rule;
		while (rule != NULL) {
			next_rule = rule->next_rule;
			free(rule);
			rule = next_rule;
		}
		free(handle->labels[i]->label);
		free(handle->labels[i]);
	}

	free(handle->label_hash);
	free(handle->labels);
	free(handle);
}

int smack_accesses_save(struct smack_accesses *handle, int fd)
{
	return accesses_print(handle, 0, fd, fd, 1, 1);
}

int smack_accesses_apply(struct smack_accesses *handle)
{
	return accesses_apply(handle, 0);
}

int smack_accesses_clear(struct smack_accesses *handle)
{
	return accesses_apply(handle, 1);
}

static int accesses_add(struct smack_accesses *handle, const char *subject,
		 const char *object, const char *allow_access_type,
		 const char *deny_access_type)
{
	struct smack_rule *rule;
	struct smack_label *subject_label;
	struct smack_label *object_label;

	rule = calloc(sizeof(struct smack_rule), 1);
	if (rule == NULL)
		return -1;

	subject_label = label_add(handle, subject);
	if (subject_label == NULL)
		goto err_out;
	object_label = label_add(handle, object);
	if (object_label == NULL)
		goto err_out;

	if (subject_label->len > SHORT_LABEL_LEN ||
	    object_label->len > SHORT_LABEL_LEN)
		handle->has_long = 1;

	rule->object_id = object_label->id;

	rule->allow_code = str_to_access_code(allow_access_type);
	if (rule->allow_code == -1)
		goto err_out;

	if (deny_access_type != NULL) {
		rule->deny_code = str_to_access_code(deny_access_type);
		if (rule->deny_code == -1)
			goto err_out;
	} else
		rule->deny_code = ACCESS_TYPE_ALL & ~rule->allow_code;

	if (subject_label->first_rule == NULL) {
		subject_label->first_rule = subject_label->last_rule = rule;
	} else {
		subject_label->last_rule->next_rule = rule;
		subject_label->last_rule = rule;
	}

	return 0;
err_out:
	free(rule);
	return -1;
}

int smack_accesses_add(struct smack_accesses *handle, const char *subject,
		       const char *object, const char *access_type)
{
	return accesses_add(handle, subject, object, access_type, NULL);
}

int smack_accesses_add_modify(struct smack_accesses *handle,
			      const char *subject,
			      const char *object,
			      const char *allow_access_type,
			      const char *deny_access_type)
{
	return accesses_add(handle, subject, object,
		allow_access_type, deny_access_type);
}

int smack_accesses_add_from_file(struct smack_accesses *accesses, int fd)
{
	FILE *file = NULL;
	char buf[LOAD_LEN + 1];
	char *ptr;
	const char *subject, *object, *access, *access2;
	int newfd;
	int ret;

	newfd = dup(fd);
	if (newfd == -1)
		return -1;

	file = fdopen(newfd, "r");
	if (file == NULL) {
		close(newfd);
		return -1;
	}

	while (fgets(buf, LOAD_LEN + 1, file) != NULL) {
		if (strcmp(buf, "\n") == 0)
			continue;
		subject = strtok_r(buf, " \t\n", &ptr);
		object = strtok_r(NULL, " \t\n", &ptr);
		access = strtok_r(NULL, " \t\n", &ptr);
		access2 = strtok_r(NULL, " \t\n", &ptr);

		if (subject == NULL || object == NULL || access == NULL ||
		    strtok_r(NULL, " \t\n", &ptr) != NULL) {
			fclose(file);
			return -1;
		}

		if (access2 == NULL)
			ret = smack_accesses_add(accesses, subject, object, access);
		else
			ret = smack_accesses_add_modify(accesses, subject, object, access, access2);

		if (ret) {
			fclose(file);
			return -1;
		}
	}

	if (ferror(file)) {
		fclose(file);
		return -1;
	}

	fclose(file);
	return 0;
}

int smack_have_access(const char *subject, const char *object,
		      const char *access_type)
{
	char buf[LOAD_LEN + 1];
	char str[ACC_LEN + 1];
	int code;
	int ret;
	int fd;
	int use_long = 1;
	ssize_t slen;
	ssize_t olen;

	if (init_smackfs_mnt())
		return -1;

	slen = get_label(NULL, subject, NULL);
	olen = get_label(NULL, object, NULL);

	if (slen < 0 || olen < 0)
		return -1;

	fd = open_smackfs_file("access2", "access", &use_long);
	if (fd < 0)
		return -1;

	if (!use_long && (slen > SHORT_LABEL_LEN || olen > SHORT_LABEL_LEN))  {
		close(fd);
		return -1;
	}

	if ((code = str_to_access_code(access_type)) < 0)
		return -1;
	access_code_to_str(code, str);

	if (use_long)
		ret = snprintf(buf, LOAD_LEN + 1, KERNEL_LONG_FORMAT,
			       subject, object, str);
	else
		ret = snprintf(buf, LOAD_LEN + 1, KERNEL_SHORT_FORMAT,
			       subject, object, str);

	if (ret < 0) {
		close(fd);
		return -1;
	}

	ret = write(fd, buf, strlen(buf));
	if (ret < 0) {
		close(fd);
		return -1;
	}

	ret = read(fd, buf, 1);
	close(fd);
	if (ret < 0)
		return -1;

	return buf[0] == '1';
}

int smack_cipso_new(struct smack_cipso **cipso)
{
	struct smack_cipso *result;

	result = calloc(sizeof(struct smack_cipso), 1);
	if (result == NULL)
		return -1;

	*cipso = result;
	return 0;
}

void smack_cipso_free(struct smack_cipso *cipso)
{
	if (cipso == NULL)
		return;

	struct cipso_mapping *mapping = cipso->first;
	struct cipso_mapping *next_mapping = NULL;

	while (mapping != NULL) {
		next_mapping = mapping->next;
		free(mapping);
		mapping = next_mapping;
	}

	free(cipso);
}

int smack_cipso_apply(struct smack_cipso *cipso)
{
	struct cipso_mapping *m = NULL;
	char buf[CIPSO_MAX_SIZE];
	int fd;
	int i;
	int offset;
	int use_long;

	if (init_smackfs_mnt())
		return -1;

	fd = open_smackfs_file("cipso2", "cipso", &use_long);
	if (fd < 0)
		return -1;

	if (!use_long && cipso->has_long)
		return -1;

	memset(buf,0,CIPSO_MAX_SIZE);
	for (m = cipso->first; m != NULL; m = m->next) {
		snprintf(buf, SMACK_LABEL_LEN + 1, use_long ? "%s" : "%-23s",
			 m->label);
		offset = strlen(buf) + 1;

		sprintf(&buf[offset], CIPSO_NUM_LEN_STR, m->level);
		offset += NUM_LEN;

		sprintf(&buf[offset], CIPSO_NUM_LEN_STR, m->ncats);
		offset += NUM_LEN;

		for (i = 0; i < m->ncats; i++){
			sprintf(&buf[offset], CIPSO_NUM_LEN_STR, m->cats[i]);
			offset += NUM_LEN;
		}

		if (write(fd, buf, offset) < 0) {
			close(fd);
			return -1;
		}
	}

	close(fd);
	return 0;
}

int smack_cipso_add_from_file(struct smack_cipso *cipso, int fd)
{
	struct cipso_mapping *mapping = NULL;
	FILE *file = NULL;
	char buf[BUF_SIZE];
	char *label, *level, *cat, *ptr;
	long int val;
	int i;
	int newfd;

	newfd = dup(fd);
	if (newfd == -1)
		return -1;

	file = fdopen(newfd, "r");
	if (file == NULL) {
		close(newfd);
		return -1;
	}

	while (fgets(buf, BUF_SIZE, file) != NULL) {
		mapping = calloc(sizeof(struct cipso_mapping), 1);
		if (mapping == NULL)
			goto err_out;

		label = strtok_r(buf, " \t\n", &ptr);
		level = strtok_r(NULL, " \t\n", &ptr);
		cat = strtok_r(NULL, " \t\n", &ptr);

		if (level == NULL)
			goto err_out;

		val  = get_label(mapping->label, label, NULL);
		if (val < 0)
			goto err_out;
		if (val > SHORT_LABEL_LEN)
			cipso->has_long = 1;

		errno = 0;
		val = strtol(level, NULL, 10);
		if (errno)
			goto err_out;

		if (val < 0 || val > LEVEL_MAX)
			goto err_out;

		mapping->level = val;

		for (i = 0; i < CAT_MAX_COUNT && cat != NULL; i++) {
			errno = 0;
			val = strtol(cat, NULL, 10);
			if (errno)
				goto err_out;

			if (val < 0 || val > CAT_MAX_VALUE)
				goto err_out;

			mapping->cats[i] = val;

			cat = strtok_r(NULL, " \t\n", &ptr);
		}

		mapping->ncats = i;

		if (cipso->first == NULL) {
			cipso->first = cipso->last = mapping;
		} else {
			cipso->last->next = mapping;
			cipso->last = mapping;
		}
	}

	if (ferror(file))
		goto err_out;

	fclose(file);
	return 0;
err_out:
	fclose(file);
	free(mapping);
	return -1;
}

const char *smack_smackfs_path(void)
{
	init_smackfs_mnt();
	return smackfs_mnt;
}

ssize_t smack_new_label_from_self(char **label)
{
	char *result;
	int fd;
	int ret;

	result = calloc(SMACK_LABEL_LEN + 1, 1);
	if (result == NULL)
		return -1;

	fd = open(SELF_LABEL_FILE, O_RDONLY);
	if (fd < 0) {
		free(result);
		return -1;
	}

	ret = read(fd, result, SMACK_LABEL_LEN);
	close(fd);
	if (ret < 0) {
		free(result);
		return -1;
	}

	*label = result;
	return ret;
}

ssize_t smack_new_label_from_socket(int fd, char **label)
{
	char dummy;
	int ret;
	socklen_t length = 1;
	char *result;

	ret = getsockopt(fd, SOL_SOCKET, SO_PEERSEC, &dummy, &length);
	if (ret < 0 && errno != ERANGE)
		return -1;

	result = calloc(length + 1, 1);
	if (result == NULL)
		return -1;

	ret = getsockopt(fd, SOL_SOCKET, SO_PEERSEC, result, &length);
	if (ret < 0) {
		free(result);
		return -1;
	}

	*label = result;
	return length;
}

ssize_t smack_new_label_from_path(const char *path, const char *xattr, 
				  int follow, char **label)
{
	char buf[SMACK_LABEL_LEN + 1];
	char *result;
	ssize_t ret = 0;

	ret = follow ?
		getxattr(path, xattr, buf, SMACK_LABEL_LEN + 1) :
		lgetxattr(path, xattr, buf, SMACK_LABEL_LEN + 1);
	if (ret < 0)
		return -1;

	result = calloc(ret + 1, 1);
	if (result == NULL)
		return -1;

	ret = get_label(result, buf, NULL);
	if (ret < 0) {
		free(result);
		return -1;
	}

	*label = result;
	return ret;
}

int smack_set_label_for_self(const char *label)
{
	int len;
	int fd;
	int ret;

	len = get_label(NULL, label, NULL);
	if (len < 0)
		return -1;

	fd = open(SELF_LABEL_FILE, O_WRONLY);
	if (fd < 0)
		return -1;

	ret = write(fd, label, len);
	close(fd);

	return (ret < 0) ? -1 : 0;
}

int smack_revoke_subject(const char *subject)
{
	int ret;
	int fd;
	int len;

	if (init_smackfs_mnt())
		return -1;

	len = get_label(NULL, subject, NULL);
	if (len < 0)
		return -1;

	fd = openat(smackfs_mnt_dirfd, "revoke-subject", O_WRONLY);
	if (fd < 0)
		return -1;

	ret = write(fd, subject, len);
	close(fd);

	return (ret < 0) ? -1 : 0;
}

static int open_smackfs_file(const char *long_name, const char *short_name,
			     int *use_long)
{
	int fd;

	fd = openat(smackfs_mnt_dirfd, long_name, O_WRONLY);
	if (fd < 0) {
		if (errno != ENOENT)
			return -1;

		fd = openat(smackfs_mnt_dirfd, short_name, O_WRONLY);
		if (fd < 0)
			return -1;

		*use_long = 0;
		return fd;
	}

	*use_long = 1;
	return fd;
}

static int accesses_apply(struct smack_accesses *handle, int clear)
{
	int ret;
	int load_fd;
	int change_fd;
	int use_long = 1;

	if (init_smackfs_mnt())
		return -1;

	load_fd = open_smackfs_file("load2", "load", &use_long);
	if (load_fd < 0)
		return -1;

	change_fd = openat(smackfs_mnt_dirfd, "change-rule", O_WRONLY);
	/* Try to continue if the file doesn't exist, we might not need it. */
	if (change_fd < 0 && errno != ENOENT) {
		ret = -1;
		goto err_out;
	}

	ret = accesses_print(handle, clear, load_fd, change_fd, use_long, 0);

err_out:
	if (load_fd >= 0)
		close(load_fd);
	if (change_fd >= 0)
		close(change_fd);
	return ret;
}

static int accesses_print(struct smack_accesses *handle, int clear,
			  int load_fd, int change_fd, int use_long, int add_lf)
{
	char buf[LOAD_LEN + 1];
	char allow_str[ACC_LEN + 1];
	char deny_str[ACC_LEN + 1];
	struct smack_label *subject_label;
	struct smack_label *object_label;
	struct smack_rule *rule;
	int ret;
	int fd;
	int i;
	int x;
	int cnt;

	if (!use_long && handle->has_long)
		return -1;

	for (x = 0; x < handle->labels_cnt; ++x) {
		subject_label = handle->labels[x];
		for (rule = subject_label->first_rule; rule != NULL; rule = rule->next_rule) {
			object_label = handle->labels[rule->object_id];
			access_code_to_str(clear ? 0 : rule->allow_code, allow_str);

			if ((rule->allow_code | rule->deny_code) != ACCESS_TYPE_ALL && !clear) {
				/* Fail immediately without doing any further processing
				   if modify rules are not supported. */
				if (change_fd < 0)
					return -1;

				access_code_to_str(rule->deny_code, deny_str);

				fd = change_fd;
				cnt = snprintf(buf, LOAD_LEN + 1, KERNEL_MODIFY_FORMAT,
					subject_label->label, object_label->label,
					allow_str, deny_str);
			} else {
				fd = load_fd;
				if (use_long)
					cnt = snprintf(buf, LOAD_LEN + 1, KERNEL_LONG_FORMAT,
						subject_label->label, object_label->label,
						allow_str);
				else
					cnt = snprintf(buf, LOAD_LEN + 1, KERNEL_SHORT_FORMAT,
						subject_label->label, object_label->label,
						allow_str);
			}

			if (cnt < 0)
				return -1;
			if (add_lf)
				buf[cnt++] = '\n';

			for (i = 0; i < cnt; ) {
				ret = write(fd, buf + i, cnt - i);
				if (ret == -1) {
					if (errno == EINTR)
						continue;
					return -1;
				}
				i += ret;
			}
		}
	}

	return 0;
}

static inline ssize_t get_label(char *dest, const char *src, unsigned int *hash)
{
	int i;
	unsigned int h = 5381;/*DJB2 hashing function magic number*/;

	if (!src || src[0] == '\0' || src[0] == '-')
		return -1;

	for (i = 0; i < (SMACK_LABEL_LEN + 1) && src[i]; i++) {
		if (src[i] <= ' ' || src[i] > '~')
			return -1;
		switch (src[i]) {
		case '/':
		case '"':
		case '\\':
		case '\'':
			return -1;
		default:
			break;
		}

		if (dest)
			dest[i] = src[i];
		if (hash)
			/* This efficient hash function,
			 * created by Daniel J. Bernstein,
			 * is known as DJB2 algorithm */
			h = (h << 5) + h + src[i];
	}

	if (dest && i < (SMACK_LABEL_LEN + 1))
		dest[i] = '\0';
	if (hash)
		*hash = h % DICT_HASH_SIZE;

	return i < (SMACK_LABEL_LEN + 1) ? i : -1;
}


static inline int str_to_access_code(const char *str)
{
	int i;
	unsigned int code = 0;

	for (i = 0; str[i] != '\0'; i++) {
		switch (str[i]) {
		case 'r':
		case 'R':
			code |= ACCESS_TYPE_R;
			break;
		case 'w':
		case 'W':
			code |= ACCESS_TYPE_W;
			break;
		case 'x':
		case 'X':
			code |= ACCESS_TYPE_X;
			break;
		case 'a':
		case 'A':
			code |= ACCESS_TYPE_A;
			break;
		case 't':
		case 'T':
			code |= ACCESS_TYPE_T;
			break;
		case 'l':
		case 'L':
			code |= ACCESS_TYPE_L;
			break;
		case '-':
			break;
		default:
			return -1;
		}
	}

	return code;
}

static inline void access_code_to_str(unsigned int code, char *str)
{
	str[0] = ((code & ACCESS_TYPE_R) != 0) ? 'r' : '-';
	str[1] = ((code & ACCESS_TYPE_W) != 0) ? 'w' : '-';
	str[2] = ((code & ACCESS_TYPE_X) != 0) ? 'x' : '-';
	str[3] = ((code & ACCESS_TYPE_A) != 0) ? 'a' : '-';
	str[4] = ((code & ACCESS_TYPE_T) != 0) ? 't' : '-';
	str[5] = ((code & ACCESS_TYPE_L) != 0) ? 'l' : '-';
	str[6] = '\0';
}

static inline struct smack_label *
is_label_known(struct smack_accesses *handle, const char *label, int hash)
{
	struct smack_label *lab = handle->label_hash[hash].first;
	while (lab != NULL && strcmp(label, lab->label) != 0)
		lab = lab->next_label;
	return lab;
}

static struct smack_label *label_add(struct smack_accesses *handle, const char *label)
{
	struct smack_hash_entry *hash_entry;
	unsigned int hash_value = 0;
	struct smack_label *new_label;
	int len;

	if (handle->labels_cnt == MAX_LABELS_CNT)
		return NULL;

	len = get_label(NULL, label, &hash_value);
	if (len == -1)
		return NULL;

	new_label = is_label_known(handle, label, hash_value);
	if (new_label == NULL) {/*no entry added yet*/
		if (handle->labels_cnt == handle->labels_alloc) {
			struct smack_label **new_labels;
			int size = handle->labels_alloc * 2 * sizeof(struct smack_label *);
			new_labels = realloc(handle->labels, size);
			if (new_labels != NULL) {
				handle->labels = new_labels;
				handle->labels_alloc *= 2;
			} else
				return NULL;
		}

		new_label = malloc(sizeof(struct smack_label));
		if (new_label == NULL)
			return NULL;
		new_label->label = malloc(len + 1);
		if (new_label->label == NULL)
			return NULL;

		memcpy(new_label->label, label, len + 1);
		new_label->id = handle->labels_cnt;
		new_label->len = len;
		new_label->first_rule = NULL;
		new_label->last_rule = NULL;
		new_label->next_label = NULL;
		hash_entry = &(handle->label_hash[hash_value]);
		if (hash_entry->first == NULL) {
			hash_entry->first = new_label;
			hash_entry->last = new_label;
		} else {
			hash_entry->last->next_label = new_label;
			hash_entry->last = new_label;
		}
		handle->labels[handle->labels_cnt++] = new_label;
	}

	return new_label;
}
