/* radare - LGPL - Copyright 2009-2018 - pancake, maijin, thestr4ng3r */

#include "r_anal.h"

#define NAME_BUF_SIZE    64
#define BASE_CLASSES_MAX 32


typedef struct rtti_complete_object_locator_t {
	ut32 signature;
	ut32 vtable_offset;         // offset of the vtable within class
	ut32 cd_offset;             // constructor displacement offset
	ut32 type_descriptor_addr;  // only a relative offset for 64bit
	ut32 class_descriptor_addr; // only a relative offset for 64bit
	ut32 object_base;           // only for 64bit, see rtti_msvc_read_complete_object_locator()
} rtti_complete_object_locator;


typedef struct rtti_class_hierarchy_descriptor_t {
	ut32 signature;
	ut32 attributes;            // bit 0 set = multiple inheritance, bit 1 set = virtual inheritance
	ut32 num_base_classes;
	ut32 base_class_array_addr; // only a relative offset for 64bit
} rtti_class_hierarchy_descriptor;

typedef struct rtti_base_class_descriptor_t {
	ut32 type_descriptor_addr;  // only a relative offset for 64bit
	ut32 num_contained_bases;
	struct {
		st32 mdisp;             // member displacement
		st32 pdisp;             // vbtable displacement
		st32 vdisp;             // displacement inside vbtable
	} where;
	ut32 attributes;
} rtti_base_class_descriptor;

typedef struct rtti_type_descriptor_t {
	ut64 vtable_addr;
	ut64 spare;
	char *name;
} rtti_type_descriptor;

static void rtti_type_descriptor_fini(rtti_type_descriptor *td) {
	free (td->name);
	td->name = NULL;
}

static inline ut64 rtti_msvc_addr(RVTableContext *context, ut64 col_addr, ut64 col_base, ut32 addr) {
	if (context->word_size != 8) {
		return addr;
	}
	return addr + (col_addr - col_base);
}

static bool rtti_msvc_read_complete_object_locator(RVTableContext *context, ut64 addr, rtti_complete_object_locator *col) {
	if (addr == UT64_MAX) {
		return false;
	}

	ut8 buf[6 * sizeof (ut32)];
	int colSize = 5 * sizeof (ut32);
	if (context->word_size == 8) {
		colSize += sizeof(ut32);
	}
	if (colSize > sizeof (buf)) {
		return false;
	}

	if (!context->anal->iob.read_at (context->anal->iob.io, addr, buf, colSize)) {
		return false;
	}

	ut32 (*read_at_32)(const void *src, size_t offset) = context->anal->big_endian ? r_read_at_be32 : r_read_at_le32;
	col->signature = read_at_32 (buf, 0);
	col->vtable_offset = read_at_32 (buf, 4);
	col->cd_offset = read_at_32 (buf, 8);

	int offsetSize = R_MIN (context->word_size, 4);
	col->type_descriptor_addr = (ut32) r_read_ble (buf + 12, (bool) context->anal->big_endian, offsetSize * 8);
	col->class_descriptor_addr = (ut32) r_read_ble (buf + 12 + offsetSize, (bool) context->anal->big_endian, offsetSize * 8);
	if (context->word_size == 8) {
		// 64bit is special:
		// Type Descriptor and Class Hierarchy Descriptor addresses are computed
		// by 32 bit values *(col+12) + *(col+0x14)
		// and *(col+16) + *(col+0x14) respectively
		col->object_base = read_at_32 (buf, 20);
	} else {
		col->object_base = 0;
	}

	return true;
}

static bool rtti_msvc_read_class_hierarchy_descriptor(RVTableContext *context, ut64 addr, rtti_class_hierarchy_descriptor *chd) {
	if (addr == UT64_MAX) {
		return false;
	}

	ut8 buf[4 * sizeof (ut32)];
	int chdSize = 3 * sizeof (ut32) + R_MIN (4, context->word_size);
	if (chdSize > sizeof (buf)) {
		return false;
	}

	if (!context->anal->iob.read_at (context->anal->iob.io, addr, buf, chdSize)) {
		return false;
	}

	ut32 (*read_at_32)(const void *src, size_t offset) = context->anal->big_endian ? r_read_at_be32 : r_read_at_le32;
	chd->signature = read_at_32 (buf, 0);
	chd->attributes = read_at_32 (buf, 4);
	chd->num_base_classes = read_at_32 (buf, 8);
	if (context->word_size <= 4) {
		chd->base_class_array_addr = (ut32) r_read_ble (buf + 12, (bool) context->anal->big_endian, context->word_size * 8);
	} else {
		// 64bit is special, like in Complete Object Locator.
		// Only the offset from the base from Complete Object Locator
		// is contained in Class Hierarchy Descriptor
		chd->base_class_array_addr = read_at_32 (buf, 12);
	}
	return true;
}

static ut64 rtti_msvc_base_class_descriptor_size(RVTableContext *context) {
	return context->word_size + 5 * sizeof (ut32);
}

static bool rtti_msvc_read_base_class_descriptor(RVTableContext *context, ut64 addr, rtti_base_class_descriptor *bcd) {
	if (addr == UT64_MAX) {
		return false;
	}

	ut8 buf[sizeof (ut64) + 5 * sizeof (ut32)];
	int bcdSize = (int) rtti_msvc_base_class_descriptor_size (context);
	if (bcdSize > sizeof (buf)) {
		return false;
	}

	if (!context->anal->iob.read_at (context->anal->iob.io, addr, buf, bcdSize)) {
		return false;
	}

	ut32 (*read_at_32)(const void *src, size_t offset) = context->anal->big_endian ? r_read_at_be32 : r_read_at_le32;
	int typeDescriptorAddrSize = R_MIN (context->word_size, 4);
	bcd->type_descriptor_addr = (ut32) r_read_ble (buf, (bool) context->anal->big_endian, typeDescriptorAddrSize * 8);
	size_t offset = (size_t) typeDescriptorAddrSize;
	bcd->num_contained_bases = read_at_32 (buf, offset);
	bcd->where.mdisp = read_at_32 (buf, offset + sizeof (ut32));
	bcd->where.pdisp = read_at_32 (buf, offset + 2 * sizeof (ut32));
	bcd->where.vdisp = read_at_32 (buf, offset + 3 * sizeof (ut32));
	bcd->attributes = read_at_32 (buf, offset + 4 * sizeof (ut32));
	return true;
}

static RList *rtti_msvc_read_base_class_array(RVTableContext *context, ut32 num_base_classes, ut64 base, ut32 offset) {
	if (base == UT64_MAX || offset == UT32_MAX || num_base_classes == UT32_MAX) {
		return NULL;
	}

	RList *ret = r_list_newf (free);
	if (!ret) {
		return NULL;
	}

	ut64 addr = base + offset;
	ut64 stride = R_MIN (context->word_size, 4);

	if (num_base_classes > BASE_CLASSES_MAX) {
		eprintf ("WARNING: Length of base class array at 0x%08"PFMT64x" exceeds %d.\n", addr, BASE_CLASSES_MAX);
		num_base_classes = BASE_CLASSES_MAX;
	}

	r_cons_break_push (NULL, NULL);
	while (num_base_classes > 0) {
		if (r_cons_is_breaked ()) {
			break;
		}

		ut64 bcdAddr;
		if (context->word_size <= 4) {
			if (!context->read_addr (context->anal, addr, &bcdAddr)) {
				break;
			}
			if (bcdAddr == UT32_MAX) {
				break;
			}
		} else {
			// special offset calculation for 64bit
			ut8 tmp[4] = {0};
			if (!context->anal->iob.read_at(context->anal->iob.io, addr, tmp, 4)) {
				r_list_free (ret);
				return NULL;
			}
			ut32 (*read_32)(const void *src) = context->anal->big_endian ? r_read_be32 : r_read_le32;
			ut32 bcdOffset = read_32 (tmp);
			if (bcdOffset == UT32_MAX) {
				break;
			}
			bcdAddr = base + bcdOffset;
		}

		rtti_base_class_descriptor *bcd = malloc (sizeof (rtti_base_class_descriptor));
		if (!bcd) {
			break;
		}
		if (!rtti_msvc_read_base_class_descriptor (context, bcdAddr, bcd)) {
			free (bcd);
			break;
		}
		r_list_append (ret, bcd);
		addr += stride;
		num_base_classes--;
	}
	r_cons_break_pop ();

	if (num_base_classes > 0) {
		// there was an error in the loop above
		r_list_free (ret);
		return NULL;
	}

	return ret;
}

static bool rtti_msvc_read_type_descriptor(RVTableContext *context, ut64 addr, rtti_type_descriptor *td) {
	if (addr == UT64_MAX) {
		return false;
	}

	if (!context->read_addr (context->anal, addr, &td->vtable_addr)) {
		return false;
	}
	if (!context->read_addr (context->anal, addr + context->word_size, &td->spare)) {
		return false;
	}

	ut64 nameAddr = addr + 2 * context->word_size;
	ut8 buf[NAME_BUF_SIZE];
	ut64 bufOffset = 0;
	size_t nameLen = 0;
	bool endFound = false;
	bool endInvalid = false;
	while (1) {
		context->anal->iob.read_at (context->anal->iob.io, nameAddr + bufOffset, buf, sizeof (buf));
		int i;
		for (i=0; i<sizeof (buf); i++) {
			if (buf[i] == '\0') {
				endFound = true;
				break;
			}
			if (buf[i] == 0xff) {
				endInvalid = true;
				break;
			}
			nameLen++;
		}
		if (endFound || endInvalid) {
			break;
		}
		bufOffset += sizeof (buf);
	}

	if (endInvalid) {
		return false;
	}

	td->name = malloc (nameLen + 1);
	if (!td->name) {
		return false;
	}

	if (bufOffset == 0) {
		memcpy (td->name, buf, nameLen + 1);
	} else {
		context->anal->iob.read_at (context->anal->iob.io, nameAddr,
									(ut8 *)td->name, (int) (nameLen + 1));
	}

	return true;
}

static void rtti_msvc_print_complete_object_locator(rtti_complete_object_locator *col, ut64 addr, const char *prefix) {
	r_cons_printf ("%sComplete Object Locator at 0x%08"PFMT64x":\n"
				   "%s\tsignature: %#x\n"
				   "%s\tvftableOffset: %#x\n"
				   "%s\tcdOffset: %#x\n"
				   "%s\ttypeDescriptorAddr: 0x%08"PFMT32x"\n"
				   "%s\tclassDescriptorAddr: 0x%08"PFMT32x"\n",
				   prefix, addr,
				   prefix, col->signature,
				   prefix, col->vtable_offset,
				   prefix, col->cd_offset,
				   prefix, col->type_descriptor_addr,
				   prefix, col->class_descriptor_addr);
	r_cons_printf ("%s\tobjectBase: 0x%08"PFMT32x"\n\n",
				   prefix, col->object_base);
}

static void rtti_msvc_print_complete_object_locator_json(rtti_complete_object_locator *col) {
	r_cons_printf ("{\"signature\":%"PFMT32u",\"vftable_offset\":%"PFMT32u",\"cd_offset\":%"PFMT32u","
				   "\"type_desc_addr\":%"PFMT32u",\"class_desc_addr\":%"PFMT32u",\"object_base\":%"PFMT32u"}",
				   col->signature, col->vtable_offset, col->cd_offset, col->type_descriptor_addr,
				   col->class_descriptor_addr, col->object_base);
}

static void rtti_msvc_print_type_descriptor(rtti_type_descriptor *td, ut64 addr, const char *prefix) {
	r_cons_printf ("%sType Descriptor at 0x%08"PFMT64x":\n"
				   "%s\tvtableAddr: 0x%08"PFMT64x"\n"
				   "%s\tspare: 0x%08"PFMT64x"\n"
				   "%s\tname: %s\n\n",
				   prefix, addr,
				   prefix, td->vtable_addr,
				   prefix, td->spare,
				   prefix, td->name);
}

static void rtti_msvc_print_type_descriptor_json(rtti_type_descriptor *td) {
	r_cons_printf ("{\"vtable_addr\":%"PFMT32u",\"spare\":%"PFMT32u",\"name\":\"%s\"}",
				   td->vtable_addr, td->spare, td->name);
}

static void rtti_msvc_print_class_hierarchy_descriptor(rtti_class_hierarchy_descriptor *chd, ut64 addr, const char *prefix) {
	r_cons_printf ("%sClass Hierarchy Descriptor at 0x%08"PFMT64x":\n"
				   "%s\tsignature: %#x\n"
				   "%s\tattributes: %#x\n"
				   "%s\tnumBaseClasses: %#x\n"
				   "%s\tbaseClassArrayAddr: 0x%08"PFMT32x"\n\n",
				   prefix, addr,
				   prefix, chd->signature,
				   prefix, chd->attributes,
				   prefix, chd->num_base_classes,
				   prefix, chd->base_class_array_addr);
}

static void rtti_msvc_print_class_hierarchy_descriptor_json(rtti_class_hierarchy_descriptor *chd) {
	r_cons_printf ("{\"signature\":%"PFMT32u",\"attributes\":%"PFMT32u",\"num_base_classes\":%"PFMT32u","
				   "\"base_class_array_addr\":%"PFMT32u"}",
				   chd->signature, chd->attributes, chd->num_base_classes, chd->base_class_array_addr);
}

static void rtti_msvc_print_base_class_descriptor(rtti_base_class_descriptor *bcd, const char *prefix) {
	r_cons_printf ("%sBase Class Descriptor:\n"
				   "%s\ttypeDescriptorAddr: 0x%08"PFMT32x"\n"
				   "%s\tnumContainedBases: %#x\n"
				   "%s\twhere:\n"
				   "%s\t\tmdisp: %d\n"
				   "%s\t\tpdisp: %d\n"
				   "%s\t\tvdisp: %d\n"
				   "%s\tattributes: %#x\n\n",
				   prefix,
				   prefix, bcd->type_descriptor_addr,
				   prefix, bcd->num_contained_bases,
				   prefix,
				   prefix, bcd->where.mdisp,
				   prefix, bcd->where.pdisp,
				   prefix, bcd->where.vdisp,
				   prefix, bcd->attributes);
}

static void rtti_msvc_print_base_class_descriptor_json(rtti_base_class_descriptor *bcd) {
	r_cons_printf ("{\"type_desc_addr\":%"PFMT32u",\"num_contained_bases\":%"PFMT32u","
				   "\"where\":{\"mdisp\":%"PFMT32d",\"pdisp\":%"PFMT32d",\"vdisp\":%"PFMT32d"},"
				   "\"attributes\":%"PFMT32u"}",
				   bcd->type_descriptor_addr, bcd->num_contained_bases,
				   bcd->where.mdisp, bcd->where.pdisp, bcd->where.vdisp, bcd->attributes);
}


/**
 * Demangle a class name as found in MSVC RTTI type descriptors.
 *
 * Examples:
 * .?AVClassA@@
 * => ClassA
 * .?AVClassInInnerNamespace@InnerNamespace@OuterNamespace@@
 * => OuterNamespace::InnerNamespace::AVClassInInnerNamespace
 */
R_API char *r_anal_rtti_msvc_demangle_class_name(const char *name) {
	if (!name) {
		return NULL;
	}
	size_t original_len = strlen (name);
	if (original_len < 7
		|| (strncmp (name, ".?AV", 4) != 0 && strncmp (name, ".?AU", 4) != 0)
		|| strncmp (name + original_len - 2, "@@", 2) != 0) {
		return NULL;
	}
	char *ret = malloc ((original_len - 6) * 2 + 1);
	if (!ret) {
		return NULL;
	}
	char *c = ret;
	const char *oc = name + original_len - 3;
	size_t part_len = 0;
	while (oc >= name + 4) {
		if (*oc == '@') {
			memcpy (c, oc + 1, part_len);
			c += part_len;
			*c++ = ':';
			*c++ = ':';
			part_len = 0;
		} else {
			part_len++;
		}
		oc--;
	}
	memcpy (c, oc + 1, part_len);
	c[part_len] = '\0';
	return ret;
}

R_API void r_anal_rtti_msvc_print_complete_object_locator(RVTableContext *context, ut64 addr, int mode) {
	rtti_complete_object_locator col;
	if (!rtti_msvc_read_complete_object_locator (context, addr, &col)) {
		eprintf ("Failed to parse Complete Object Locator at 0x%08"PFMT64x"\n", addr);
		return;
	}

	if (mode == 'j') {
		rtti_msvc_print_complete_object_locator_json (&col);
	} else {
		rtti_msvc_print_complete_object_locator (&col, addr, "");
	}
}

R_API void r_anal_rtti_msvc_print_type_descriptor(RVTableContext *context, ut64 addr, int mode) {
	rtti_type_descriptor td = { 0 };
	if (!rtti_msvc_read_type_descriptor (context, addr, &td)) {
		eprintf ("Failed to parse Type Descriptor at 0x%08"PFMT64x"\n", addr);
		return;
	}

	if (mode == 'j') {
		rtti_msvc_print_type_descriptor_json (&td);
	} else {
		rtti_msvc_print_type_descriptor (&td, addr, "");
	}

	rtti_type_descriptor_fini (&td);
}

R_API void r_anal_rtti_msvc_print_class_hierarchy_descriptor(RVTableContext *context, ut64 addr, int mode) {
	rtti_class_hierarchy_descriptor chd;
	if (!rtti_msvc_read_class_hierarchy_descriptor (context, addr, &chd)) {
		eprintf ("Failed to parse Class Hierarchy Descriptor at 0x%08"PFMT64x"\n", addr);
		return;
	}

	if (mode == 'j') {
		rtti_msvc_print_class_hierarchy_descriptor_json (&chd);
	} else {
		rtti_msvc_print_class_hierarchy_descriptor (&chd, addr, "");
	}
}

R_API void r_anal_rtti_msvc_print_base_class_descriptor(RVTableContext *context, ut64 addr, int mode) {
	rtti_base_class_descriptor bcd;
	if (!rtti_msvc_read_base_class_descriptor (context, addr, &bcd)) {
		eprintf ("Failed to parse Base Class Descriptor at 0x%08"PFMT64x"\n", addr);
		return;
	}

	if (mode == 'j') {
		rtti_msvc_print_base_class_descriptor_json (&bcd);
	} else {
		rtti_msvc_print_base_class_descriptor (&bcd, "");
	}
}

static bool rtti_msvc_print_complete_object_locator_recurse(RVTableContext *context, ut64 atAddress, int mode, bool strict) {
	bool use_json = mode == 'j';

	ut64 colRefAddr = atAddress - context->word_size;
	ut64 colAddr;
	if (!context->read_addr (context->anal, colRefAddr, &colAddr)) {
		return false;
	}

	// complete object locator
	rtti_complete_object_locator col;
	if (!rtti_msvc_read_complete_object_locator (context, colAddr, &col)) {
		if (!strict) {
			eprintf ("Failed to parse Complete Object Locator at 0x%08"PFMT64x" (referenced from 0x%08"PFMT64x")\n", colAddr, colRefAddr);
		}
		return false;
	}

	// type descriptor
	ut64 typeDescriptorAddr = rtti_msvc_addr (context, colAddr, col.object_base, col.type_descriptor_addr);
	rtti_type_descriptor td = { 0 };
	if (!rtti_msvc_read_type_descriptor (context, typeDescriptorAddr, &td)) {
		if (!strict) {
			eprintf ("Failed to parse Type Descriptor at 0x%08"PFMT64x"\n", typeDescriptorAddr);
		}
		return false;
	}

	// class hierarchy descriptor
	ut64 classHierarchyDescriptorAddr = rtti_msvc_addr (context, colAddr, col.object_base, col.class_descriptor_addr);
	rtti_class_hierarchy_descriptor chd;
	if (!rtti_msvc_read_class_hierarchy_descriptor (context, classHierarchyDescriptorAddr, &chd)) {
		if (!strict) {
			eprintf ("Failed to parse Class Hierarchy Descriptor at 0x%08"PFMT64x"\n", classHierarchyDescriptorAddr);
		}
		rtti_type_descriptor_fini (&td);
		return false;
	}

	ut64 base = chd.base_class_array_addr;
	ut32 baseClassArrayOffset = 0;
	if (context->word_size == 8) {
		base = colAddr - col.object_base;
		baseClassArrayOffset = chd.base_class_array_addr;
	}

	RList *baseClassArray = rtti_msvc_read_base_class_array (context, chd.num_base_classes, base, baseClassArrayOffset);
	if (!baseClassArray) {
		if (!strict) {
			eprintf ("Failed to parse Base Class Array starting at 0x%08"PFMT64x"\n", base + baseClassArrayOffset);
		}
		rtti_type_descriptor_fini (&td);
		return false;
	}


	// print
	if (use_json) {
		r_cons_print ("{\"complete_object_locator\":");
		rtti_msvc_print_complete_object_locator_json (&col);
		r_cons_print (",\"type_desc\":");
		rtti_msvc_print_type_descriptor_json (&td);
		r_cons_print (",\"class_hierarchy_desc\":");
		rtti_msvc_print_class_hierarchy_descriptor_json (&chd);
		r_cons_print (",\"base_classes\":[");
	} else {
		rtti_msvc_print_complete_object_locator (&col, colAddr, "");
		rtti_msvc_print_type_descriptor (&td, typeDescriptorAddr, "\t");
		rtti_msvc_print_class_hierarchy_descriptor (&chd, classHierarchyDescriptorAddr, "\t");
	}


	// base classes
	bool json_first = true;
	RListIter *bcdIter;
	rtti_base_class_descriptor *bcd;
	r_list_foreach (baseClassArray, bcdIter, bcd) {
		if (use_json) {
			if (json_first) {
				r_cons_print ("{\"desc\":");
				json_first = false;
			} else {
				r_cons_print (",{\"desc\":");
			}
		}

		if (use_json) {
			rtti_msvc_print_base_class_descriptor_json (bcd);
		} else {
			rtti_msvc_print_base_class_descriptor (bcd, "\t\t");
		}

		ut64 baseTypeDescriptorAddr = rtti_msvc_addr (context, colAddr, col.object_base, bcd->type_descriptor_addr);
		rtti_type_descriptor btd = { 0 };
		if (rtti_msvc_read_type_descriptor (context, baseTypeDescriptorAddr, &btd)) {
			if (use_json) {
				r_cons_print (",\"type_desc\":");
				rtti_msvc_print_type_descriptor_json (&btd);
			} else {
				rtti_msvc_print_type_descriptor (&btd, baseTypeDescriptorAddr, "\t\t\t");
			}
			rtti_type_descriptor_fini (&btd);
		} else {
			if (!strict) {
				eprintf ("Failed to parse Type Descriptor at 0x%08"PFMT64x"\n", baseTypeDescriptorAddr);
			}
		}

		if(use_json) {
			r_cons_print ("}");
		}
	}
	if (use_json) {
		r_cons_print ("]");
	}

	if (use_json) {
		r_cons_print ("}");
	}

	rtti_type_descriptor_fini (&td);
	return true;
}

R_API bool r_anal_rtti_msvc_print_at_vtable(RVTableContext *context, ut64 addr, int mode, bool strict) {
	return rtti_msvc_print_complete_object_locator_recurse (context, addr, mode, strict);
}






typedef struct recovery_complete_object_locator_t {
	ut64 addr;
	bool valid;
	RVTableInfo *vtable;
	rtti_complete_object_locator col;
	struct recovery_type_descriptor_t *td;
	rtti_class_hierarchy_descriptor chd;
	RList *bcd; // <rtti_base_class_descriptor>
	RPVector base_td; // <analyzed_type_descriptor_t>
} RecoveryCompleteObjectLocator;

RecoveryCompleteObjectLocator *recovery_complete_object_locator_new() {
	RecoveryCompleteObjectLocator *col = R_NEW (RecoveryCompleteObjectLocator);
	if (!col) {
		return NULL;
	}
	col->addr = 0;
	col->valid = false;
	col->vtable = NULL;
	col->td = NULL;
	memset (&col->chd, 0, sizeof (col->chd));
	col->bcd = NULL;
	r_pvector_init (&col->base_td, NULL);
	return col;
}

void recovery_complete_object_locator_free(RecoveryCompleteObjectLocator *col) {
	if (!col) {
		return;
	}
	r_list_free (col->bcd);
	r_pvector_clear (&col->base_td);
	free (col);
}


typedef struct recovery_type_descriptor_t {
	ut64 addr;
	bool valid;
	rtti_type_descriptor td;
	RecoveryCompleteObjectLocator *col;
} RecoveryTypeDescriptor;

RecoveryTypeDescriptor *recovery_type_descriptor_new() {
	RecoveryTypeDescriptor *td = R_NEW (RecoveryTypeDescriptor);
	if (!td) {
		return NULL;
	}

	td->addr = 0;
	td->valid = false;
	memset (&td->td, 0, sizeof (td->td));
	td->col = NULL;
	//td->vtable = NULL;
	return td;
}

void recovery_type_descriptor_free(RecoveryTypeDescriptor *td) {
	if (!td) {
		return;
	}
	rtti_type_descriptor_fini (&td->td);
	free (td);
}


typedef struct rtti_msvc_anal_context_t {
	RVTableContext *vt_context;
	RPVector vtables; // <RVTableInfo>
	RPVector complete_object_locators; // <RecoveryCompleteObjectLocator> TODO: use some more efficient map
	RPVector type_descriptors; // <analyzed_typed_descriptor> TODO: use some more efficient map
} RRTTIMSVCAnalContext;


RecoveryTypeDescriptor *recovery_anal_type_descriptor(RRTTIMSVCAnalContext *context, ut64 addr, RecoveryCompleteObjectLocator *col);

RecoveryCompleteObjectLocator *recovery_anal_complete_object_locator(RRTTIMSVCAnalContext *context, ut64 addr, RVTableInfo *vtable) {
	RecoveryCompleteObjectLocator *col;
	void **it;
	r_pvector_foreach (&context->complete_object_locators, it) {
		col = *it;
		if (col->addr == addr) {
			return col;
		}
	}

	col = recovery_complete_object_locator_new ();
	if (!col) {
		return NULL;
	}
	r_pvector_push (&context->complete_object_locators, col);
	col->addr = addr;
	col->valid = rtti_msvc_read_complete_object_locator (context->vt_context, addr, &col->col);
	if (!col->valid) {
		return col;
	}
	col->vtable = vtable;


	ut64 td_addr = rtti_msvc_addr (context->vt_context, col->addr, col->col.object_base, col->col.type_descriptor_addr);
	col->td = recovery_anal_type_descriptor (context, td_addr, col);
	if (!col->td->valid) {
		col->valid = false;
		return col;
	}
	col->td->col = col;


	ut64 chd_addr = rtti_msvc_addr (context->vt_context, col->addr, col->col.object_base, col->col.class_descriptor_addr);
	col->valid &= rtti_msvc_read_class_hierarchy_descriptor (context->vt_context, chd_addr, &col->chd);
	if (!col->valid) {
		return col;
	}


	ut64 base = col->chd.base_class_array_addr;
	ut32 baseClassArrayOffset = 0;
	if (context->vt_context->word_size == 8) {
		base = col->addr - col->col.object_base;
		baseClassArrayOffset = col->chd.base_class_array_addr;
	}

	col->bcd = rtti_msvc_read_base_class_array (context->vt_context, col->chd.num_base_classes, base, baseClassArrayOffset);
	if (!col->bcd) {
		col->valid = false;
		return col;
	}


	r_pvector_reserve (&col->base_td, (size_t)col->bcd->length);
	RListIter *bcdIter;
	rtti_base_class_descriptor *bcd;
	r_list_foreach (col->bcd, bcdIter, bcd) {
		ut64 base_td_addr = rtti_msvc_addr (context->vt_context, col->addr, col->col.object_base, bcd->type_descriptor_addr);
		RecoveryTypeDescriptor *td = recovery_anal_type_descriptor (context, base_td_addr, NULL);
		if (td == col->td) {
			continue;
		}
		if (!td->valid) {
			eprintf("Warning: type descriptor of base is invalid.\n");
			continue;
		}
		r_pvector_push (&col->base_td, td);
	}

	return col;
}

RecoveryTypeDescriptor *recovery_anal_type_descriptor(RRTTIMSVCAnalContext *context, ut64 addr, RecoveryCompleteObjectLocator *col) {
	RecoveryTypeDescriptor *td;
	void **it;
	r_pvector_foreach (&context->type_descriptors, it) {
		td = *it;
		if (td->addr == addr) {
			if (col != NULL) {
				td->col = col;
			}
			return td;
		}
	}

	td = recovery_type_descriptor_new ();
	if (!td) {
		return NULL;
	}
	r_pvector_push (&context->type_descriptors, td);
	td->addr = addr;
	td->valid = rtti_msvc_read_type_descriptor (context->vt_context, addr, &td->td);
	if (!td->valid) {
		return td;
	}

	td->col = col;

	/*ut64 vtable_addr = td->td.vtable_addr; // TODO: this is probably broken for 64bit, should use rtti_msvc_addr
	ut64 colRefAddr = vtable_addr - context->vt_context->word_size;
	ut64 colAddr;
	if (!context->vt_context->read_addr (context->vt_context->anal, colRefAddr, &colAddr)) {
		eprintf ("Warning: invalidating td because of failure to get col address.\n");
		td->valid = false;
		return td;
	}
	td->col = recovery_anal_complete_object_locator (context, colAddr, NULL);
	if (!td->col->valid) {
		eprintf ("Warning: invalidating td because of invalid col.\n");
		td->valid = false;
	}

	td->vtable = vtable;
	if (!td->vtable) {
		td->vtable = r_anal_vtable_parse_at (context->vt_context, td->td.vtable_addr);
		if (td->vtable) {
			r_pvector_push (&context->vtables, td->vtable);
		}
	}*/

	return td;
}



RAnalClass *recovery_apply_type_descriptor(RRTTIMSVCAnalContext *context, RecoveryTypeDescriptor *td) {
	if (!td->valid) {
		return NULL;
	}

	RAnal *anal = context->vt_context->anal;
	void **it;
	r_pvector_foreach (&anal->classes, it) {
		RAnalClass *existing = *((RAnalClass **)it);
		if (existing->addr == td->addr) {
			return existing;
		}
	}

	char *name = r_anal_rtti_msvc_demangle_class_name (td->td.name);
	if (!name) {
		eprintf("Failed to demangle a class name: \"%s\"\n", td->td.name);
		name = strdup (td->td.name);
		if (!name) {
			return NULL;
		}
	}
	RAnalClass *cls = r_anal_class_new (name);
	free (name);
	r_anal_class_add (anal, cls);
	cls->addr = td->addr;

	if (!td->col || !td->col->valid) {
		return cls;
	}

	if (td->col->vtable) {
		cls->vtable_addr = td->col->vtable->saddr;
		RVTableMethodInfo *vmeth;
		r_vector_foreach (&td->col->vtable->methods, vmeth) {
			RAnalMethod *meth = r_anal_method_new ();
			if (!meth) {
				continue;
			}
			meth->addr = vmeth->addr;
			meth->vtable_index = (int) vmeth->vtable_offset;
			meth->name = r_str_newf ("virtual_%d", meth->vtable_index);
			r_pvector_push (&cls->methods, meth);
		}
	} else {
		cls->vtable_addr = UT64_MAX;
	}

	r_pvector_foreach (&td->col->base_td, it) {
		RecoveryTypeDescriptor *base_td = (RecoveryTypeDescriptor *)*it;
		RAnalClass *base_cls = recovery_apply_type_descriptor (context, base_td);
		if (!base_cls) {
			eprintf ("Failed to convert base td to a class\n");
			continue;
		}

		RAnalBaseClass *base_cls_info = r_vector_push (&cls->base_classes, NULL);
		base_cls_info->offset = 0; // TODO: we do have this info \o/
		base_cls_info->cls = base_cls;
	}

	return cls;
}




R_API void r_anal_rtti_msvc_recover_all(RVTableContext *vt_context, RList *vtables) {
	RRTTIMSVCAnalContext context;
	context.vt_context = vt_context;
	r_pvector_init (&context.complete_object_locators, (RPVectorFree) recovery_complete_object_locator_free);
	r_pvector_init (&context.type_descriptors, (RPVectorFree) recovery_type_descriptor_free);
	r_pvector_init (&context.vtables, (RPVectorFree)r_anal_vtable_info_free);

	RListIter *vtableIter;
	RVTableInfo *table;
	r_list_foreach (vtables, vtableIter, table) {
		/* TODO if (r_cons_is_breaked ()) {
			break;
		}*/
		ut64 colRefAddr = table->saddr - vt_context->word_size;
		ut64 colAddr;
		if (!vt_context->read_addr (vt_context->anal, colRefAddr, &colAddr)) {
			continue;
		}
		recovery_anal_complete_object_locator (&context, colAddr, table);
	}

	void **it;
	r_pvector_foreach (&context.type_descriptors, it) {
		RecoveryTypeDescriptor *td = *it;
		if (!td->valid) {
			continue;
		}
		recovery_apply_type_descriptor (&context, td);
	}

	r_pvector_clear (&context.complete_object_locators);
	r_pvector_clear (&context.type_descriptors);
	r_pvector_clear (&context.vtables);
}

