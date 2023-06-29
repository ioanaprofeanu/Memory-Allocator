// SPDX-License-Identifier: BSD-3-Clause

#include <printf.h>
#include <unistd.h>
#include "osmem.h"
#include "helpers.h"

block_meta * block_meta_head = NULL;

// function which requests space for allocating a new memory block
struct block_meta *request_space(struct block_meta *last_block_meta,
						size_t size, int is_calloc, int is_malloc)
{
	block_meta *new_block = NULL;
	size_t compare_value = 0;

	// check the type of allocation and set the compare value
	if (is_calloc) {
		long result = sysconf(_SC_PAGESIZE);

		DIE(result < 0, "sysconf failed");
		compare_value = result;
	} else if (is_malloc) {
		compare_value = MMAP_THRESHOLD_VALUE;
	}

	// check if the size is large enough to be allocated with mmap
	if (size + ALIGN(META_SIZE) >= compare_value) {
		// allocate the new block (containing the metadata as well)
		new_block = mmap(NULL, size + ALIGN(META_SIZE),
				PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		DIE(new_block == (void *)-1, "mmap failed");
		new_block->status = STATUS_MAPPED;
		new_block->size = size;
		new_block->next = NULL;

		// check the position of the new block
		if (block_meta_head == NULL)
			block_meta_head = new_block;
		else
			last_block_meta->next = new_block;

	// if the size is smaller than the compare value, allocate it with sbrk
	} else {
		// if no memory have been allocated with brk
		if (block_meta_head == NULL || check_brk_allocated_memory(block_meta_head) == 0) {
			// make preallocation
			new_block = sbrk(MMAP_THRESHOLD_VALUE);
			DIE(new_block == (void *)-1, "sbrk failed");
			new_block->status = STATUS_ALLOC;
			new_block->size = MMAP_THRESHOLD_VALUE - ALIGN(META_SIZE);
			// split the new block
			make_split(new_block, size);
			// check position
			if (block_meta_head == NULL)
				block_meta_head = new_block;

		// if there is already memory allocated with brk
		} else {
			// check if we can expand the last allocated block
			// get to the last block allocated with sbrk
			block_meta *last_block_alloc = block_meta_head;

			while (last_block_alloc->next != NULL) {
				if (last_block_alloc->next->status == STATUS_MAPPED)
					break;
				last_block_alloc = last_block_alloc->next;
			}

			if (last_block_alloc->size < size + ALIGN(META_SIZE) &&
				last_block_alloc->status == STATUS_FREE) {
				// allocate memory only for the remaining size
				new_block = sbrk(size - last_block_alloc->size);
				last_block_alloc->status = STATUS_ALLOC;
				last_block_alloc->size = size;
				return last_block_alloc;
			}

			// if we can't expand, allocate a new block
			new_block = sbrk(size + ALIGN(META_SIZE));
			DIE(new_block == (void *)-1, "sbrk failed");
			new_block->status = STATUS_ALLOC;
			new_block->size = size;
			last_block_meta->next = new_block;
		}
	}

	return new_block;
}

// function which allocates memory
void *allocate_memory(size_t size, int is_calloc, int is_malloc)
{
	if (size <= 0)
		return NULL;

	size_t compare_value = 0;

	// check the type of allocation and set the compare value
	if (is_calloc) {
		long result = sysconf(_SC_PAGESIZE);

		DIE(result < 0, "sysconf failed");
		compare_value = result;
	} else if (is_malloc) {
		compare_value = MMAP_THRESHOLD_VALUE;
	}

	size_t block_size = ALIGN(size);
	block_meta *current_block = block_meta_head;

	// if no memory has previously been allocated, create a new block
	if (block_meta_head == NULL) {
		current_block = request_space(NULL, block_size, is_calloc, is_malloc);
		if (current_block == NULL)
			return NULL;

	// otherwise, all possible cases
	} else {
		// if we should use mmap
		if (size >= compare_value) {
			// get to the last block and request space for the new block
			block_meta *last_block = block_meta_head;

			while (last_block->next != NULL)
				last_block = last_block->next;

			current_block = request_space(last_block, block_size,
							is_calloc, is_malloc);

			if (current_block == NULL)
				return NULL;

		// otherwise, check if we can find a free block within the
		// already allocated memory
		} else {
			block_meta *best_fit = find_best_free_block(block_size, block_meta_head);

			// if we could not find a best fit
			if (best_fit == NULL) {
				// get to the last block and request space for the new block
				block_meta *last_block = block_meta_head;

				while (last_block->next != NULL)
					last_block = last_block->next;

				current_block = request_space(last_block, block_size,
								is_calloc, is_malloc);
				if (current_block == NULL)
					return NULL;

			// otherwise, if we found free allocated space
			} else {
				// mark as allocated and split the block
				current_block = best_fit;
				current_block->status = STATUS_ALLOC;
				make_split(current_block, block_size);
			}
		}
	}

	return (void *)(current_block + 1);
}

// malloc function
void *os_malloc(size_t size)
{
	return allocate_memory(size, 0, 1);
}

// free function, which frees the memory of a block
void os_free(void *ptr)
{
	if (!ptr)
		return;

	// get to the wanted block; keep track of the previous block
	block_meta *current_block = block_meta_head;
	block_meta *prev_block = NULL;

	while (current_block != NULL) {
		coalesce(current_block, current_block->next);
		if (current_block + 1 == ptr)
			break;
		prev_block = current_block;
		current_block = current_block->next;
	}

	// check if the block is valid
	if (current_block == NULL)
		return;

	if (current_block->status == STATUS_FREE)
		return;

	// if it was allocated with sbrk
	if (current_block->status == STATUS_ALLOC) {
		// mark as free
		current_block->status = STATUS_FREE;
		// coalesce the current block with its left
		// and right neighbors
		coalesce(current_block, current_block->next);
		coalesce(prev_block, current_block);

	// if the block was allocated with mmap
	} else {
		// remove from list
		if (prev_block != NULL)
			prev_block->next = current_block->next;
		if (prev_block == NULL)
			block_meta_head = NULL;
		else
			prev_block->next = current_block->next;
		// unmap the memory block
		int result = munmap(current_block,
					current_block->size + ALIGN(META_SIZE));
		DIE(result < 0, "munmap failed");
	}
}

// calloc function
void *os_calloc(size_t nmemb, size_t size)
{
	size_t total_block_size = nmemb * size;

	// check if the parameters are valid
	if (nmemb == 0 || size == 0 ||  total_block_size / nmemb != size)
		return NULL;

	// allocate memory
	void *ptr = allocate_memory(total_block_size, 1, 0);

	// if the allocation was successful, initialize memory to 0
	if (ptr != NULL) {
		// Use the byte-wise memset to initialize memory to 0
		for (size_t i = 0; i < total_block_size; i++)
			((char *)ptr)[i] = 0;
	}
	return ptr;
}

// realloc function
void *os_realloc(void *ptr, size_t size)
{
	// check the special cases (size == 0 or ptr == NULL)
	if (size == 0) {
		os_free(ptr);
		return NULL;
	}
	if (ptr == NULL)
		return os_malloc(size);

	// get to the wanted block
	block_meta *current_block = block_meta_head;

	while (current_block != NULL) {
		coalesce(current_block, current_block->next);
		if (current_block + 1 == ptr)
			break;
		current_block = current_block->next;
	}

	// check if the block is valid
	if (current_block == NULL || current_block->status == STATUS_FREE)
		return NULL;

	size_t block_size = ALIGN(size);

	// if the block was allocated with mmap
	if (current_block->status == STATUS_MAPPED) {
		// allocate a new memory block
		void *new_ptr = os_malloc(size);

		if (new_ptr == NULL)
			return NULL;

		// copy the data from the old block to the new one,
		size_t minimum_size = current_block->size;

		if (block_size < minimum_size)
			minimum_size = block_size;

		for (size_t i = 0; i < minimum_size; i++)
			((char *)new_ptr)[i] = ((char *)ptr)[i];

		// free the old block
		os_free(ptr);
		return new_ptr;
	}

	// if the current block is larger than the requested size
	if (current_block->size >= block_size) {
		// split the block
		make_split(current_block, block_size);
		return ptr;
	}

	// if the next block is free and there is enough space, merge them
	if (current_block->next != NULL
		&& current_block->next->status == STATUS_FREE
		&& current_block->size + current_block->next->size
		+ ALIGN(META_SIZE) >= block_size) {
		// merge the two blocks
		current_block->size += current_block->next->size
							+ ALIGN(META_SIZE);
		current_block->next = current_block->next->next;
		// split the block
		make_split(current_block, block_size);
		return ptr;
	}

	// if the block is the last one allocated with sbrk,
	// mark it as free so that when we call the malloc function,
	// it will expand the current block
	if (current_block->next == NULL
		|| current_block->next->status == STATUS_MAPPED)
		current_block->status = STATUS_FREE;

	// allocate a new memory block
	void *new_ptr = os_malloc(size);

	if (new_ptr == NULL)
		return NULL;

	// copy the data from the old block to the new one,
	size_t minimum_size = current_block->size;

	if (block_size < minimum_size)
		minimum_size = block_size;

	for (size_t i = 0; i < minimum_size; i++)
		((char *)new_ptr)[i] = ((char *)ptr)[i];

	// if the block was the last one, return it (we don't have to
	// free the block)
	if (new_ptr == ptr && current_block->next == NULL)
		return new_ptr;

	// free the old block
	os_free(ptr);

	return new_ptr;
}
