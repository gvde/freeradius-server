/*
 *   This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Lesser General Public
 *   License as published by the Free Software Foundation; either
 *   version 2.1 of the License, or (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *   Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/** Functions which we wish were included in the standard talloc distribution
 *
 * @file src/lib/util/talloc.c
 *
 * @copyright 2017 The FreeRADIUS server project
 * @copyright 2017 Arran Cudbard-Bell (a.cudbardb@freeradius.org)
 */
RCSID("$Id$")

#include <freeradius-devel/util/atexit.h>
#include <freeradius-devel/util/debug.h>
#include <freeradius-devel/util/dlist.h>
#include <freeradius-devel/util/syserror.h>


static TALLOC_CTX *global_ctx;
static _Thread_local TALLOC_CTX *thread_local_ctx;

/** A wrapper that can be passed to tree or hash alloc functions that take a #fr_free_t
 */
void talloc_free_data(void *data)
{
	talloc_free(data);
}

/** Retrieve the current talloc NULL ctx
 *
 * Talloc doesn't provide a function to retrieve the top level memory tracking context.
 * This function does that...
 *
 * @return the current talloc NULL context or NULL if memory tracking is not enabled.
 */
void *talloc_null_ctx(void)
{
	TALLOC_CTX *null_ctx;
	bool *tmp;

	tmp = talloc(NULL, bool);
	null_ctx = talloc_parent(tmp);
	talloc_free(tmp);

	return null_ctx;
}

/** Called with the fire_ctx is freed
 *
 */
static int _talloc_destructor_fire(fr_talloc_destructor_t *d)
{
	if (d->ds) {
		talloc_set_destructor(d->ds, NULL);	/* Disarm the disarmer */
		TALLOC_FREE(d->ds);			/* Free the disarm trigger ctx */
	}

	return d->func(d->fire, d->uctx);
}

/** Called when the disarm_ctx ctx is freed
 *
 */
static int _talloc_destructor_disarm(fr_talloc_destructor_disarm_t *ds)
{
	talloc_set_destructor(ds->d, NULL);		/* Disarm the destructor */
	return talloc_free(ds->d);			/* Free memory allocated to the destructor */
}

/** Add an additional destructor to a talloc chunk
 *
 * @param[in] fire_ctx		When this ctx is freed the destructor function
 *				will be called.
 * @param[in] disarm_ctx	When this ctx is freed the destructor will be
 *				disarmed. May be NULL.  #talloc_destructor_disarm
 *				may be used to disarm the destructor too.
 * @param[in] func		to call when the fire_ctx is freed.
 * @param[in] uctx		data to pass to the above function.
 * @return
 *	- A handle to access the destructor on success.
 *	- NULL on failure.
 */
fr_talloc_destructor_t *talloc_destructor_add(TALLOC_CTX *fire_ctx, TALLOC_CTX *disarm_ctx,
					      fr_talloc_free_func_t func, void const *uctx)
{
	fr_talloc_destructor_t *d;

	if (!fire_ctx) {
		fr_strerror_const("No firing ctx provided when setting destructor");
		return NULL;
	}

	d = talloc(fire_ctx, fr_talloc_destructor_t);
	if (!d) {
	oom:
		fr_strerror_const("Out of Memory");
		return NULL;
	}

	d->fire = fire_ctx;
	d->func = func;
	memcpy(&d->uctx, &uctx, sizeof(d->uctx));

	if (disarm_ctx) {
		fr_talloc_destructor_disarm_t *ds;

		ds = talloc(disarm_ctx, fr_talloc_destructor_disarm_t);
		if (!ds) {
			talloc_free(d);
			goto oom;
		}
		ds->d = d;
		d->ds = ds;
		talloc_set_destructor(ds, _talloc_destructor_disarm);
	}

	talloc_set_destructor(d, _talloc_destructor_fire);

	return d;
}

/** Disarm a destructor and free all memory allocated in the trigger ctxs
 *
 */
void talloc_destructor_disarm(fr_talloc_destructor_t *d)
{
	if (d->ds) {
		talloc_set_destructor(d->ds, NULL);	/* Disarm the disarmer */
		TALLOC_FREE(d->ds);			/* Free the disarmer ctx */
	}

	talloc_set_destructor(d, NULL);			/* Disarm the destructor */
	talloc_free(d);					/* Free the destructor ctx */
}

static int _talloc_link_ctx_free(UNUSED void *parent, void *child)
{
	talloc_free(child);

	return 0;
}

/** Link two different parent and child contexts, so the child is freed before the parent
 *
 * @note This is not thread safe. Do not free parent before threads are joined, do not call from a
 *	child thread.
 *
 * @param parent who's fate the child should share.
 * @param child bound to parent's lifecycle.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int talloc_link_ctx(TALLOC_CTX *parent, TALLOC_CTX *child)
{
	if (!talloc_destructor_add(parent, child, _talloc_link_ctx_free, child)) return -1;

	return 0;
}

/** Return a page aligned talloc memory array
 *
 * Because we can't intercept talloc's malloc() calls, we need to do some tricks
 * in order to get the first allocation in the array page aligned, and to limit
 * the size of the array to a multiple of the page size.
 *
 * The reason for wanting a page aligned talloc array, is it allows us to
 * mprotect() the pages that belong to the array.
 *
 * Talloc chunks appear to be allocated within the protected region, so this should
 * catch frees too.
 *
 * @param[in] ctx	to allocate array memory in.
 * @param[out] start	The first aligned address in the array.
 * @param[in] alignment	What alignment the memory chunk should have.
 * @param[in] size	How big to make the array.  Will be corrected to a multiple
 *			of the page size.  The actual array size will be size
 *			rounded to a multiple of the (page_size), + page_size
 * @return
 *	- A talloc chunk on success.
 *	- NULL on failure.
 */
TALLOC_CTX *talloc_aligned_array(TALLOC_CTX *ctx, void **start, size_t alignment, size_t size)
{
	size_t		rounded;
	size_t		array_size;
	void		*next;
	TALLOC_CTX	*array;

	rounded = ROUND_UP(size, alignment);		/* Round up to a multiple of the page size */
	if (rounded == 0) rounded = alignment;

	array_size = rounded + alignment;
	array = talloc_array(ctx, uint8_t, array_size);		/* Over allocate */
	if (!array) {
		fr_strerror_const("Out of memory");
		return NULL;
	}

	next = (void *)ROUND_UP((uintptr_t)array, alignment);		/* Round up address to the next multiple */
	*start = next;

	return array;
}

/** How big the chunk header is
 *
 * Non-zero portion will always fit in a uint8_t, so we don't need to worry about atomicity.
 */
static size_t		t_hdr_size;

/** Calculate the size of talloc chunk headers, once, on startup
 *
 */
static void _talloc_hdr_size(void)
{
	TALLOC_CTX	*pool;
	void		*chunk;

	pool = talloc_pool(NULL, 1024);	/* Allocate 1k of memory, this will always be bigger than the chunk header */
	if (unlikely(pool == NULL)) {
	oom:
		fr_strerror_const("Out of memory");
		return;
	}
	chunk = talloc_size(pool, 1);	/* Get the starting address */
	if (unlikely(chunk == NULL)) goto oom;

	/*
	 *	Sanity check... make sure the chunk is within the pool
	 *	if it's not, we're in trouble.
	 */
	if (!fr_cond_assert((chunk > pool) && ((uintptr_t)chunk < ((uintptr_t)pool + 1024)))) {
		fr_strerror_const("Initial allocation outside of pool memory");
		return;
	}

	/*
	 *	Talloc returns the address after the chunk header, so
	 *	the address of the pool is <malloc address> + <hdr_size>.
	 *
	 *	If we allocate a chunk inside the pool, then the address
	 *	of the chunk will be <malloc address> + <hdr_size> + <hdr_size>.
	 *	If we subtrace the chunk from the pool address, we get
	 *	the size of the talloc header.
	 */
	t_hdr_size = (uintptr_t)chunk - (uintptr_t)pool;

	talloc_free(pool); /* Free the memory we used */
}

/** Calculate the size of the talloc chunk header
 *
 * @return
 *	- >0 the size of the talloc chunk header.
 *	- -1 on error.
 */
ssize_t talloc_hdr_size(void)
{
	static pthread_once_t once_control = PTHREAD_ONCE_INIT;

	if (t_hdr_size > 0) return t_hdr_size;	/* We've already calculated it */

	if (pthread_once(&once_control, _talloc_hdr_size) != 0) {
		fr_strerror_printf("pthread_once failed: %s", fr_syserror(errno));
		return -1;
	}
	if (t_hdr_size == 0) return -1; /* Callback should set error */

	return t_hdr_size;
}

/** Return a page aligned talloc memory pool
 *
 * Because we can't intercept talloc's malloc() calls, we need to do some tricks
 * in order to get the first allocation in the pool page aligned, and to limit
 * the size of the pool to a multiple of the page size.
 *
 * The reason for wanting a page aligned talloc pool, is it allows us to
 * mprotect() the pages that belong to the pool.
 *
 * Talloc chunks appear to be allocated within the protected region, so this should
 * catch frees too.
 *
 * @param[in] ctx	to allocate pool memory in.
 * @param[out] start	A page aligned address within the pool.  This can be passed
 *			to mprotect().
 * @param[out] end_len	how many bytes to protect.
 * @param[in] headers	how much room we should allocate for talloc headers.
 *			This value should usually be >= 1.
 * @param[in] size	How big to make the pool.  Will be corrected to a multiple
 *			of the page size.  The actual pool size will be size
 *			rounded to a multiple of the (page_size), + page_size
 */
TALLOC_CTX *talloc_page_aligned_pool(TALLOC_CTX *ctx, void **start, size_t *end_len, unsigned int headers, size_t size)
{
	size_t		rounded, alloced, page_size = (size_t)getpagesize();
	size_t		hdr_size;
	void		*next;
	TALLOC_CTX	*pool;

	hdr_size = talloc_hdr_size();
	size += (hdr_size * headers);	/* Add more space for the chunks headers of the pool's children */
	size += hdr_size;		/* Add one more header to the pool for the padding allocation */

	/*
	 *  Allocate enough space for the padding header the other headers
	 *  and the actual data, and sure it's a multiple of the page_size.
	 *
	 *   |<--- page_size --->|<-- page_size -->|
	 *   |<-- hdr_size -->|<-- size -->|
	 */
	if (size % page_size == 0) {
		rounded = size;
	} else {
		rounded = ROUND_UP(size, page_size);
	}

	/*
	 *  Add another page, so that we can align the first allocation in
	 *  the pool to a page boundary.  We have no idea what chunk malloc
	 *  will give to talloc, and what the first address after adding the
	 *  pools header will be
	 */
	alloced = rounded + page_size;
	pool = talloc_pool(ctx, alloced);
	if (!pool) {
		fr_strerror_const("Out of memory");
		return NULL;
	}

	/*
	 *  If we didn't get lucky, and the first address in the pool is
	 *  not the start of a page, we need to allocate some padding to
	 *  get the first allocation in the pool to be on or after the
	 *  start of the next page.
	 */
	if ((uintptr_t)pool % page_size != 0) {
		size_t	pad_size;
		void	*padding;

		next = (void *)ROUND_UP((uintptr_t)pool, page_size);	/* Round up address to the next page */

		/*
		 *	We don't care if the first address if on a page
		 *	boundary, just that it comes after one.
		 */
		pad_size = ((uintptr_t)next - (uintptr_t)pool);
		if (pad_size > hdr_size) {
			pad_size -= hdr_size;			/* Save ~111 bytes by not over-padding */
		} else {
			pad_size = 0;				/* Allocate as few bytes as possible */
		}

		padding = talloc_size(pool, pad_size);
		if (!fr_cond_assert(((uintptr_t)padding + (uintptr_t)pad_size) >= (uintptr_t)next)) {
			fr_strerror_const("Failed padding pool memory");
			return NULL;
		}
	} else {
		next = pool;
	}

	*start = next;			/* This is the address we feed into mprotect */
	*end_len = rounded;		/* This is how much memory we protect */

	/*
	 *	Start address must be page aligned
	 */
	fr_assert(((uintptr_t)*start % page_size) == 0);

	/*
	 *	For our sanity, length must be a multiple of the page size.
	 *	mprotect will silently protect an entire additional page
	 *	if length is not a multiple of page size.
	 */
	fr_assert(((uintptr_t)*end_len % page_size) == 0);

	/*
	 *	We can't protect pages before the pool
	 */
	fr_assert(*start >= pool);

	/*
	 *	We can't protect pages after the pool
	 */
	fr_assert(((uintptr_t)*start + *end_len) <= ((uintptr_t)pool + alloced));

	return pool;
}

/** Version of talloc_realloc which zeroes out freshly allocated memory
 *
 * @param[in] ctx	talloc ctx to allocate in.
 * @param[in] ptr	the pointer to the old memory.
 * @param[in] elem_size	the size of each element in the array.
 * @param[in] count	the number of elements in the array.
 * @param[in] name	the name of the new chunk.
 * @return
 *	- A pointer to the new memory.
 *	- NULL on error.
 */
void *_talloc_realloc_zero(const void *ctx, void *ptr, size_t elem_size, unsigned count, const char *name)
{
    size_t old_size = talloc_get_size(ptr);
    size_t new_size = elem_size * count;

    void *new = _talloc_realloc_array(ctx, ptr, elem_size, count, name);
    if (!new) return NULL;

    if (new_size > old_size) {
        memset((uint8_t *)new + old_size, 0, new_size - old_size);
    }

    return new;
}

/** Call talloc_memdup, setting the type on the new chunk correctly
 *
 * @param[in] ctx	The talloc context to hang the result off.
 * @param[in] in	The data you want to duplicate.
 * @param[in] inlen	the length of the data to be duplicated.
 * @return
 *	- Duplicated data.
 *	- NULL on error.
 *
 * @hidecallergraph
 */
uint8_t	*talloc_typed_memdup(TALLOC_CTX *ctx, uint8_t const *in, size_t inlen)
{
	uint8_t *n;

	n = talloc_memdup(ctx, in, inlen);
	if (unlikely(!n)) return NULL;
	talloc_set_type(n, uint8_t);

	return n;
}

/** Call talloc_strdup, setting the type on the new chunk correctly
 *
 * For some bizarre reason the talloc string functions don't set the
 * memory chunk type to char, which causes all kinds of issues with
 * verifying fr_pair_ts.
 *
 * @param[in] ctx	The talloc context to hang the result off.
 * @param[in] p		The string you want to duplicate.
 * @return
 *	- Duplicated string.
 *	- NULL on error.
 *
 * @hidecallergraph
 */
char *talloc_typed_strdup(TALLOC_CTX *ctx, char const *p)
{
	char *n;

	n = talloc_strdup(ctx, p);
	if (unlikely(!n)) return NULL;
	talloc_set_type(n, char);

	return n;
}

/** Call talloc_strndup, setting the type on the new chunk correctly
 *
 * This function is similar to talloc_typed_strdup but gets the chunk
 * length using talloc functions.
 *
 * @param[in] ctx	The talloc context to hang the result off.
 * @param[in] p		The string you want to duplicate.
 * @return
 *	- Duplicated string.
 *	- NULL on error.
 *
 * @hidecallergraph
 */
char *talloc_typed_strdup_buffer(TALLOC_CTX *ctx, char const *p)
{
	char *n;

	n = talloc_strndup(ctx, p, talloc_array_length(p) - 1);
	if (unlikely(!n)) return NULL;
	talloc_set_type(n, char);

	return n;
}

/** Call talloc vasprintf, setting the type on the new chunk correctly
 *
 * For some bizarre reason the talloc string functions don't set the
 * memory chunk type to char, which causes all kinds of issues with
 * verifying fr_pair_ts.
 *
 * @param[in] ctx	The talloc context to hang the result off.
 * @param[in] fmt	The format string.
 * @return
 *	- Formatted string.
 *	- NULL on error.
 */
char *talloc_typed_asprintf(TALLOC_CTX *ctx, char const *fmt, ...)
{
	char *n;
	va_list ap;

	va_start(ap, fmt);
	n = talloc_vasprintf(ctx, fmt, ap);
	va_end(ap);
	if (unlikely(!n)) return NULL;
	talloc_set_type(n, char);

	return n;
}

/** Call talloc vasprintf, setting the type on the new chunk correctly
 *
 * For some bizarre reason the talloc string functions don't set the
 * memory chunk type to char, which causes all kinds of issues with
 * verifying fr_pair_ts.
 *
 * @param[in] ctx	The talloc context to hang the result off.
 * @param[in] fmt	The format string.
 * @param[in] ap	varadic arguments.
 * @return
 *	- Formatted string.
 *	- NULL on error.
 */
char *talloc_typed_vasprintf(TALLOC_CTX *ctx, char const *fmt, va_list ap)
{
	char *n;

	n = talloc_vasprintf(ctx, fmt, ap);
	if (unlikely(!n)) return NULL;
	talloc_set_type(n, char);

	return n;
}

/** Binary safe strdup function
 *
 * @param[in] ctx 	the talloc context to allocate new buffer in.
 * @param[in] in	String to dup, may contain embedded '\0'.
 * @return duped string.
 */
char *talloc_bstrdup(TALLOC_CTX *ctx, char const *in)
{
	char	*p;
	size_t	inlen = talloc_array_length(in);

	if (inlen == 0) inlen = 1;

	p = talloc_array(ctx, char, inlen);
	if (unlikely(!p)) return NULL;

	/*
	 * C99 (7.21.1/2) - Length zero results in noop
	 *
	 * But ubsan still flags this, grrr.
	 */
	if (inlen > 0) memcpy(p, in, inlen - 1);
	p[inlen - 1] = '\0';

	return p;
}

/** Binary safe strndup function
 *
 * @param[in] ctx 	he talloc context to allocate new buffer in.
 * @param[in] in	String to dup, may contain embedded '\0'.
 * @param[in] inlen	Number of bytes to dup.
 * @return duped string.
 */
char *talloc_bstrndup(TALLOC_CTX *ctx, char const *in, size_t inlen)
{
	char *p;

	p = talloc_array(ctx, char, inlen + 1);
	if (unlikely(!p)) return NULL;

	/*
	 * C99 (7.21.1/2) - Length zero results in noop
	 *
	 * But ubsan still flags this, grrr.
	 */
	if (inlen > 0) memcpy(p, in, inlen);
	p[inlen] = '\0';

	return p;
}

/** Append a bstr to a bstr
 *
 * @param[in] ctx	to allocated.
 * @param[in] to	string to append to.
 * @param[in] from	string to append from.
 * @param[in] from_len	Length of from.
 * @return
 *	- Realloced buffer containing both to and from.
 *	- NULL on failure. To will still be valid.
 */
char *talloc_bstr_append(TALLOC_CTX *ctx, char *to, char const *from, size_t from_len)
{
	char	*n;
	size_t	to_len = 0;

	if (to) {
		to_len = talloc_array_length(to);
		if (to[to_len - 1] == '\0') to_len--;	/* Inlen should be length of input string */
	}

	n = talloc_realloc_size(ctx, to, to_len + from_len + 1);
	if (!n) {
		fr_strerror_printf("Extending bstr buffer to %zu bytes failed", to_len + from_len + 1);
		return NULL;
	}

	memcpy(n + to_len, from, from_len);
	n[to_len + from_len] = '\0';
	talloc_set_type(n, char);

	return n;
}

/** Trim a bstr (char) buffer
 *
 * Reallocs to inlen + 1 and '\0' terminates the string buffer.
 *
 * @param[in] ctx	to realloc buffer into.
 * @param[in] in	string to trim.  Will be invalid after
 *			this function returns. If NULL a new zero terminated
 *			buffer of inlen bytes will be allocated.
 * @param[in] inlen	Length to trim string to.
 * @return
 *	- The realloced string on success.  in then points to invalid memory.
 *	- NULL on failure. In will still be valid.
 */
char *talloc_bstr_realloc(TALLOC_CTX *ctx, char *in, size_t inlen)
{
	char *n;

	if (!in) {
		n = talloc_array(ctx, char, inlen);
		n[0] = '\0';
		return n;
	}

	n = talloc_realloc_size(ctx, in, inlen + 1);
	if (unlikely(!n)) return NULL;

	n[inlen] = '\0';
	talloc_set_type(n, char);

	return n;
}

/** Concatenate to + from
 *
 * @param[in] ctx	to allocate realloced buffer in.
 * @param[in] to	talloc string buffer to append to.
 * @param[in] from	talloc string buffer to append.
 * @return
 *	- NULL if to or from are NULL or if the realloc fails.
 *	  Note: You'll still need to free to if this function
 *	  returns NULL.
 *	- The concatenation of to + from.  After this function
 *	  returns to may point to invalid memory and should
 *	  not be used.
 */
char *talloc_buffer_append_buffer(TALLOC_CTX *ctx, char *to, char const *from)
{
	size_t to_len, from_len, total_len;
	char *out;

	if (!to || !from) return NULL;

	to_len = talloc_array_length(to);
	from_len = talloc_array_length(from);
	total_len = to_len + (from_len - 1);

	out = talloc_realloc(ctx, to, char, total_len);
	if (!out) return NULL;

	memcpy(out + (to_len - 1), from, from_len);
	out[total_len - 1] = '\0';

	return out;
}

/** Concatenate to + ...
 *
 * @param[in] ctx	to allocate realloced buffer in.
 * @param[in] to	talloc string buffer to append to.
 * @param[in] argc	how many variadic arguments were passed.
 * @param[in] ...	talloc string buffer(s) to append.
 *			Arguments can be NULL to simplify
 *			calling logic.
 * @return
 *	- NULL if to or from are NULL or if the realloc fails.
 *	  Note: You'll still need to free to if this function
 *	  returns NULL.
 *	- The concatenation of to + from.  After this function
 *	  returns to may point to invalid memory and should
 *	  not be used.
 */
char *talloc_buffer_append_variadic_buffer(TALLOC_CTX *ctx, char *to, int argc, ...)
{
	va_list		ap_val, ap_len;
	int		i;

	size_t		to_len, total_len = 0;
	char		*out, *p;

	if (!to) return NULL;

	va_start(ap_val, argc);
	va_copy(ap_len, ap_val);

	total_len += to_len = talloc_array_length(to) - 1;

	/*
	 *	Figure out how much we need to realloc
	 */
	for (i = 0; i < argc; i++) {
		char *arg;

		arg = va_arg(ap_len, char *);
		if (!arg) continue;

		total_len += (talloc_array_length(arg) - 1);
	}

	/*
	 *	It's a noop...
	 */
	if (total_len == to_len) {
		va_end(ap_val);
		va_end(ap_len);
		return to;
	}

	out = talloc_realloc(ctx, to, char, total_len + 1);
	if (!out) goto finish;

	p = out + to_len;

	/*
	 *	Copy the args in
	 */
	for (i = 0; i < argc; i++) {
		char	*arg;
		size_t	len;

		arg = va_arg(ap_val, char *);
		if (!arg) continue;

		len = talloc_array_length(arg) - 1;

		memcpy(p, arg, len);
		p += len;
	}
	*p = '\0';

finish:
	va_end(ap_val);
	va_end(ap_len);

	return out;
}

/** Compares two talloced uint8_t arrays with memcmp
 *
 * Talloc arrays carry their length as part of the structure, so can be passed to a generic
 * comparison function.
 *
 * @param a	Pointer to first array.
 * @param b	Pointer to second array.
 * @return
 *	- 0 if the arrays match.
 *	- a positive or negative integer otherwise.
 */
int talloc_memcmp_array(uint8_t const *a, uint8_t const *b)
{
	size_t a_len, b_len;

	a_len = talloc_array_length(a);
	b_len = talloc_array_length(b);

	if (a_len > b_len) return +1;
	if (a_len < b_len) return -1;

	return memcmp(a, b, a_len);
}

/** Compares two talloced char arrays with memcmp
 *
 * Talloc arrays carry their length as part of the structure, so can be passed to a generic
 * comparison function.
 *
 * @param a	Pointer to first array.
 * @param b	Pointer to second array.
 * @return
 *	- 0 if the arrays match.
 *	- a positive or negative integer otherwise.
 */
int talloc_memcmp_bstr(char const *a, char const *b)
{
	size_t a_len, b_len;

	a_len = talloc_array_length(a);
	b_len = talloc_array_length(b);

	if (a_len > b_len) return +1;
	if (a_len < b_len) return -1;

	return memcmp(a, b, a_len);
}

/** Decrease the reference count on a ptr
 *
 * Ptr will be freed if count reaches zero.
 *
 * This is equivalent to talloc 1.0 behaviour of talloc_free.
 *
 * @param ptr to decrement ref count for.
 * @return
 *	- 0	The memory was freed.
 *	- >0	How many references remain.
 */
int talloc_decrease_ref_count(void const *ptr)
{
	size_t ref_count;
	void *to_free;

	if (!ptr) return 0;

	memcpy(&to_free, &ptr, sizeof(to_free));

	ref_count = talloc_reference_count(to_free);
	if (ref_count == 0) {
		talloc_free(to_free);
	} else {
		talloc_unlink(talloc_parent(ptr), to_free);
	}

	return ref_count;
}

/** Add a NULL pointer to an array of pointers
 *
 * This is needed by some 3rd party libraries which take NULL terminated
 * arrays for arguments.
 *
 * If allocation fails, NULL will be returned and the original array
 * will not be touched.
 *
 * @param[in] array to null terminate.  Will be invalidated (realloced).
 * @return
 *	- NULL if array is NULL, or if reallocation fails.
 *	- A realloced version of array with an additional NULL element.
 */
void **talloc_array_null_terminate(void **array)
{
	size_t		len;
	TALLOC_CTX	*ctx;
	void		**new;
	size_t		size;

	if (!array) return NULL;

	len = talloc_array_length(array);
	ctx = talloc_parent(array);
	size = talloc_get_size(array) / talloc_array_length(array);

	new = _talloc_realloc_array(ctx, array, size, len + 1, talloc_get_name(array));
	if (!new) return NULL;

	new[len] = NULL;

	return new;
}

/** Remove a NULL termination pointer from an array of pointers
 *
 * If the end of the array is not NULL, NULL will be returned (error).
 *
 * @param[in] array to null strip.  Will be invalidated (realloced).
 * @return
 *	- NULL if array is NULL, if terminating element is not NULL, or reallocation fails.
 *	- A realloced version of array without the terminating NULL element.
 */
void **talloc_array_null_strip(void **array)
{
	size_t		len;
	TALLOC_CTX	*ctx;
	void		**new;
	size_t		size;

	if (!array) return NULL;

	len = talloc_array_length(array);
	ctx = talloc_parent(array);
	size = talloc_get_size(array) / talloc_array_length(array);

	if ((len - 1) == 0) return NULL;

	if (array[len - 1] != NULL) return NULL;

	new = _talloc_realloc_array(ctx, array, size, len - 1, talloc_get_name(array));
	if (!new) return NULL;

	return new;
}

/** Concat an array of strings (not NULL terminated), with a string separator
 *
 * @param[out] out	Where to write the resulting string.
 * @param[in] array	of strings to concat.
 * @param[in] sep	to insert between elements.  May be NULL.
 * @return
 *      - >= 0 on success - length of the string created.
 *	- <0 on failure.  How many bytes we would need.
 */
fr_slen_t talloc_array_concat(fr_sbuff_t *out, char const * const *array, char const *sep)
{
	fr_sbuff_t		our_out = FR_SBUFF(out);
	size_t			len = talloc_array_length(array);
	char const * const *	p;
	char const * const *	end;
	fr_sbuff_escape_rules_t	e_rules = {
					.name = __FUNCTION__,
					.chr = '\\'
				};

	if (sep) e_rules.subs[(uint8_t)*sep] = *sep;

	for (p = array, end = array + len;
	     (p < end);
	     p++) {
		if (*p) FR_SBUFF_RETURN(fr_sbuff_in_escape, &our_out, *p, strlen(*p), &e_rules);

		if (sep && ((p + 1) < end)) {
			FR_SBUFF_RETURN(fr_sbuff_in_strcpy, &our_out, sep);
		}
	}

	FR_SBUFF_SET_RETURN(out, &our_out);
}

/** Callback to free the autofree ctx on global exit
 *
 */
static int _autofree_on_exit(void *af)
{
	talloc_set_destructor(af, NULL);
	return talloc_free(af);
}

/** Ensures in the autofree ctx is manually freed, things don't explode atexit
 *
 */
static int _autofree_global_destructor(TALLOC_CTX *af)
{
	return fr_atexit_global_disarm(true, _autofree_on_exit, af);
}

TALLOC_CTX *talloc_autofree_context_global(void)
{
	TALLOC_CTX *af = global_ctx;

	if (!af) {
		af = talloc_init_const("global_autofree_context");
		talloc_set_destructor(af, _autofree_global_destructor);
		if (unlikely(!af)) return NULL;

		fr_atexit_global(_autofree_on_exit, af);
		global_ctx = af;
	}

	return af;
}

/** Ensures in the autofree ctx is manually freed, things don't explode atexit
 *
 */
static int _autofree_thread_local_destructor(TALLOC_CTX *af)
{
	return fr_atexit_thread_local_disarm(true, _autofree_on_exit, af);
}

/** Get a thread-safe autofreed ctx that will be freed when the thread or process exits
 *
 * @note This should be used in place of talloc_autofree_context (which is now deprecated)
 * @note Will not be thread-safe if NULL tracking is enabled.
 *
 * @return A talloc ctx which will be freed when the thread or process exits.
 */
TALLOC_CTX *talloc_autofree_context_thread_local(void)
{
	TALLOC_CTX *af = thread_local_ctx;

	if (!af) {
		af = talloc_init_const("thread_local_autofree_context");
		talloc_set_destructor(af, _autofree_thread_local_destructor);
		if (unlikely(!af)) return NULL;

		fr_atexit_thread_local(thread_local_ctx, _autofree_on_exit, af);
	}

	return af;
}


struct talloc_child_ctx_s {
	struct talloc_child_ctx_s *next;
};

static int _child_ctx_free(TALLOC_CHILD_CTX *list)
{
	while (list->next != NULL) {
		TALLOC_CHILD_CTX *entry = list->next;
		TALLOC_CHILD_CTX *next = entry->next;

		if (talloc_free(entry) < 0) return -1;

		list->next = next;
	}

	return 0;
}

/** Allocate and initialize a TALLOC_CHILD_CTX
 *
 *  The TALLOC_CHILD_CTX ensures ordering for allocators and
 *  destructors.  When a TALLOC_CHILD_CTX is created, it is added to
 *  parent, in LIFO order.  In contrast, the basic talloc operations
 *  do not guarantee any kind of order.
 *
 *  When the TALLOC_CHILD_CTX is freed, the children are freed in FILO
 *  order.  That process ensures that the children are freed before
 *  the parent, and that the younger siblings are freed before the
 *  older siblings.
 *
 *  The idea is that if we have an initializer for A, which in turn
 *  initializes B and C.  When the memory is freed, we should do the
 *  operations in the reverse order.
 */
TALLOC_CHILD_CTX *talloc_child_ctx_init(TALLOC_CTX *ctx)
{
	TALLOC_CHILD_CTX *child;

	child = talloc_zero(ctx, TALLOC_CHILD_CTX);
	if (!child) return NULL;

	talloc_set_destructor(child, _child_ctx_free);
	return child;
}

/** Allocate a TALLOC_CHILD_CTX from a parent.
 *
 */
TALLOC_CHILD_CTX *talloc_child_ctx_alloc(TALLOC_CHILD_CTX *parent)
{
	TALLOC_CHILD_CTX *child;

	child = talloc(parent, TALLOC_CHILD_CTX);
	if (!child) return NULL;

	child->next = parent->next;
	parent->next = child;
	return child;
}
