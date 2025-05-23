/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/** Deal with 'unknown' attributes, creating ephemeral dictionary attributes for them
 *
 * @file src/lib/util/dict_unknown.c
 *
 * @copyright 2019 The FreeRADIUS server project
 */
RCSID("$Id$")

#include <freeradius-devel/util/dict_priv.h>

/** Converts an unknown to a known by adding it to the internal dictionaries.
 *
 * Does not free old #fr_dict_attr_t, that is left up to the caller.
 *
 * @param[in] dict		of protocol context we're operating in.
 *				If NULL the internal dictionary will be used.
 * @param[in] unknown		attribute to add.
 * @return
 *	- Existing #fr_dict_attr_t if unknown was found in a dictionary.
 *	- A new entry representing unknown.
 */
fr_dict_attr_t const *fr_dict_attr_unknown_add(fr_dict_t *dict, fr_dict_attr_t const *unknown)
{
	fr_dict_attr_t const *da;
	fr_dict_attr_t const *parent;
	fr_dict_attr_flags_t flags;

	if (unlikely(dict->read_only)) {
		fr_strerror_printf("%s dictionary has been marked as read only", fr_dict_root(dict)->name);
		return NULL;
	}

#ifdef STATIC_ANALYZER
	if (!unknown->name || !unknown->parent) return NULL;
#endif

	da = fr_dict_attr_by_name(NULL, unknown->parent, unknown->name);
	if (da) {
		if (da->attr == unknown->attr) return da;

		fr_strerror_printf("Unknown attribute '%s' conflicts with existing attribute in namespace '%s'",
				   da->name, unknown->parent->name);
		return da;
	}

	/*
	 *	Define the complete unknown hierarchy
	 */
	if (unknown->parent && unknown->parent->flags.is_unknown) {
		parent = fr_dict_attr_unknown_add(dict, unknown->parent);
		if (!parent) {
			fr_strerror_printf_push("Failed adding parent \"%s\"", unknown->parent->name);
			return NULL;
		}
	} else {
		parent = unknown->parent;
	}

	memcpy(&flags, &unknown->flags, sizeof(flags));
	flags.is_unknown = 0;

	/*
	 *	If this is a vendor, we skip most of the sanity
	 *	checks and add it to the vendor hash, and add it
	 *	as a child attribute to the Vendor-Specific
	 *	container.
	 */
	if (unknown->type == FR_TYPE_VENDOR) {
		fr_dict_attr_t *n;

		if (dict_vendor_add(dict, unknown->name, unknown->attr) < 0) return NULL;

		n = dict_attr_alloc(dict->pool, parent, unknown->name, unknown->attr, unknown->type,
				    &(dict_attr_args_t){ .flags = &flags });
		if (unlikely(!n)) return NULL;

		/*
		 *	Setup parenting for the attribute
		 */
		if (dict_attr_child_add(UNCONST(fr_dict_attr_t *, unknown->parent), n) < 0) return NULL;

		return n;
	}

	/*
	 *	Look up the attribute by number.  If it doesn't exist,
	 *	add it both by name and by number.  If it does exist,
	 *	add it only by name.
	 */
	da = fr_dict_attr_child_by_num(parent, unknown->attr);
	if (da) {
		fr_dict_attr_t *n;

		n = dict_attr_alloc(dict->pool, parent, unknown->name, unknown->attr, unknown->type,
				    &(dict_attr_args_t){ .flags = &flags });
		if (!n) return NULL;

		/*
		 *	Add the unknown by NAME.  e.g. if the admin does "Attr-26", we want
		 *	to return "Attr-26", and NOT "Vendor-Specific".  The rest of the server
		 *	is responsible for converting "Attr-26 = 0x..." to an actual attribute,
		 *	if it so desires.
		 */
		if (dict_attr_add_to_namespace(parent, n) < 0) {
			talloc_free(n);
			return NULL;
		}

		return n;
	}

	/*
	 *	Add the attribute by both name and number.
	 *
	 *	Fixme - Copy extensions?
	 */
	if (fr_dict_attr_add(dict, parent, unknown->name, unknown->attr, unknown->type, &flags) < 0) return NULL;

	/*
	 *	For paranoia, return it by name.
	 */
	return fr_dict_attr_by_name(NULL, parent, unknown->name);
}

/** Free dynamically allocated (unknown attributes)
 *
 * If the da was dynamically allocated it will be freed, else the function
 * will return without doing anything.
 *
 * @param[in] da to free.
 */
void fr_dict_attr_unknown_free(fr_dict_attr_t const **da)
{
	if (!da || !*da) return;

	/* Don't free real DAs */
	if (!(*da)->flags.is_unknown) {
		return;
	}

	talloc_const_free(*da);

	*da = NULL;
}

/**  Allocate an unknown DA.
 *
 */
static fr_dict_attr_t *dict_unknown_alloc(TALLOC_CTX *ctx, fr_dict_attr_t const *da, fr_type_t type)
{
	fr_dict_attr_t		*n;
	fr_dict_attr_t const	*parent;
	fr_dict_attr_flags_t	flags = da->flags;

	fr_assert(!da->flags.is_root); /* cannot copy root attributes */

	/*
	 *	Set the unknown flag, and copy only those other flags
	 *	which we know to be correct.
	 */
	flags.is_unknown = 1;
	flags.is_raw = 1;
	flags.array = 0;
	flags.has_value = 0;
	if (type != FR_TYPE_VENDOR) {
		flags.length = 0;	/* not fixed length */
	} else {
		flags.type_size = da->flags.type_size;
		flags.length = da->flags.length;
	}
	flags.extra = 0;

	/*
	 *	Allocate an attribute.
	 */
	n = dict_attr_alloc_null(ctx, da->dict->proto);
	if (!n) return NULL;

	/*
	 *	We want to have parent / child relationships, AND to
	 *	copy all unknown parents, AND to free the unknown
	 *	parents when this 'da' is freed.  We therefore talloc
	 *	the parent from the 'da'.
	 */
	if (da->parent && da->parent->flags.is_unknown) {
		parent = fr_dict_attr_unknown_copy(n, da->parent);
		if (!parent) {
		error:
			talloc_free(n);
			return NULL;
		}

	} else {
		parent = da->parent;
	}

	/*
	 *	Initialize the rest of the fields.
	 */
	if (dict_attr_init(&n, parent, da->name, da->attr, type, &(dict_attr_args_t){ .flags = &flags }) < 0) {
		goto error;
	}
	if (type != FR_TYPE_OCTETS) {
		dict_attr_ext_copy_all(&n, da);

	} else if (fr_dict_attr_ext(da, FR_DICT_ATTR_EXT_PROTOCOL_SPECIFIC) &&
		   !dict_attr_ext_copy(&n, da, FR_DICT_ATTR_EXT_PROTOCOL_SPECIFIC)) {
		goto error;
	}
	DA_VERIFY(n);

	return n;
}

/** Copy a known or unknown attribute to produce an unknown attribute with the specified name
 *
 * Will copy the complete hierarchy down to the first known attribute.
 */
fr_dict_attr_t *fr_dict_attr_unknown_afrom_da(TALLOC_CTX *ctx, fr_dict_attr_t const *da)
{
	fr_type_t type = da->type;

	/*
	 *	VENDOR, etc. are logical containers, and can have
	 *	unknown children, so they're left alone.  All other
	 *	base types are mangled to OCTETs.
	 *
	 *	Note that we can't allocate an unknown STRUCT.  If the
	 *	structure is malformed, then it's just a sequence of
	 *	OCTETS.  Similarly, if a GROUP is malformed, then we
	 *	have no idea what's inside of it, and we make it OCTETS.
	 */
	switch (type) {
	case FR_TYPE_VENDOR:
		fr_assert(da->flags.type_size != 0);
		break;

	case FR_TYPE_TLV:
	case FR_TYPE_VSA:
		break;

	default:
		type = FR_TYPE_OCTETS;
		break;
	}

	return dict_unknown_alloc(ctx, da, type);
}

/** Initialise a fr_dict_attr_t from a number and a data type
 *
 * @param[in] ctx		to allocate the attribute in.
 * @param[in] parent		of the unknown attribute (may also be unknown).
 * @param[in] num		of the unknown attribute.
 * @param[in] type		data type
 * @param[in] raw		is it raw, i.e. _bad_ value, versus unknown?
 * @return
 *	- An fr_dict_attr_t on success.
 *	- NULL on failure.
 */
fr_dict_attr_t *fr_dict_attr_unknown_typed_afrom_num_raw(TALLOC_CTX *ctx, fr_dict_attr_t const *parent, unsigned int num, fr_type_t type, bool raw)
{
	fr_dict_attr_flags_t	flags = {
					.is_unknown = true,
					.internal = parent->flags.internal,
					.is_raw = raw,
				};

	switch (type) {
	case FR_TYPE_VENDOR:
		if (parent->type != FR_TYPE_VSA) goto fail;

		if (!fr_cond_assert(!parent->flags.is_unknown)) return NULL;

		/*
		 *	These can be reset later if needed.  But these
		 *	values are most common.
		 */
		flags.type_size = 1;
		flags.length = 1;
		break;

	case FR_TYPE_NULL:
	case FR_TYPE_VALUE_BOX:
	case FR_TYPE_VOID:
	case FR_TYPE_MAX:
		fr_strerror_printf("%s: Cannot allocate unknown %s attribute (%u) - invalid data type",
				   __FUNCTION__,
				   fr_type_to_str(type), num);
		return NULL;

	default:
		if (fr_dict_attr_is_key_field(parent)) break;

		if (!fr_type_is_structural_except_vsa(parent->type)) {
		fail:
			fr_strerror_printf("%s: Cannot allocate unknown %s attribute (%u) with parent type %s",
					   __FUNCTION__,
					   fr_type_to_str(type), num,
					   fr_type_to_str(parent->type));
			return NULL;
		}

		/*
		 *	We can convert anything to 'octets'.  But we shouldn't be able to create a raw
		 *	attribute which is a _different_ type than an existing one.
		 */
		if (type != FR_TYPE_OCTETS) {
			fr_dict_attr_t const *child;

			child = fr_dict_attr_child_by_num(parent, num);
			if (child && (child->type != type)) {
				fr_strerror_printf("%s: Cannot allocate unknown attribute (%u) which changes type from %s to %s",
						   __FUNCTION__,
						   num,
						   fr_type_to_str(child->type),
						   fr_type_to_str(type));
				return NULL;
			}
		}

		break;
	}

	if (parent->depth >= FR_DICT_MAX_TLV_STACK) {
		fr_strerror_const("Attribute depth is too large");
		return NULL;
	}

	return dict_attr_alloc(ctx, parent, NULL, num, type,
			       &(dict_attr_args_t){ .flags = &flags });
}

/** Initialise an octets type attribute from a da
 *
 * @param[in] ctx		to allocate the attribute in.
 * @param[in] da		of the unknown attribute.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
fr_dict_attr_t	*fr_dict_attr_unknown_raw_afrom_da(TALLOC_CTX *ctx, fr_dict_attr_t const *da)
{
	return dict_unknown_alloc(ctx, da, FR_TYPE_OCTETS);
}

/** Create a fr_dict_attr_t from an ASCII attribute and value
 *
 * Where the attribute name is in the form:
 *  - %d
 *  - %d.%d.%d...
 *
 * @note If vendor != 0, an unknown vendor (may) also be created, parented by
 *	the correct VSA attribute. This is accessible via vp->parent,
 *	and will be use the unknown da as its talloc parent.
 *
 * @param[in] ctx		to alloc new attribute in.
 * @param[out] out		Where to write the head of the chain unknown
 *				dictionary attributes.
 * @param[in] parent		Attribute to use as the root for resolving OIDs in.
 *				Usually the root of a protocol dictionary.
 * @param[in] in		OID string to parse
 * @param[in] type		data type of the unknown attribute
 * @return
 *	- The number of bytes parsed on success.
 *	- <= 0 on failure.  Negative offset indicates parse error position.
 */
fr_slen_t fr_dict_attr_unknown_afrom_oid_substr(TALLOC_CTX *ctx,
					   fr_dict_attr_t const **out,
			      		   fr_dict_attr_t const *parent,
					   fr_sbuff_t *in, fr_type_t type)
{
	fr_sbuff_t		our_in = FR_SBUFF(in);
	fr_dict_attr_t const	*our_parent = parent;
	fr_dict_attr_t		*n = NULL;
	fr_dict_attr_flags_t	flags = {
					.is_unknown = true,
					.is_raw = true,
					.type_size = parent->dict->root->flags.type_size,
					.length = parent->dict->root->flags.length,
				};

	*out = NULL;

	/*
	 *	Allocate the final attribute first, so that any
	 *	unknown parents can be freed when this da is freed.
	 *
	 *      See fr_dict_attr_unknown_afrom_da() for more details.
	 *
	 *	Note also that we copy the input name, even if it is
	 *	not normalized.
	 *
	 *	While the name of this attribute is "Attr-#.#.#", one
	 *	or more of the leading components may, in fact, be
	 *	known.
	 */
	n = dict_attr_alloc_null(ctx, parent->dict->proto);

	/*
	 *	Loop until there's no more component separators.
	 */
	for (;;) {
		uint32_t		num;
		fr_sbuff_parse_error_t	sberr;

		fr_sbuff_out(&sberr, &num, &our_in);
		switch (sberr) {
		case FR_SBUFF_PARSE_OK:
			switch (our_parent->type) {
			/*
			 *	If the parent is a VSA, this component
			 *	must specify a vendor.
			 */
			case FR_TYPE_VSA:
			{
				fr_dict_attr_t	*ni;

				if (fr_sbuff_next_if_char(&our_in, '.')) {
					ni = fr_dict_attr_unknown_vendor_afrom_num(n, our_parent, num);
					if (!ni) {
					error:
						talloc_free(n);
						FR_SBUFF_ERROR_RETURN(&our_in);
					}
					our_parent = ni;
					continue;
				}
				if (dict_attr_init(&n, our_parent, NULL, num, FR_TYPE_VENDOR,
						   &(dict_attr_args_t){ .flags = &flags }) < 0) goto error;
			}
				break;

			/*
			 *	If it's structural, this component must
			 *	specify a TLV.
			 */
			case FR_TYPE_STRUCTURAL_EXCEPT_VSA:
			{
				fr_dict_attr_t	*ni;

				if (fr_sbuff_next_if_char(&our_in, '.')) {
					ni = fr_dict_attr_unknown_typed_afrom_num(n, our_parent, num, FR_TYPE_TLV);
					if (!ni) goto error;
					our_parent = ni;
					continue;
				}
			}
				FALL_THROUGH;

			default:
				/*
				 *	Leaf type with more components
				 *	is an error.
				 */
				if (fr_sbuff_is_char(&our_in, '.')) {
					fr_strerror_printf("Interior OID component cannot proceed a %s type",
							   fr_type_to_str(our_parent->type));
					goto error;
				}
				if (dict_attr_init(&n, our_parent, NULL, num, type,
						   &(dict_attr_args_t){ .flags = &flags }) < 0) goto error;
				break;
			}
			break;

		default:
		{
			fr_sbuff_marker_t c_start;

			fr_sbuff_marker(&c_start, &our_in);
			fr_sbuff_adv_past_allowed(&our_in, FR_DICT_ATTR_MAX_NAME_LEN, fr_dict_attr_allowed_chars, NULL);
			fr_strerror_printf("Unknown attribute \"%.*s\" for parent \"%s\"",
					   (int)fr_sbuff_behind(&c_start), fr_sbuff_current(&c_start), our_parent->name);
			goto error;
		}
		}
		break;
	}

	DA_VERIFY(n);

	*out = n;

	FR_SBUFF_SET_RETURN(in, &our_in);
}

/** Fixup the parent of an unknown attribute using an equivalent known attribute
 *
 * This can be useful where an unknown attribute's ancestors are added to
 * a dictionary but not the unknown attribute itself.
 *
 * @param[in] da	to fixup.
 * @param[in] parent	to assign.  If NULL, we will attempt to resolve
 *			the parent in the dictionary the current unknown
 *			attribute extends.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int fr_dict_attr_unknown_parent_to_known(fr_dict_attr_t *da, fr_dict_attr_t const *parent)
{
	fr_dict_attr_t const *da_u, *da_k;

	if (parent) {
		/*
		 *	Walk back up the hierarchy until we get to a known
		 *	ancestor on the unknown side.
		 */
		for (da_u = da->parent, da_k = parent;
		     da_k && da_u && da_u->flags.is_unknown;
		     da_u = da_u->parent, da_k = da_k->parent) {
			if (unlikely(da_u->attr != da_k->attr)) {
				fr_strerror_printf("Unknown parent number %u does not match "
						   "known parent number %u (%s)",
						   da_u->attr, da_k->attr, da_k->name);
				return -1;
			}

			if (unlikely(da_u->depth != da_k->depth)) {
				fr_strerror_printf("Unknown parent depth %u does not match "
						   "known parent depth %u (%s)",
						   da_u->depth, da_k->depth, da_k->name);
				return -1;
			}
		}
		if ((da_k == NULL) != (da_u == NULL)) {
			fr_strerror_printf("Truncated or over-extended hierarchy "
					   "for unknown attribute %u", da->attr);
			return -1;
		}
	} else {
		parent = fr_dict_attr_unknown_resolve(fr_dict_by_da(da), da->parent);
		if (!parent) {
			fr_strerror_printf("Failed resolving unknown attribute %u "
					   "in dictionary", da->attr);
			return -1;
		}
	}

	da->parent = fr_dict_attr_unconst(parent);

	return 0;
}

/** Check to see if we can convert a nested TLV structure to known attributes
 *
 * @param[in] dict			to search in.
 * @param[in] da			Nested tlv structure to convert.
 * @return
 *	- NULL if we can't.
 *	- Known attribute if we can.
 */
fr_dict_attr_t const *fr_dict_attr_unknown_resolve(fr_dict_t const *dict, fr_dict_attr_t const *da)
{
	INTERNAL_IF_NULL(dict, NULL);

	if (!da->flags.is_unknown) return da;	/* It's known */

	if (da->parent) {
		fr_dict_attr_t const *parent;

		parent = fr_dict_attr_unknown_resolve(dict, da->parent);
		if (!parent) return NULL;

		return fr_dict_attr_child_by_num(parent, da->attr);
	}

	if (dict->root == da) return dict->root;
	return NULL;
}
