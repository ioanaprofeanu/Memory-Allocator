/* SPDX-License-Identifier: BSD-3-Clause */

#pragma once

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define DIE(assertion, call_description)						\
	do {										\
		if (assertion) {							\
			fprintf(stderr, "(%s, %d): ", __FILE__, __LINE__);		\
			perror(call_description);					\
			exit(errno);							\
		}									\
	} while (0)

/* Structure to hold memory block metadata */
typedef struct block_meta block_meta;
struct block_meta {
	size_t size;
	int status;
	struct block_meta *next;
};

/* Block metadata status values */
#define STATUS_FREE   0
#define STATUS_ALLOC  1
#define STATUS_MAPPED 2

#define META_SIZE sizeof(struct block_meta)
#define MMAP_THRESHOLD_VALUE 128 * 1024
#define ALIGNMENT 8
#define ALIGN(size) (((size) + (ALIGNMENT-1)) & ~(ALIGNMENT-1))

void coalesce(block_meta *left_block, block_meta *right_block);

void *find_best_free_block(size_t size, block_meta *block_meta_head);

void make_split(block_meta *last_block, size_t size);

int check_brk_allocated_memory(block_meta *block_meta_head);
