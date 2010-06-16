#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "objc/runtime.h"
#include "method_list.h"

size_t objc_alignof_type (const char *type);

// It would be so nice if this works, but in fact it returns nonsense:
//#define alignof(x) __alignof__(x)
//
#define alignof(type) __builtin_offsetof(struct { const char c; type member; }, member)

const char *objc_skip_type_qualifiers (const char *type)
{
	static const char *type_qualifiers = "rnNoORV";
	while('\0' != *type && strchr(type_qualifiers, *type))
	{
		type++;
	}
	return type;
}

static const char *sizeof_type(const char *type, size_t *size);

const char *objc_skip_typespec(const char *type)
{
	size_t ignored = 0;
	return sizeof_type(type, &ignored);
}

const char *objc_skip_argspec(const char *type)
{
	type = objc_skip_typespec(type);
	while(isdigit(*type)) { type++; }
	return type;
}

static size_t lengthOfTypeEncoding(const char *types)
{
	const char *end = objc_skip_typespec(types);
	size_t length = end - types + 1;
	return length;
}

static char *copyTypeEncoding(const char *types)
{
	size_t length = lengthOfTypeEncoding(types);
	char *copy = malloc(length + 1);
	memcpy(copy, types, length);
	copy[length] = '\0';
	return copy;
}

static const char * findParameterStart(const char *types, unsigned int index)
{
	for (unsigned int i=0 ; i<index ; i++)
	{
		types = objc_skip_argspec(types);
		if ('\0' == *types)
		{
			return NULL;
		}
	}
	return types;
}


typedef const char *(*type_parser)(const char*, void*);

static int parse_array(const char **type, type_parser callback, void *context)
{
	// skip [
	(*type)++;
	int element_count = (int)strtol(*type, (char**)type, 10);
	*type = callback(*type, context);
	// skip ]
	(*type)++;
	return element_count;
}

static void parse_struct_or_union(const char **type, type_parser callback, void *context, char endchar)
{
	// Skip the ( and structure name
	do
	{
		(*type)++;
		// Opaque type has no =definition
		if (endchar == **type) { (*type)++; return; }
	} while('=' != **type);
	// Skip =
	(*type)++;

	while (**type != endchar)
	{
		*type = callback(*type, context);
	}
	// skip }
	(*type)++;
}

static void parse_union(const char **type, type_parser callback, void *context)
{
	parse_struct_or_union(type, callback, context, ')');
}

static void parse_struct(const char **type, type_parser callback, void *context)
{
	parse_struct_or_union(type, callback, context, '}');
}

inline static void round_up(size_t *v, size_t b)
{
	if (*v % b)
	{
		*v += b - (*v % b);
	}
}
inline static size_t max(size_t v, size_t v2)
{
	return v>v2 ? v : v2;
}

static const char *sizeof_union_field(const char *type, size_t *size);

static const char *sizeof_type(const char *type, size_t *size)
{
	type = objc_skip_type_qualifiers(type);
	switch (*type)
	{
		// For all primitive types, we round up the current size to the
		// required alignment of the type, then add the size
#define APPLY_TYPE(typeName, name, capitalizedName, encodingChar) \
		case encodingChar:\
		{\
			round_up(size, (alignof(typeName) * 8));\
			*size += (sizeof(typeName) * 8);\
			return type + 1;\
		}
#define NON_INTEGER_TYPES 1
#include "type_encoding_cases.h"
		case '?':
		case 'v': return type+1;
		case 'j':
		{
			type++;
			switch (*type)
			{
#define APPLY_TYPE(typeName, name, capitalizedName, encodingChar) \
		case encodingChar:\
		{\
			round_up(size, (alignof(_Complex typeName) * 8));\
			*size += (sizeof(_Complex typeName) * 8);\
			return type + 1;\
		}
#include "type_encoding_cases.h"
			}
		}
		case '{':
		{
			const char *t = type;
			parse_struct(&t, (type_parser)sizeof_type, size);
			size_t align = objc_alignof_type(type);
			round_up(size, align * 8);
			return t;
		}
		case '[':
		{
			const char *t = type;
			int element_size = 0;
			// FIXME: aligned size
			int element_count = parse_array(&t, (type_parser)sizeof_type, &element_size);
			(*size) += element_size * element_count;
			return t;
		}
		case '(':
		{
			const char *t = type;
			size_t union_size = 0;
			parse_union(&t, (type_parser)sizeof_union_field, &union_size);
			*size += union_size;
			return t;
		}
		//FIXME: union
		case 'b':
		{
			// Consume the b
			type++;
			// Ignore the offset
			strtol(type, (char**)&type, 10);
			// Consume the element type
			type++;
			// Read the number of bits
			*size += strtol(type, (char**)&type, 10);
			return type;
		}
		case '^':
		{
			// All pointers look the same to me.
			*size += sizeof(void*) * 8;
			size_t ignored;
			// Skip the definition of the pointeee type.
			return sizeof_type(type+1, &ignored);
		}
	}
	abort();
	return NULL;
}

static const char *sizeof_union_field(const char *type, size_t *size)
{
	size_t field_size = 0;
	const char *end = sizeof_type(type, &field_size);
	*size = max(*size, field_size);
	return end;
}

static const char *alignof_type(const char *type, size_t *align)
{
	type = objc_skip_type_qualifiers(type);
	switch (*type)
	{
		// For all primitive types, we return the maximum of the new alignment
		// and the old one
#define APPLY_TYPE(typeName, name, capitalizedName, encodingChar) \
		case encodingChar:\
		{\
			*align = max((alignof(typeName) * 8), *align);\
			return type + 1;\
		}
#define NON_INTEGER_TYPES 1
#include "type_encoding_cases.h"
		case '?':
		case 'v': return type+1;
		case 'j':
		{
			type++;
			switch (*type)
			{
#define APPLY_TYPE(typeName, name, capitalizedName, encodingChar) \
		case encodingChar:\
		{\
			*align = max((alignof(_Complex typeName) * 8), *align);\
			return type + 1;\
		}
#include "type_encoding_cases.h"
			}
		}
		case '{':
		{
			const char *t = type;
			parse_struct(&t, (type_parser)alignof_type, align);
			return t;
		}
		case '(':
		{
			const char *t = type;
			parse_union(&t, (type_parser)alignof_type, align);
			return t;
		}
		case '[':
		{
			const char *t = type;
			// FIXME: aligned size
			parse_array(&t, (type_parser)alignof_type, &align);
			return t;
		}
		//FIXME: union
		case 'b':
		{
			// Consume the b
			type++;
			// Ignore the offset
			strtol(type, (char**)&type, 10);
			// Alignment of a bitfield is the alignment of the type that
			// contains it
			type = alignof_type(type, align);
			// Ignore the number of bits
			strtol(type, (char**)&type, 10);
			return type;
		}
		case '^':
		{
			*align = max((alignof(void*) * 8), *align);
			// All pointers look the same to me.
			size_t ignored;
			// Skip the definition of the pointeee type.
			return alignof_type(type+1, &ignored);
		}
	}
	abort();
	return NULL;
}

size_t objc_sizeof_type(const char *type)
{
	size_t size = 0;
	sizeof_type(type, &size);
	return size / 8;
}

size_t objc_alignof_type (const char *type)
{
	size_t align = 0;
	alignof_type(type, &align);
	return align / 8;
}

size_t objc_aligned_size(const char *type)
{
	size_t size  = objc_sizeof_type(type);
	size_t align = objc_alignof_type(type);
	return size + (size % align);
}

/*
  The size rounded up to the nearest integral of the wordsize, taken
  to be the size of a void *.
*/

size_t objc_promoted_size(const char *type)
{
	size_t size = objc_sizeof_type(type);
	return size + (size % sizeof(void*));
}

void method_getReturnType(Method method, char *dst, size_t dst_len)
{
	//TODO: Coped and pasted code.  Factor it out.
	const char *types = method->types;
	size_t length = lengthOfTypeEncoding(types);
	if (length < dst_len)
	{
		memcpy(dst, types, length);
		dst[length] = '\0';
	}
	else
	{
		memcpy(dst, types, dst_len);
	}
}

const char *method_getTypeEncoding(Method method)
{
	return method->types;
}


void method_getArgumentType(Method method,
                            unsigned int index,
                            char *dst,
                            size_t dst_len)
{
	const char *types = findParameterStart(method->types, index);
	if (NULL == types)
	{
		strncpy(dst, "", dst_len);
		return;
	}
	size_t length = lengthOfTypeEncoding(types);
	if (length < dst_len)
	{
		memcpy(dst, types, length);
		dst[length] = '\0';
	}
	else
	{
		memcpy(dst, types, dst_len);
	}
}

unsigned method_getNumberOfArguments(Method method)
{
	const char *types = method->types;
	unsigned int count = 0;
	while('\0' != *types)
	{
		types = objc_skip_argspec(types);
		count++;
	}
	return count - 1;
}

unsigned method_get_number_of_arguments(struct objc_method *method)
{
	return method_getNumberOfArguments(method);
}


char * method_copyArgumentType(Method method, unsigned int index)
{
	const char *types = findParameterStart(method->types, index);
	if (NULL == types)
	{
		return NULL;
	}
	return copyTypeEncoding(types);
}

char * method_copyReturnType(Method method)
{
	return copyTypeEncoding(method->types);
}

unsigned objc_get_type_qualifiers (const char *type)
{
	unsigned flags = 0;
#define MAP(chr, bit) case chr: flags |= (1<<bit); break;
	do
	{
		switch (*(type++))
		{
			default: return flags;
			MAP('r', 1)
			MAP('n', 2)
			MAP('N', 3)
			MAP('o', 4)
			MAP('O', 5)
			MAP('R', 6)
			MAP('V', 7)
			MAP('!', 8)
		}
	} while (1);
}

struct objc_struct_layout
{
	const char *original_type;
	const char *type;
	const char *prev_type;
	unsigned int record_size;
	unsigned int record_align;
};

// Note: The implementations of these functions is horrible.
void objc_layout_structure (const char *type,
                            struct objc_struct_layout *layout)
{
	layout->original_type = type;
	layout->type = 0;
}

static const char *layout_structure_callback(const char *type, struct objc_struct_layout *layout)
{
	size_t align = 0;
	size_t size = 0;
	const char *end = sizeof_type(type, &size);
	alignof_type(type, &align);
	//printf("Callback called with %s\n", type);
	if (layout->prev_type < type)
	{
		if (layout->record_align == 0)
		{
			layout->record_align = align;
			layout->type = type;
		}
	}
	else
	{
		round_up((size_t*)&layout->record_size, align);
		layout->record_size += size;
	}
	return end;
}

BOOL objc_layout_structure_next_member(struct objc_struct_layout *layout)
{
	const char *end = layout->type;
	layout->record_size = 0;
	layout->record_align = 0;
	layout->prev_type = layout->type;
	const char *type = layout->original_type;
	parse_struct(&type, (type_parser)layout_structure_callback, layout);
	//printf("Calculated: (%s) %s %d %d\n", layout->original_type, layout->type, layout->record_size, layout->record_align);
	//printf("old start %s, new start %s\n", end, layout->type);
	return layout->type != end;
}

void objc_layout_structure_get_info (struct objc_struct_layout *layout,
                                     unsigned int *offset,
                                     unsigned int *align,
                                     const char **type)
{
	//printf("%p\n", layout);
	*type = layout->type;
	*offset = layout->record_size / 8;
	*align= layout->record_align / 8;
	round_up((size_t*)offset, (size_t)*align);
	printf("%s %d %d\n", *type, *offset, *align);
}

#ifdef ENCODING_TESTS

#define TEST(type) do {\
	if (alignof(type) != objc_alignof_type(@encode(type)))\
		printf("Incorrect alignment for %s: %d != %d\n", @encode(type), objc_alignof_type(@encode(type)), alignof(type));\
	if (sizeof(type) != objc_sizeof_type(@encode(type)))\
		printf("Incorrect size for %s: %d != %d\n", @encode(type), objc_sizeof_type(@encode(type)), sizeof(type));\
	} while(0)

struct foo
{
	int a[2];
	int b:5;
	struct
	{
		double d;
		const char *str;
		float e;
	}c;
	long long **g;
	union { const char c; long long b; } h;
	long long f;
	_Complex int z;
	_Complex double y;
	char v;
};

typedef struct
{
	float x,y;
} Point;

typedef struct
{
	Point a, b;
} Rect;


int main(void)
{
	TEST(int);
	TEST(const char);
	TEST(unsigned long long);
	TEST(_Complex int);
	TEST(struct foo);
	struct objc_struct_layout layout;

	objc_layout_structure(@encode(Rect), &layout);
	while (objc_layout_structure_next_member (&layout))
	{
		unsigned offset;
		unsigned align;
		const char *ftype;
		struct objc_struct_layout layout2;
		objc_layout_structure_get_info (&layout, &offset, &align, &ftype);
		printf("%s: offset: %d, alignment: %d\n", ftype, offset, align);
		objc_layout_structure(ftype, &layout2);
		while (objc_layout_structure_next_member (&layout2))
		{
			objc_layout_structure_get_info (&layout2, &offset, &align, &ftype);
			printf("%s: offset: %d, alignment: %d\n", ftype, offset, align);
		}
	}
	printf("%d\n", offsetof(Rect, a.x));
	printf("%d\n", offsetof(Rect, a.y));
	printf("%d\n", offsetof(Rect, b.x));
	printf("%d\n", offsetof(Rect, b.y));

}
#endif
