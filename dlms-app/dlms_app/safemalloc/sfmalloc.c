/*
 * MIT License
 *
 * Copyright (c) 2020 Davidson Francis <davidsondfgl@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* Undef SF_TRANSPARENT to ourselves. */
#undef SF_TRANSPARENT

#include <string.h>

#include "sfmalloc.h"

#define DEBUG_LOG_MODULE_NAME "SFMALLOC"
/** To activate logs, configure the following line with "LVL_INFO". */
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

#include "common.h"

/*
 * SafeMalloc main context structure.
 */
static struct safemalloc sf;

/* Forward declarations. */
static int hashtable_init(struct hashtable *ht);
static int hashtable_finish(struct hashtable *ht);
static int hashtable_add(struct hashtable *ht, struct address *key);
static void* hashtable_remove(struct hashtable *ht, uintptr_t address);
static void* hashtable_get(struct hashtable *ht, uintptr_t key);

/* Hash stuffs */
static int hashtable_cmp_ptr(uintptr_t key1, uintptr_t key2);
static uint64_t hashtable_splitmix64_hash(uintptr_t key, size_t size);


/**
 * Deallocates all the memory allocated at the moment and warns
 * the user (if VERBOSE_MODE_2) for possible leaks.
 */
void sf_free_all(void)
{
	struct hashtable *h;   /* Hashtable.    */
	struct address *l_ptr; /* List pointer. */
	size_t i;              /* Loop index.   */

	h = &sf.ht;

    if (! h->elements)
    {
        LOGI("Heap memory elements (capacity: %u / peak: %u)",
            h->capacity, h->peak_elements);
        return;
    }

    LOGE("Heap memory elements (capacity: %u/ peak: %u) %u memory leaks",
            h->capacity, h->peak_elements, h->elements);

	/*
	 * For each bucket, deallocates each list.
	 */
	for (i = 0; i < h->capacity; i++)
	{
		struct address *l_ptr_next;

		l_ptr = h->bucket[i];
		while (l_ptr != NULL)
		{
			l_ptr_next = l_ptr->next;

			/* If not supressing messages. */
			if (sf.verbose_mode & VERBOSE_MODE_2)
			{
#ifndef SFMALLOC_SAVE_SPACE
				const char * fn = &l_ptr->file[strlen(l_ptr->file)];
				while (fn > l_ptr->file && *(fn - 1) != '/')
				{
					fn--;
				}
				LOGE("- 0x%08X (%u B): [%s:%d] %s",
					l_ptr->address, l_ptr->bytes, fn, l_ptr->line, l_ptr->context);
#else
				LOGE("- 0x%08X", l_ptr->address);
#endif
			}

			free_fn(l_ptr);
			l_ptr = l_ptr_next;
		}
	}

	/* Allocate a new hashtable again. */
	hashtable_init(&sf.ht);
}

/**
 * This function provides the heap memory status (minus the buffers allocated
 * by safe malloc for the hastable). It is identical to sf_free_all() except
 * that it does not free any memory
 */
void sf_heap_status(void)
{
	struct hashtable *h;   /* Hashtable.    */
	struct address *l_ptr; /* List pointer. */
	size_t i;              /* Loop index.   */

	h = &sf.ht;

	if (! h->elements)
	{
		LOGI("Heap memory elements (capacity: %u / peak: %u) no allocated buffers",
			h->capacity, h->peak_elements);
		return;
	}

    LOGE("Heap memory elements (capacity: %u/ peak: %u) %u memory leaks",
            h->capacity, h->peak_elements, h->elements);

	/*
	 * For each bucket, prints allocation infos for each allocated buffers
	 */
	for (i = 0; i < h->capacity; i++)
	{
		struct address *l_ptr_next;

		l_ptr = h->bucket[i];
		while (l_ptr != NULL)
		{
			l_ptr_next = l_ptr->next;

			/* If not supressing messages. */
			if (sf.verbose_mode & VERBOSE_MODE_2)
			{
#ifndef SFMALLOC_SAVE_SPACE
				const char * fn = &l_ptr->file[strlen(l_ptr->file)];
				while (fn > l_ptr->file && *(fn - 1) != '/')
				{
					fn--;
				}
				LOGI("- 0x%08X (%u B): [%s:%d] %s",
					l_ptr->address, l_ptr->bytes, fn, l_ptr->line, l_ptr->context);
#else
				LOGE("- 0x%08X", l_ptr->address);
#endif
			}

			l_ptr = l_ptr_next;
		}
	}
}

/**
 * Initializes the safemalloc context.
 *
 * @param verbose_mode Verbose mode level.
 *
 * @return Returns 0 if success and a negative
 * number otherwise.
 */
int sf_init(int verbose_mode)
{
	/* Already initialized. */
	if (sf.initialized)
	{
		return (0);
	}

	/* Allocate hashtable. */
	hashtable_init(&sf.ht);

	sf.verbose_mode = verbose_mode;
	sf.initialized  = 1;

	((void)hashtable_finish);
	((void)hashtable_get);

	return (0);
}

/**
 * Allocates @p size bytes and returns a pointer to the
 * allocated memory. The memory is not initialized.
 *
 * If @p size is zero, safemalloc will allocate a single
 * byte, thus, ensuring that no issues should occur with
 * an accidental dereference.
 *
 * @param size Size in bytes to be allocated.
 *
 * @return If success, returns a void pointer pointing to the
 * memory block allocated. On error, the should may return
 * NULL.
 */
void *_sf_malloc(size_t size
#ifndef SFMALLOC_SAVE_SPACE
	,const char *file, const char *func, int line
#endif
)
{
	struct address *addr;

    if (isInterrupt())
    {
        return NULL;
    }

	sf_init(VERBOSE_MODE_2);

	/* Avoid undefined behaviour. */
	if (!size)
		size = 1;

	/* Try to allocate memory. */
	addr = malloc_fn(sizeof(struct address) + size);
	CHECK_ADDR(addr, return NULL,
		"malloc: failed while allocating %u bytes!", size);

	/* Fill struct addr fields. */
	addr->address = (uintptr_t)(addr+1);
	addr->next    = NULL;

#ifndef SFMALLOC_SAVE_SPACE
	addr->bytes = size;
	addr->file  = file;
	addr->line  = line;
	addr->context  = Common_getContextName();
#endif

	/* Fill some infos. */
	sf.total_allocated += size;
	sf.malloc_calls++;
#ifndef SFMALLOC_SAVE_SPACE
	sf.current_memory += size;
	if (sf.current_memory > sf.peak_memory)
		sf.peak_memory = sf.current_memory;
#endif

	/* Add into the addresses list. */
	hashtable_add(&sf.ht, addr);

	/* Return address. */
	return ((void*)(addr+1));
}

/**
 * Allocates memory for an array of @p nmemb elements
 * of @p size bytes each and returns a pointer to the
 * allocated memory. The memory is set to zero.
 *
 * If total amount of bytes is 0 (either nmemb or size
 * equals 0), sfcalloc will allocate a single byte, thus,
 * ensuring that no issues should occur with
 * an accidental dereference.
 *
 * @return If success, returns a void pointer pointing to the
 * memory block allocated. On error, the should may return
 * NULL.
 */
void *_sf_calloc(size_t nmemb, size_t size
#ifndef SFMALLOC_SAVE_SPACE
	,const char *file, const char *func, int line
#endif
)
{
	struct address *addr;
	size_t new_size;

    if (isInterrupt())
    {
        return NULL;
    }

	sf_init(VERBOSE_MODE_2);

	/*
	 * There are some concerns about @p nmemb and @p size
	 * regarding why calloc have 2 parameters instead of one,
	 * like malloc.
	 *
	 * (See https://stackoverflow.com/q/4083916/3594716)
	 *
	 * Considering this, if nmemb*size exceeds size_t size
	 * the following statement will occur in overflow. Since
	 * I also need to allocate the 'struct address', this wrapper
	 * approach consists of allocating one single block of
	 * size*nmemb + sizeof(struct address) bytes. I'll leave
	 * the overflow check for the calloc_fn.
	 */
	size *= nmemb;

	/* Avoid undefined behaviour. */
	if (!size)
		size = 1;

	new_size = size + (sizeof(struct address));

	/* Try to allocate memory. */
	addr = calloc_fn(1, new_size);
	CHECK_ADDR(addr, return NULL,
		"calloc: failed while allocating %u bytes!", size);

	/* Fill struct addr fields. */
	addr->address = (uintptr_t)(addr+1);
	addr->next    = NULL;

#ifndef SFMALLOC_SAVE_SPACE
	addr->bytes = size;
	addr->file  = file;
	addr->line  = line;
	addr->context  = Common_getContextName();
#endif

	/* Fill some infos. */
	sf.total_allocated += size;
	sf.calloc_calls++;
#ifndef SFMALLOC_SAVE_SPACE
	sf.current_memory += size;
	if (sf.current_memory > sf.peak_memory)
		sf.peak_memory = sf.current_memory;
#endif

	/* Add into the addresses list. */
	hashtable_add(&sf.ht, addr);

	/* Return address. */
	return ((void*)(addr+1));
}

/**
 * Changes the size of the memory block pointed to by @p ptr.
 *
 * The function may move the memory block to a new location
 * (whose address is returned by the function). The content
 * of the memory block is preserved up to the lesser of the
 * new and old sizes, even if the block is moved to a new
 * location. If the new @p size is larger, the value of the
 * newly allocated portion is indeterminate.
 *
 * In case that ptr is a NULL pointer, the function behaves
 * like malloc, assigning a new block of size bytes and
 * returning a pointer to its beginning.
 *
 * If @p size is zero, safemalloc will allocate a single
 * byte, thus, ensuring that no issues should occur with
 * an accidental dereference.
 *
 * @param ptr Pointer to be reallocated, or NULL.
 * @param size New size.
 *
 * @return If success, returns a void pointer pointing to the
 * memory block allocated. On error, (if realloc failed or
 * invalid pointer) the should may return
 * NULL.
 */
void *_sf_realloc(void *ptr, size_t size
#ifndef SFMALLOC_SAVE_SPACE
	,const char *file, const char *func, int line
#endif
)
{
	struct address *addr     = NULL;
	struct address *new_addr = NULL;

	if (isInterrupt())
	{
	    return NULL;
	}

	sf_init(VERBOSE_MODE_2);

	/*
	 * Size equals 0 is implementation-defined, in order to avoid
	 * undefined behaviour, safemalloc will allocate a single-byte
	 * memory block that can be freed with sf_free().
	 *
	 * See here: https://stackoverflow.com/q/16759849/3594716
	 * for more infos about.
	 */
	if (!size)
		size = 1;

	/*
	 * Remove from the hashtable, if a valid address.
	 *
	 * The reasoning behind this is simple: the address _may_
	 * be different from the previously allocated, and thus,
	 * the hash too.
	 */
	if (ptr != NULL)
	{
		addr = hashtable_remove(&sf.ht, (uintptr_t)ptr);

		CHECK_ADDR(addr, return NULL,
			"sfrealloc: invalid address: %p!\n"
			"make sure that the address was previously allocated by "
			"[m,c,re]alloc!\n", ptr);
	}

	/* Try to allocate memory. */
	new_addr = realloc_fn(addr, sizeof(struct address) + size);

	/*
	 * If ptr != NULL and ptr == NULL, realloc should return with
	 * errors, but should not deallocate the previous address, thus,
	 * we need to insert again in the hashtable, before emiting
	 * errors with CHECK_ADDR =/.
	 */
	if (ptr != NULL && new_addr == NULL)
	{
		hashtable_add(&sf.ht, addr);
	}
	CHECK_ADDR(new_addr, return NULL,
		"realloc: failed while allocating %u bytes!", size);

	addr = new_addr;

	/* Fill struct addr fields. */
	addr->address = (uintptr_t)(addr+1);
	addr->next    = NULL;

#ifndef SFMALLOC_SAVE_SPACE
	addr->bytes = size;
	addr->file  = file;
	addr->line  = line;
	addr->context  = Common_getContextName();
#endif

	/* Fill some infos. */
	sf.total_allocated += size;
	sf.realloc_calls++;
#ifndef SFMALLOC_SAVE_SPACE
	if (sf.current_memory)
		sf.current_memory -= addr->bytes;
	sf.current_memory += size;
	if (sf.current_memory > sf.peak_memory)
		sf.peak_memory = sf.current_memory;
#endif

	/* Add into the addresses list. */
	hashtable_add(&sf.ht, addr);

	/* Return address. */
	return ((void*)(addr+1));
}

/**
 * Frees the memory space pointed to by ptr, which
 * must have been returned by a previous call to
 * sf_malloc(), sf_calloc(), or sfrealloc().
 *
 * If free(ptr) has already been called before and/or,
 * if @p ptr is an invalid pointer sffree may silently
 * returns or abort execution with or without verbose
 * mode, accordingly with the initial flags set on
 * sf_init().
 *
 * If @p ptr is NULL, no operation is performed.
 */
void _sf_free(void *ptr
#ifndef SFMALLOC_SAVE_SPACE
	,const char *file, const char *func, int line
#endif
)
{
	struct address *addr;

	if (isInterrupt())
	{
	    return;
	}

	if (ptr == NULL)
		return;

	/* Attempts to remove the previous address. */
	addr = hashtable_remove(&sf.ht, (uintptr_t)ptr);

	CHECK_ADDR(addr, return,
		"sffree: invalid address: %p!\nmaybe a double free?", ptr);


#ifndef SFMALLOC_SAVE_SPACE
	sf.current_memory -= addr->bytes;
#endif

	sf.free_calls++;
	free_fn(addr);
}

/*===========================================================================*
 *                                                                           *
 *                          HASHTABLE FUNCTIONS                              *
 *                                                                           *
 * From this point below all the macros, functions, structs belongs to the   *
 * hashtable.                                                                *
 *===========================================================================*/

/**
 * @brief Initializes the hashtable.
 *
 * @param ht Hashtable structure pointer to be
 * initialized.
 *
 * @return Returns 0 if success and a negative number
 * otherwise.
 */
static int hashtable_init(struct hashtable *ht)
{
	struct hashtable *out;
	out = ht;

	out->capacity = HASHTABLE_DEFAULT_SIZE;
	out->elements = 0;
	out->peak_elements = 0;
	memset(out->bucket, 0, sizeof(out->bucket));

	return (0);
}

/**
 * @brief Deallocates the hashtable.
 *
 * @param ht Hashtable pointer.
 *
 * @return Returns 0 if success.
 */
static int hashtable_finish(struct hashtable *ht)
{
	struct hashtable *h;    /* Hashtable.    */
	struct address *l_ptr;  /* List pointer. */
	size_t i;               /* Loop index.   */

	h = ht;

	/*
	 * For each bucket, deallocates each list.
	 */
	for (i = 0; i < h->capacity; i++)
	{
		struct address *l_ptr_next;

		l_ptr = h->bucket[i];
		while (l_ptr != NULL)
		{
			l_ptr_next = l_ptr->next;
			free_fn(l_ptr);
			l_ptr = l_ptr_next;
		}
	}

	return (0);
}

/**
 * @brief Calculates the bucket size accordingly with the
 * current hash function set and the current hashtable
 * capacity.
 *
 * @param ht Hashtable pointer.
 * @param key Key to be hashed.
 *
 * @return Returns a value between [0,h->capacity] that
 * should be used to address the target bucket for the
 * hash table.
 */
static size_t hashtable_bucket_index(struct hashtable *ht, uintptr_t key)
{
	const struct hashtable *h;  /* Hashtable.    */
	uint64_t hash;              /* Hash value.   */
	size_t index;               /* Bucket index. */

	/*
	 * Its important to note that the hashtable->capacity
	 * will always be power of 2, thus, allowing the &-1
	 * as a modulus operation.
	 */
	h     = ht;
	hash  = hashtable_splitmix64_hash(key, h->key_size);
	index = hash & (h->capacity - 1);

	return (index);
}

/**
 * @brief Adds the current @p key and @p value into the hashtable.
 *
 * @param ht Hashtable pointer.
 * @param key Key to be added.
 * @param value Value to be added.
 *
 * The hash table will grows twice whenever it reaches
 * its threshold of 60% percent.
 *
 * @return Returns 0
 */
static int hashtable_add(struct hashtable *ht, struct address *key)
{
	struct hashtable *h;    /* Hashtable.    */
	struct address *l_ptr;  /* List pointer. */
	size_t hash;            /* Hash value.   */

	h = ht;

	/* Hash it. */
	hash = hashtable_bucket_index(ht, key->address);

	/*
	 * Loops through the list in order to see if the key already exists,
	 * if so, do nothing, just returns. However, this should not occur.
	 */
	l_ptr = h->bucket[hash];
	while (l_ptr != NULL)
	{
		if (l_ptr->address != 0 && hashtable_cmp_ptr(l_ptr->address, key->address) == 0)
			return (0);

		l_ptr = l_ptr->next;
	}

	/* Fill the entry. */
	key->next = h->bucket[hash];

	/* Add into the top of the buckets list. */
#if HASHTABLE_DEBUG
	if (h->bucket[hash] != NULL)
		h->collisions++;
#endif

	h->bucket[hash] = key;
	h->elements++;
	if (h->elements > h->peak_elements)
	{
	    h->peak_elements = h->elements;
	}

	return (0);
}

/**
 * @brief Removes the address @p address from the hashtable.
 *
 * @param ht Hashtable pointer.
 * @param address Address to be removed.
 *
 * The hash table will shrinks twice whenever it reaches down
 * its threshold of 60% percent.
 *
 * @return If success, returns a valid pointer to 'struct address',
 * otherwise, returns NULL.
 */
static void *hashtable_remove(struct hashtable *ht, uintptr_t address)
{
	struct hashtable *h;    /* Hashtable.    */
	struct address *l_ptr;  /* List pointer. */
	struct address **prev;  /* Prev pointer. */
	size_t hash;            /* Hash value.   */

	h = ht;

	/* Hash it. */
	hash = hashtable_bucket_index(ht, address);

	/*
	 * Loops through the list in order to see if the key exists,
	 * if so, removes.
	 */
	prev  = &h->bucket[hash];
	l_ptr = h->bucket[hash];
	while (l_ptr != NULL)
	{
		if (l_ptr->address != 0 && hashtable_cmp_ptr(l_ptr->address, address) == 0)
		{
			*prev = l_ptr->next;

			h->elements--;
#if HASHTABLE_DEBUG
			if (l_ptr != h->bucket[hash])
				if (h->collisions)
					h->collisions--;
#endif
			return (l_ptr);
		}

		prev  = &l_ptr->next;
		l_ptr = l_ptr->next;
	}

	return (NULL);
}

/**
 * @brief Retrieves the value belonging to the parameter @p key.
 *
 * @param ht Hashtable pointer.
 * @param key Corresponding key to be retrieved.
 *
 * @return Returns the value belonging to the @p key, or NULL
 * if not found.
 */
static void* hashtable_get(struct hashtable *ht, uintptr_t key)
{
	struct hashtable *h;    /* Hashtable.    */
	struct address *l_ptr;  /* List pointer. */
	uint64_t hash;          /* Hash value.   */

	h = ht;

	/* Hashtable exists and has at least one element?. */
	if (h == NULL || h->elements < 1)
		return (NULL);

	/* Hash it. */
	hash = hashtable_bucket_index(ht, key);

	/*
	 * Loops through the list in order to see if the key exists,
	 * if so, gets the value.
	 */
	l_ptr = h->bucket[hash];
	while (l_ptr != NULL)
	{
		if (l_ptr->address != 0 && hashtable_cmp_ptr(l_ptr->address, key) == 0)
			return (l_ptr);

		l_ptr = l_ptr->next;
	}

	/* If not found, return NULL. */
	return (NULL);
}

/*===========================================================================*
 *                         -.- Hash functions -.-                            *
 *===========================================================================*/

/**
 * @brief Generic key comparator.
 *
 * Compares two given pointers and returns a negative, 0 or positive
 * number if less than, equal or greater for the keys specified.
 *
 * @param key1 First key to be compared.
 * @param key2 Second key to be compared.
 *
 * @returns Returns a negative, 0 or positive number if @p key1
 * is less than, equal or greater than @p key2.
 */
static int hashtable_cmp_ptr(uintptr_t key1, uintptr_t key2)
{
	return ( (int)( key1 - key2 ) );
}

/*----------------------------------------------------------------------------*
 * Based in splitmix64, from Better Bit Mixing - Improving on MurmurHash3's   *
 * 64-bit Finalizer                                                           *
 *                                                                            *
 * Link:http://zimbry.blogspot.com/2011/09/better-bit-mixing-improving-on.html*
 * ----                                                                       *
 * I've found this particular interesting and the results showed that the     *
 * resulting hash is very close to MurMur3 ;-).                               *
 *----------------------------------------------------------------------------*/

/**
 * @brief splitmix64 based algorithm.
 *
 * This algorithm seems to be the 'Mix13' for the link above.
 *
 * @param key Key to be hashed.
 * @param size Pointer size (unused here).
 *
 * @return Returns a hashed number for the @p key argument.
 */
static uint64_t hashtable_splitmix64_hash(uintptr_t key, size_t size)
{
	uint64_t x; /* Hash key. */
	((void)size);

	x = (uint64_t)key;
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9U;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebU;
	x = x ^ (x >> 31);
	return (x);
}

