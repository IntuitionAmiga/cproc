#include <stdbool.h>
#include <string.h>
#include "util.h"
#include "cc.h"

const struct target *targ;

static const struct target alltargs[] = {
	{
		.name = "x86_64-sysv",
		.typewchar = &typeint,
		.typevalist = &(struct type){
			.kind = TYPEARRAY,
			.align = 8, .size = 24,
			.base = &(struct type){
				.kind = TYPESTRUCT,
				.align = 8, .size = 24,
			},
		},
		.signedchar = 1,
	},
	{
		.name = "aarch64",
		.typevalist = &(struct type){
			.kind = TYPESTRUCT,
			.align = 8, .size = 32,
			.u.structunion.tag = "va_list",
		},
		.typewchar = &typeuint,
	},
	{
		.name = "riscv64",
		.typevalist = &(struct type){
			.kind = TYPEPOINTER, .prop = PROPSCALAR,
			.align = 8, .size = 8,
			.base = &typevoid,
		},
		.typewchar = &typeint,
	},
	{
		/* Frozen IE64 bare-metal LP64 ABI. */
		.name = "ie64",
		.typevalist = &(struct type){
			.kind = TYPEPOINTER, .prop = PROPSCALAR,
			.align = 8, .size = 8,
			.base = &typeuchar,
		},
		.typewchar = &typeuint,
		.signedchar = 1,
	},
};

void
targinit(const char *name)
{
	size_t i;
	enum typequal qual;

	if (!name) {
		/* TODO: provide a way to set this default */
		targ = &alltargs[0];
	}
	for (i = 0; i < countof(alltargs) && !targ; ++i) {
		if (strcmp(alltargs[i].name, name) == 0)
			targ = &alltargs[i];
	}
	if (!targ)
		fatal("unknown target '%s'", name);
	typechar.u.arith.issigned = targ->signedchar;
	if (strcmp(targ->name, "ie64") == 0) {
		/* The frozen IE64 ABI represents long double as IEEE binary64. */
		typeldouble.size = 8;
		typeldouble.align = 8;
	}
	qual = QUALNONE;
	typeadjvalist = typeadjust(targ->typevalist, &qual);
}
