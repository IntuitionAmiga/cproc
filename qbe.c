#define _POSIX_C_SOURCE 200809L  /* open_memstream */
#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"
#include "cc.h"

struct value {
	enum {
		VALUE_NONE,
		VALUE_GLOBAL,
		VALUE_INTCONST,
		VALUE_FLTCONST,
		VALUE_DBLCONST,
		VALUE_TEMP,
		VALUE_TYPE,
		VALUE_LABEL,

		VALUE_EXTERN = 1<<4,
		VALUE_THREAD = 1<<5,
		VALUE_QUOTE = 1<<6,
	} kind;
	unsigned id;
	/* Integer extension already present in the low word of this value. */
	unsigned wordext;
	struct type *abiresult;
	union {
		char *name;
		unsigned long long i;
		double f;
	} u;
};

/*
Output of internal-linkage functions and of anonymous or block-scope static
data is buffered and written at the end of the translation unit, once it is
known whether anything refers to it.  Every name that is written out is
recorded (see emitname) against the unit being written: a "root" unit
(everything that is always emitted) or a deferred unit.  An unreferenced
deferred unit is dropped; a deferred unit that is referenced only by other
dropped units is dropped too.
*/
struct sym {
	struct sym **refs;
	size_t nrefs, caprefs;
	unsigned long stamp;
	bool defined;   /* a unit for this symbol was written */
	bool rooted;    /* referenced from an always-emitted unit */
	bool kept;
};

struct seg {
	FILE *fp;
	char *buf;
	size_t len;
	struct sym *sym;  /* NULL for always-emitted output */
	struct seg *next;
};

static struct seg *segs, **segsend = &segs, *curseg;
static struct map symtab;
static unsigned long segserial;

/* CPROC_KEEP_INTERNAL=1 turns the dropping of unreferenced internal symbols off */
static bool
keepinternal(void)
{
	static int keep = -1;
	const char *env;

	if (keep < 0) {
		env = getenv("CPROC_KEEP_INTERNAL");
		keep = env && *env && strcmp(env, "0") != 0;
	}
	return keep;
}

/* the real standard output, which the macros below hide */
static FILE *
realout(void)
{
	return stdout;
}

#undef stdout
#undef printf
#undef putchar
#undef puts
#define stdout (curout())
#define printf(...) fprintf(curout(), __VA_ARGS__)
#define putchar(c) putc((c), curout())
#define puts(s) (fputs((s), curout()), putc('\n', curout()))

static void
openseg(struct sym *sym)
{
	struct seg *s;

	s = xmalloc(sizeof(*s));
	s->sym = sym;
	s->buf = NULL;
	s->len = 0;
	s->next = NULL;
	s->fp = open_memstream(&s->buf, &s->len);
	if (!s->fp)
		fatal("open_memstream:");
	*segsend = s;
	segsend = &s->next;
	curseg = s;
	++segserial;
}

static void
closeseg(void)
{
	if (!curseg)
		return;
	/* glibc can lose memory stream data without reporting it on every write */
	if (ferror(curseg->fp) || fflush(curseg->fp) != 0 || fclose(curseg->fp) != 0 || !curseg->buf)
		fatal("write failed");
	curseg->fp = NULL;
	curseg = NULL;
}

static FILE *
curout(void)
{
	if (!curseg)
		openseg(NULL);
	return curseg->fp;
}

/* look up the record of a name that has no external linkage */
static struct sym *
symof(struct value *v)
{
	struct mapkey k;
	struct sym *sym;
	size_t i;
	char *name;
	int n;

	if ((v->kind & 0xf) != VALUE_GLOBAL || v->kind & VALUE_THREAD)
		return NULL;
	/*
	A quoted name is the assembler symbol itself, so it can name a
	static definition through an asm label on an extern declaration.
	*/
	if (v->kind & VALUE_EXTERN && !(v->kind & VALUE_QUOTE))
		return NULL;
	if (v->id) {
		name = xmalloc(24);
		n = snprintf(name, 24, "%u", v->id);
	} else if (v->u.name) {
		name = xmalloc(strlen(v->u.name) + 2);
		n = sprintf(name, "$%s", v->u.name);
	} else {
		return NULL;
	}
	if (!symtab.cap)
		mapinit(&symtab, 256);
	mapkey(&k, name, n);
	if (mapput(&symtab, &k, &i)) {
		sym = xmalloc(sizeof(*sym));
		memset(sym, 0, sizeof(*sym));
		symtab.vals[i].p = sym;
	} else {
		free(name);
		sym = symtab.vals[i].p;
	}
	return sym;
}

static void
noteref(struct value *v)
{
	struct sym *sym, *from;

	sym = symof(v);
	if (!sym)
		return;
	if (!curseg)
		openseg(NULL);
	from = curseg->sym;
	if (!from) {
		sym->rooted = true;
	} else if (sym != from && sym->stamp != segserial) {
		sym->stamp = segserial;
		if (from->nrefs == from->caprefs) {
			from->caprefs = from->caprefs ? from->caprefs * 2 : 8;
			from->refs = xreallocarray(from->refs, from->caprefs, sizeof(from->refs[0]));
		}
		from->refs[from->nrefs++] = sym;
	}
}

static bool
deferrablefunc(struct decl *d)
{
	return !keepinternal() && d->linkage == LINKINTERN && !d->weak && !d->asmname && !d->section;
}

static bool
deferrabledata(struct decl *d)
{
	return !keepinternal() && d->kind == DECLOBJECT && d->linkage == LINKNONE
		&& d->u.obj.storage == SDSTATIC && !d->weak && !d->asmname && !d->section;
}

/* start output that may be dropped, if nothing refers to symbol v */
static void
beginunit(struct value *v, bool deferrable)
{
	struct sym *sym;

	if (!deferrable || !(sym = symof(v)))
		return;
	closeseg();
	openseg(sym);
	sym->defined = true;
}

static void
endunit(void)
{
	if (curseg && curseg->sym)
		closeseg();
}

enum memtype { MEM_UNKNOWN, MEM_SHORT, MEM_INT };

struct lvalue {
	struct value *addr;
	struct bitfield bits;
	enum memtype memtype;
};

enum instkind {
	INONE,

#define OP(op, name) op,
#include "ops.h"
#undef OP

	IARG,
	IVARARG,
};

struct qbetype {
	char base, data;
	enum instkind load, store;
};

struct inst {
	enum instkind kind;
	enum memtype memtype;
	int class;
	struct value res, *arg[2];
};

struct jump {
	enum {
		JUMP_NONE,
		JUMP_JMP,
		JUMP_JNZ,
		JUMP_RET,
		JUMP_HLT,
	} kind;
	struct value *arg;
	struct block *blk[2];
};

struct block {
	struct value label;
	struct array insts;
	struct {
		int class;
		struct block *blk[2];
		struct value *val[2];
		struct value res;
	} phi;
	struct jump jump;

	struct block *next;
};

struct switchcase {
	struct treenode node;
	struct block *body;
};

struct func {
	struct decl *decl, *namedecl;
	char *name;
	struct value *paramtemps;
	struct type *type;
	struct block *start, *end;
	struct map gotos;
	unsigned lastid;
};

static const int ptrclass = 'l';

void
switchcase(struct switchcases *cases, unsigned long long i, struct block *b)
{
	struct switchcase *c;

	c = treeinsert(&cases->root, i, sizeof(*c));
	if (!c->node.new)
		error(&tok.loc, "multiple 'case' labels with same value");
	c->body = b;
}

/* values */

struct block *
mkblock(char *name)
{
	static unsigned id;
	struct block *b;

	b = xmalloc(sizeof(*b));
	b->label.kind = VALUE_LABEL;
	b->label.u.name = name;
	b->label.id = ++id;
	b->insts = (struct array){0};
	b->jump.kind = JUMP_NONE;
	b->phi.res.kind = VALUE_NONE;
	b->next = NULL;

	return b;
}

struct value *
mkglobal(struct decl *d)
{
	static unsigned id;
	struct value *v;

	v = xmalloc(sizeof(*v));
	v->kind = VALUE_GLOBAL;
	if (d->linkage == LINKEXTERN)
		v->kind |= VALUE_EXTERN;
	if (d->kind == DECLOBJECT && d->u.obj.storage == SDTHREAD)
		v->kind |= VALUE_THREAD;
	if (d->asmname) {
		v->kind |= VALUE_QUOTE;
		v->u.name = d->asmname;
		v->id = 0;
	} else {
		v->u.name = d->name;
		v->id = d->linkage == LINKNONE ? ++id : 0;
	}

	return v;
}

struct value *
mkintconst(unsigned long long n)
{
	struct value *v;

	v = xmalloc(sizeof(*v));
	v->kind = VALUE_INTCONST;
	v->u.i = n;

	return v;
}

static struct value *
mkfltconst(int kind, double n)
{
	struct value *v;

	v = xmalloc(sizeof(*v));
	v->kind = kind;
	v->u.f = n;

	return v;
}

static struct qbetype
qbetype(struct type *t)
{
	static const struct qbetype
		ub = {'w', 'b', ILOADUB, ISTOREB},
		sb = {'w', 'b', ILOADSB, ISTOREB},
		uh = {'w', 'h', ILOADUH, ISTOREH},
		sh = {'w', 'h', ILOADSH, ISTOREH},
		w = {'w', 'w', ILOADW, ISTOREW},
		l = {'l', 'l', ILOADL, ISTOREL},
		s = {'s', 's', ILOADS, ISTORES},
		d = {'d', 'd', ILOADD, ISTORED},
		v = {0};

	if (t == &typevoid)
		return v;
	if (!(t->prop & PROPSCALAR))
		return l;
	switch (t->size) {
	case 1: return t->u.arith.issigned ? sb : ub;
	case 2: return t->u.arith.issigned ? sh : uh;
	case 4: return t->prop & PROPFLOAT ? s : w;
	case 8: return t->prop & PROPFLOAT ? d : l;
	case 16: fatal("long double is not yet supported");
	}
	assert(0);
}

static bool
isie64(void)
{
	return strcmp(targ->name, "ie64") == 0;
}

static bool
ie64wideint(struct type *t)
{
	return isie64()
		&& t->prop & PROPINT && t->size < 8;
}

static int
abiclass(struct type *t)
{
	return ie64wideint(t) ? 'l' : qbetype(t).base;
}

static struct value *convert(struct func *, struct type *, struct type *,
	struct value *);

static struct value *
ie64abiextend(struct func *f, struct type *t, struct value *v)
{
	if (!ie64wideint(t))
		return v;
	return convert(f, t->u.arith.issigned ? &typelong : &typeulong, t, v);
}

static enum instkind
volatileop(enum instkind op)
{
	if (strcmp(targ->name, "ie64") != 0)
		error(&tok.loc, "volatile access is not yet supported on this target");
	switch (op) {
	case ISTOREB: return ISTOREVB;
	case ISTOREH: return ISTOREVH;
	case ISTOREW: return ISTOREVW;
	case ISTOREL: return ISTOREVL;
	case ISTORES: return ISTOREVS;
	case ISTORED: return ISTOREVD;
	case ILOADSB: return ILOADVSB;
	case ILOADUB: return ILOADVUB;
	case ILOADSH: return ILOADVSH;
	case ILOADUH: return ILOADVUH;
	case ILOADW: return ILOADVW;
	case ILOADL: return ILOADVL;
	case ILOADS: return ILOADVS;
	case ILOADD: return ILOADVD;
	default: fatal("invalid volatile memory operation");
	}
}

/* functions */

static void emittype(struct type *);
static void emitname(struct value *);
static void emitvalue(struct value *);

static void
functemp(struct func *f, struct value *v)
{
	v->kind = VALUE_TEMP;
	v->wordext = 0;
	v->u.name = NULL;
	v->id = ++f->lastid;
	v->abiresult = NULL;
}

static const char *const instname[] = {
#define OP(op, name) [op] = name,
#include "ops.h"
#undef OP
};

static const enum instkind ie64inst[IE64B_COUNT] = {
	[IE64B_NOP] = IIE64NOP, [IE64B_HALT] = IIE64HALT,
	[IE64B_SEI] = IIE64SEI, [IE64B_CLI] = IIE64CLI,
	[IE64B_TLBFLUSH] = IIE64TLBFLUSH, [IE64B_SUAEN] = IIE64SUAEN,
	[IE64B_SUADIS] = IIE64SUADIS, [IE64B_ERET] = IIE64ERET,
	[IE64B_RTI] = IIE64RTI, [IE64B_SMODE] = IIE64SMODE,
	[IE64B_FMOVSR] = IIE64FMOVSR, [IE64B_FMOVCR] = IIE64FMOVCR,
	[IE64B_TLBINVAL] = IIE64TLBINVAL, [IE64B_WAIT] = IIE64WAIT,
	[IE64B_SYSCALL] = IIE64SYSCALL, [IE64B_MFCR] = IIE64MFCR,
	[IE64B_FMOVECR] = IIE64FMOVECR, [IE64B_DMOVECR] = IIE64DMOVECR,
	[IE64B_FMOVSC] = IIE64FMOVSC, [IE64B_FMOVCC] = IIE64FMOVCC,
	[IE64B_MTCR] = IIE64MTCR, [IE64B_FMOD] = IIE64FMOD,
	[IE64B_DMOD] = IIE64DMOD, [IE64B_FABS] = IIE64FABS,
	[IE64B_DABS] = IIE64DABS, [IE64B_FINT] = IIE64FINT,
	[IE64B_DINT] = IIE64DINT, [IE64B_FCVTSD] = IIE64FCVTSD,
	[IE64B_FCVTDS] = IIE64FCVTDS, [IE64B_FSIN] = IIE64FSIN,
	[IE64B_FCOS] = IIE64FCOS, [IE64B_FTAN] = IIE64FTAN,
	[IE64B_FATAN] = IIE64FATAN, [IE64B_FLOG] = IIE64FLOG,
	[IE64B_FEXP] = IIE64FEXP, [IE64B_FPOW] = IIE64FPOW,
	[IE64B_FSQRT] = IIE64FSQRT,
	[IE64B_DSIN] = IIE64DSIN, [IE64B_DCOS] = IIE64DCOS,
	[IE64B_DTAN] = IIE64DTAN, [IE64B_DATAN] = IIE64DATAN,
	[IE64B_DLOG] = IIE64DLOG, [IE64B_DEXP] = IIE64DEXP,
	[IE64B_DPOW] = IIE64DPOW, [IE64B_DSQRT] = IIE64DSQRT,
	[IE64B_XCHG] = IIE64XCHG,
	[IE64B_FAA] = IIE64FAA, [IE64B_FAND] = IIE64FAND,
	[IE64B_FOR] = IIE64FOR, [IE64B_FXOR] = IIE64FXOR,
	[IE64B_CAS] = IIE64CAS,
	[IE64B_FIPR] = IIE64FIPR, [IE64B_FTRV] = IIE64FTRV,
};

static struct inst *
mkinst(struct func *f, int op, int class, struct value *arg0, struct value *arg1)
{
	struct inst *inst;

	inst = xmalloc(sizeof(*inst));
	inst->kind = op;
	inst->class = class;
	inst->memtype = MEM_UNKNOWN;
	inst->arg[0] = arg0;
	inst->arg[1] = arg1;
	inst->res.wordext = 0;
	if (class && op != IARG)
		functemp(f, &inst->res);
	else
		inst->res.kind = VALUE_NONE;
	return inst;
}

static struct value *
funcinstmem(struct func *f, int op, int class, struct value *arg0,
    struct value *arg1, enum memtype mt)
{
	struct inst *inst;
	struct block *b;

	if (f->end->jump.kind) {
		b = mkblock("dead");
		funclabel(f, b);
	}
	inst = mkinst(f, op, class, arg0, arg1);
	inst->memtype = mt;
	if (class == 'w') {
		switch (op) {
		case ILOADSB: case ILOADVSB: inst->res.wordext = IEXTSB; break;
		case ILOADUB: case ILOADVUB: inst->res.wordext = IEXTUB; break;
		case ILOADSH: case ILOADVSH: inst->res.wordext = IEXTSH; break;
		case ILOADUH: case ILOADVUH: inst->res.wordext = IEXTUH; break;
		}
	}
	arrayaddptr(&f->end->insts, inst);
	return &inst->res;
}

static struct value *
funcinst(struct func *f, int op, int class, struct value *arg0, struct value *arg1)
{
	return funcinstmem(f, op, class, arg0, arg1, MEM_UNKNOWN);
}

static struct value *
funcbits(struct func *f, struct type *t, struct value *v, struct bitfield b)
{
	int class, bits;

	class = t->size <= 4 ? 'w' : 'l';
	bits = b.after;
	if (!bits && t->kind == TYPEBITINT)
		bits = t->size * 8 - t->u.arith.width;
	if (bits) {
		bits += (t->size + 3 & -4) - t->size << 3;
		v = funcinst(f, ISHL, class, v, mkintconst(bits));
	}
	bits += b.before;
	if (bits)
		v = funcinst(f, t->u.arith.issigned ? ISAR : ISHR, class, v, mkintconst(bits));
	return v;
}

static struct value *
convert(struct func *f, struct type *dst, struct type *src, struct value *l)
{
	enum instkind op;
	struct value *r = NULL;
	int class;

	if (src == dst)
		return l;
	if (src->kind == TYPEPOINTER)
		src = &typeulong;
	if (dst->kind == TYPEPOINTER)
		dst = &typeulong;
	if (dst->kind == TYPEVOID)
		return NULL;
	if (!(src->prop & PROPREAL) || !(dst->prop & PROPREAL))
		fatal("internal error; unsupported conversion");
	if (dst->kind == TYPEBOOL) {
		class = 'w';
		if (src->prop & PROPINT) {
			r = mkintconst(0);
			switch (src->size) {
			case 1: op = ICNEW; if (src->kind != TYPEBITINT) l = funcinst(f, IEXTUB, 'w', l, NULL); break;
			case 2: op = ICNEW; if (src->kind != TYPEBITINT) l = funcinst(f, IEXTUH, 'w', l, NULL); break;
			case 4: op = ICNEW; break;
			case 8: op = ICNEL; break;
			default: fatal("internal error; unknown integer conversion");
			}
		} else {
			assert(src->prop & PROPFLOAT);
			switch (src->size) {
			case 4: op = ICNES, r = mkfltconst(VALUE_FLTCONST, 0); break;
			case 8: op = ICNED, r = mkfltconst(VALUE_DBLCONST, 0); break;
			default: fatal("internal error; unknown floating point conversion");
			}
		}
	} else if (dst->prop & PROPINT) {
		class = dst->size == 8 ? 'l' : 'w';
		if (src->prop & PROPINT) {
			if (dst->u.arith.width <= src->u.arith.width) {
				if (dst->kind == TYPEBITINT)
					return funcbits(f, dst, l, (struct bitfield){0});
				return l;
			}
			/* IE64 calls return narrow integers already extended to 64 bits. */
			if (l->kind == VALUE_TEMP && l->abiresult == src)
				return l;
			if (src->kind == TYPEBITINT && (src->size == 8 || dst->size == 4))
				return l;
			switch (src->size) {
			case 4: op = src->u.arith.issigned ? IEXTSW : IEXTUW; break;
			case 2: op = src->u.arith.issigned ? IEXTSH : IEXTUH; break;
			case 1: op = src->u.arith.issigned ? IEXTSB : IEXTUB; break;
			default: fatal("internal error; unknown integer conversion");
			}
		} else {
			if (dst->u.arith.issigned)
				op = src->size == 8 ? IDTOSI : ISTOSI;
			else
				op = src->size == 8 ? IDTOUI : ISTOUI;
		}
	} else {
		class = dst->size == 8 ? 'd' : 's';
		if (src->prop & PROPINT) {
			if (src->size < 4)
				l = convert(f, src->u.arith.issigned ? &typeint : &typeuint, src, l);
			if (src->u.arith.issigned)
				op = src->size == 8 ? ISLTOF : ISWTOF;
			else
				op = src->size == 8 ? IULTOF : IUWTOF;
		} else {
			assert(src->prop & PROPFLOAT);
			if (src->size == dst->size)
				return l;
			op = src->size < dst->size ? IEXTS : ITRUNCD;
		}
	}

	/* Widening within the word class can reuse an already extending load.
	 * A cast to a narrower type keeps the same value but does not create
	 * an extension fact for that narrower width. */
	if (isie64() && class == 'w' && l->kind == VALUE_TEMP
	&& (op == IEXTSB || op == IEXTUB || op == IEXTSH || op == IEXTUH)
	&& l->wordext == op)
		return l;
	return funcinst(f, op, class, l, r);
}

static void
calcvla(struct func *f, struct type *t)
{
	struct value *length, *basesize;

	if (!(t->prop & PROPVM))
		return;
	if (t->base)
		calcvla(f, t->base);
	if (t->kind == TYPEFUNC || t->size)
		return;
	assert(t->kind == TYPEARRAY);
	if (!t->u.array.size) {
		assert(t->base->size || t->base->kind == TYPEARRAY);
		assert(t->u.array.length);
		length = convert(f, &typeulong, t->u.array.length->type, funcexpr(f, t->u.array.length));
		basesize = t->base->size ? mkintconst(t->base->size) : t->base->u.array.size;
		t->u.array.size = funcinst(f, IMUL, 'l', length, basesize);
	}
}

static void
funcalloc(struct func *f, struct decl *d)
{
	enum instkind op;
	struct block *end;
	struct value *v;
	int align;

	assert(!d->type->incomplete);
	calcvla(f, d->type);
	end = f->end;
	if (d->type->size) {
		f->end = f->start;
		v = mkintconst(d->type->size);
	} else {
		assert(d->type->kind == TYPEARRAY);
		assert(d->type->u.array.size);
		v = d->type->u.array.size;
	}
	align = d->u.obj.align;
	switch (align) {
	case 1:
	case 2:
	case 4:  op = IALLOC4; break;
	case 8:  op = IALLOC8; break;
	default: v = funcinst(f, IADD, ptrclass, v, mkintconst(align - 16));  /* fallthrough */
	case 16: op = IALLOC16; break;
	}
	v = funcinst(f, op, ptrclass, v, NULL);
	if (align > 16) {
		/* TODO: implement alloc32 in QBE and use that instead */
		v = funcinst(f, IADD, ptrclass, v, mkintconst(align - 16));
		v = funcinst(f, IAND, ptrclass, v, mkintconst(-align));
	}
	d->value = v;
	f->end = end;
}

static void
funccopy(struct func *f, struct value *dst, struct value *src, unsigned long long size, int align)
{
	enum instkind load, store;
	int class;
	struct value *tmp, *inc;
	unsigned long long off;

	assert((align & align - 1) == 0);
	class = 'w';
	switch (align) {
	case 1: load = ILOADUB, store = ISTOREB; break;
	case 2: load = ILOADUH, store = ISTOREH; break;
	case 4: load = ILOADW, store = ISTOREW; break;
	default: load = ILOADL, store = ISTOREL, align = 8, class = 'l'; break;
	}
	inc = mkintconst(align);
	off = 0;
	for (;;) {
		tmp = funcinst(f, load, class, src, NULL);
		funcinst(f, store, 0, tmp, dst);
		off += align;
		if (off >= size)
			break;
		src = funcinst(f, IADD, ptrclass, src, inc);
		dst = funcinst(f, IADD, ptrclass, dst, inc);
	}
}

static struct value *
atomictemp(struct func *f, struct type *t)
{
	struct decl *d;

	d = mkdecl(NULL, DECLOBJECT, t, QUALNONE, LINKNONE);
	d->u.obj.storage = SDAUTO;
	funcalloc(f, d);
	return d->value;
}

static void
atomiccall(struct func *f, const char *name, struct value *object,
	struct value *buffer, struct type *t, enum typequal tq)
{
	struct decl *d;
	struct value *callee;

	d = mkdecl((char *)name, DECLFUNC, NULL, QUALNONE, LINKEXTERN);
	callee = mkglobal(d);
	funcinst(f, ICALL, 0, callee, NULL);
	funcinst(f, IARG, ptrclass, object, NULL);
	funcinst(f, IARG, ptrclass, buffer, NULL);
	funcinst(f, IARG, 'l', mkintconst(t->size), NULL);
	funcinst(f, IARG, 'l', mkintconst(!!(tq & QUALVOLATILE)), NULL);
}

static struct value *
atomicruntimecall(struct func *f, const char *name, int resultclass,
	struct value **args, int *classes, size_t nargs)
{
	struct decl *d;
	struct value *callee, *result;
	size_t i;

	d = mkdecl((char *)name, DECLFUNC, NULL, QUALNONE, LINKEXTERN);
	callee = mkglobal(d);
	result = funcinst(f, ICALL, resultclass, callee, NULL);
	for (i = 0; i < nargs; ++i)
		funcinst(f, IARG, classes[i], args[i], NULL);
	return result;
}

static void
atomicvalidate(struct func *f, struct value *success, struct value *failure)
{
	struct value *args[] = {success, failure};
	int classes[] = {'l', 'l'};
	atomicruntimecall(f, "__ie64_atomic_validate_orders", 0, args, classes, 2);
}

static struct value *
funcstore(struct func *f, struct type *t, enum typequal tq, struct lvalue lval, struct value *v)
{
	struct value *r;
	enum typeprop tp;
	unsigned long long mask;
	struct qbetype qt;
	int bits;

	if (tq & QUALCONST)
		error(&tok.loc, "cannot store to 'const' object");
	if (tq & QUALATOMIC) {
		struct value *tmp;
		if (!isie64())
			error(&tok.loc, "atomic access is not yet supported on this target");
		if (lval.bits.before || lval.bits.after)
			error(&tok.loc, "atomic bit-fields are not supported");
		tmp = atomictemp(f, t);
		funcstore(f, t, QUALNONE, (struct lvalue){tmp}, v);
		atomiccall(f, "__ie64_atomic_store", lval.addr, tmp, t, tq);
		return v;
	}
	tp = t->prop;
	assert(!lval.bits.before && !lval.bits.after || tp & PROPINT);
	r = v;
	switch (t->kind) {
	case TYPESTRUCT:
	case TYPEUNION:
	case TYPEARRAY:
		if (tq & QUALVOLATILE)
			error(&tok.loc, "volatile aggregate store is not yet supported");
		funccopy(f, lval.addr, v, t->size, t->align);
		break;
	case TYPEPOINTER:
		t = &typeulong;
		/* fallthrough */
	default:
		assert(tp & PROPSCALAR);
		qt = qbetype(t);
		bits = lval.bits.before + lval.bits.after;
		if (bits) {
			mask = 0xffffffffffffffffu >> 64 - t->size * 8 + bits << lval.bits.before;
			v = funcinst(f, ISHL, qt.base, v, mkintconst(lval.bits.before));
			r = funcbits(f, t, v, lval.bits);
			v = funcinst(f, IAND, qt.base, v, mkintconst(mask));
			v = funcinst(f, IOR, qt.base, v,
				funcinst(f, IAND, qt.base,
					funcinst(f, tq & QUALVOLATILE ? volatileop(qt.load) : qt.load,
						qt.base, lval.addr, NULL),
					mkintconst(~mask)
				)
			);
		}
		funcinstmem(f, tq & QUALVOLATILE ? volatileop(qt.store) : qt.store,
			0, v, lval.addr, tq & QUALVOLATILE ? MEM_UNKNOWN : lval.memtype);
		break;
	}
	return r;
}

static struct value *
funcload(struct func *f, struct type *t, enum typequal tq, struct lvalue lval)
{
	struct value *v;
	struct qbetype qt;

	if (tq & QUALATOMIC) {
		struct value *tmp;
		if (!isie64())
			error(&tok.loc, "atomic access is not yet supported on this target");
		if (lval.bits.before || lval.bits.after)
			error(&tok.loc, "atomic bit-fields are not supported");
		tmp = atomictemp(f, t);
		atomiccall(f, "__ie64_atomic_load", lval.addr, tmp, t, tq);
		return funcload(f, t, QUALNONE, (struct lvalue){tmp});
	}

	switch (t->kind) {
	case TYPESTRUCT:
	case TYPEUNION:
	case TYPEARRAY:
		return lval.addr;
	}
	qt = qbetype(t);
	v = funcinstmem(f, tq & QUALVOLATILE ? volatileop(qt.load) : qt.load,
		qt.base, lval.addr, NULL, tq & QUALVOLATILE ? MEM_UNKNOWN : lval.memtype);
	return funcbits(f, t, v, lval.bits);
}

struct func *
mkfunc(struct decl *decl, char *name, struct type *t, struct scope *s)
{
	struct func *f;
	struct decl *d;
	struct value *v;

	f = xmalloc(sizeof(*f));
	f->decl = decl;
	f->name = name;
	f->type = t;
	f->start = f->end = mkblock("start");
	f->lastid = 0;
	mapinit(&f->gotos, 8);
	emittype(t->base);

	/* allocate space for parameters */
	f->paramtemps = xreallocarray(NULL, t->u.func.nparam, sizeof *f->paramtemps);
	for (d = t->u.func.params, v = f->paramtemps; d; d = d->next, ++v) {
		emittype(d->type);
		functemp(f, v);
		if(!d->name)
			continue;
		if (d->type->value) {
			d->value = v;
		} else {
			funcalloc(f, d);
			funcstore(f, d->type, QUALNONE, (struct lvalue){d->value}, v);
		}
	}

	t = mkarraytype(&typechar, QUALCONST, strlen(name) + 1);
	d = mkdecl("__func__", DECLOBJECT, t, QUALNONE, LINKNONE);
	d->u.obj.storage = SDSTATIC;
	d->value = mkglobal(d);
	scopeputdecl(s, d);
	f->namedecl = d;

	funclabel(f, mkblock("body"));

	return f;
}

void
delfunc(struct func *f)
{
	struct block *b;
	struct inst **inst;

	while (b = f->start) {
		f->start = b->next;
		arrayforeach (&b->insts, inst)
			free(*inst);
		free(b->insts.val);
		free(b);
	}
	mapfree(&f->gotos, free);
	free(f);
}

struct type *
functype(struct func *f)
{
	return f->type;
}

void
funclabel(struct func *f, struct block *b)
{
	f->end->next = b;
	f->end = b;
}

void
funcjmp(struct func *f, struct block *l)
{
	struct block *b = f->end;

	if (!b->jump.kind) {
		b->jump.kind = JUMP_JMP;
		b->jump.blk[0] = l;
	}
}

void
funcjnz(struct func *f, struct value *v, struct type *t, struct block *l1, struct block *l2)
{
	struct block *b = f->end;

	if (b->jump.kind)
		return;
	if (t) {
		assert(t->prop & PROPSCALAR);
		/*
		Ideally we would just do this conversion unconditionally,
		but QBE is not currently able to optimize the conversion
		away for int.
		*/
		if (t->prop & PROPINT && t->size < 4)
			v = convert(f, &typeint, t, v);
		else if (t->prop & PROPFLOAT || t->size > 4)
			v = convert(f, &typebool, t, v);
	}
	b->jump.kind = JUMP_JNZ;
	b->jump.arg = v;
	b->jump.blk[0] = l1;
	b->jump.blk[1] = l2;
}

void
funcret(struct func *f, struct value *v)
{
	struct block *b = f->end;

	if (!b->jump.kind) {
		if (v)
			v = ie64abiextend(f, f->type->base, v);
		b->jump.kind = JUMP_RET;
		b->jump.arg = v;
	}
}

void
funchlt(struct func *f)
{
	struct block *b = f->end;

	if (!b->jump.kind)
		b->jump.kind = JUMP_HLT;
}

struct gotolabel *
funcgoto(struct func *f, char *name)
{
	struct gotolabel *g;
	struct mapkey key;
	size_t idx;

	mapkey(&key, name, strlen(name));
	if (mapput(&f->gotos, &key, &idx)) {
		g = xmalloc(sizeof(*g));
		g->label = mkblock(name);
		f->gotos.vals[idx].p = g;
	}

	return f->gotos.vals[idx].p;
}

/* Only scalar accesses through a typed pointer parameter qualify. A member
 * selection, cast, or array decay on the address path breaks this form.
 * In particular, an explicit union access must keep its full alias set. */
static enum memtype
parammemtype(struct func *f, struct expr *e)
{
	struct decl *p;
	struct expr *addr;

	if (!isie64() || e->kind != EXPRUNARY || e->op != TMUL
	|| (e->type->kind != TYPESHORT && e->type->kind != TYPEINT))
		return MEM_UNKNOWN;
	if (e->base->kind == EXPRTEMP && e->base->u.temp.origin)
		return parammemtype(f, e->base->u.temp.origin);
	for (addr=e->base; addr->kind == EXPRBINARY
	&& (addr->op == TADD || addr->op == TSUB); addr=addr->u.binary.l)
		if (addr->u.binary.l->type->kind != TYPEPOINTER)
			return MEM_UNKNOWN;
	if (addr->kind != EXPRIDENT || addr->type->kind != TYPEPOINTER
	|| addr->type->base->kind != e->type->kind)
		return MEM_UNKNOWN;
	for (p=f->type->u.func.params; p; p=p->next)
		if (p == addr->u.ident.decl)
			return e->type->kind == TYPESHORT ? MEM_SHORT : MEM_INT;
	return MEM_UNKNOWN;
}

static struct lvalue
funclval(struct func *f, struct expr *e)
{
	struct lvalue lval = {0};
	struct decl *d;

	if (e->kind == EXPRBITFIELD) {
		lval.bits = e->u.bitfield.bits;
		e = e->base;
	}
	switch (e->kind) {
	case EXPRIDENT:
		d = e->u.ident.decl;
		if (d->kind != DECLOBJECT && d->kind != DECLFUNC)
			error(&tok.loc, "identifier '%s' is not an object or function", d->name);
		if (d == f->namedecl) {
			beginunit(d->value, deferrabledata(d));
			fputs("data ", stdout);
			emitname(d->value);
			printf(" = { b \"%s\", b 0 }\n", f->name);
			endunit();
			f->namedecl = NULL;
		}
		lval.addr = d->value;
		break;
	case EXPRSTRING:
		d = stringdecl(e);
		lval.addr = d->value;
		break;
	case EXPRCOMPOUND:
		if (e->toeval)
			funcexpr(f, e->toeval);
		d = e->u.compound.decl;
		if (d->u.obj.storage == SDAUTO)
			funcinit(f, d, e->u.compound.init, true);
		lval.addr = d->value;
		break;
	case EXPRUNARY:
		if (e->op != TMUL)
			error(&tok.loc, "expression is not an object");
		lval.addr = funcexpr(f, e->base);
		if (!lval.bits.before && !lval.bits.after)
			lval.memtype = parammemtype(f, e);
		break;
	default:
		if (e->type->kind != TYPESTRUCT && e->type->kind != TYPEUNION)
			error(&tok.loc, "expression is not an object");
		lval.addr = funcexpr(f, e);
	}
	return lval;
}

/* returns the expression value in the true branch */
struct value *
funcbranch(struct func *f, struct expr *e, struct block *bt, struct block *bf)
{
	static struct value one = {.kind = VALUE_INTCONST, .u.i = 1};
	struct expr *l, *r;
	struct value *v;
	struct block *b;

	/* Maybe we we could do something for EXPRCOND as well. */
	switch (e->kind) {
	case EXPRBINARY:
		l = e->u.binary.l;
		r = e->u.binary.r;
		switch (e->op) {
		case TEQL:
		case TNEQ:
			r = eval(r);
			if (r->kind == EXPRCONST && r->type->prop & PROPINT && r->u.constant.u == 0) {
				if (e->op == TEQL)
					b = bt, bt = bf, bf = b;
				funcbranch(f, l, bt, bf);
				return &one;
			}
			break;
		case TLOR:
		case TLAND:
			if (e->op == TLOR) {
				b = mkblock("logic_or");
				funcbranch(f, l, bt, b);
			} else {
				b = mkblock("logic_and");
				funcbranch(f, l, b, bf);
			}
			funclabel(f, b);
			funcbranch(f, r, bt, bf);
			return &one;
		}
		break;
	case EXPRCOMMA:
		for (e = e->base; e->next; e = e->next)
			funcexpr(f, e);
		return funcbranch(f, e, bt, bf);
	}
	v = funcexpr(f, e);
	funcjnz(f, v, e->type, bt, bf);
	return v;
}

struct value *
funcexpr(struct func *f, struct expr *e)
{
	enum instkind op = INONE;
	struct decl *d;
	struct value *l, *r, *v, **argvals;
	struct lvalue lval;
	struct expr *arg;
	struct block *b[3];
	struct type *t, *functype;
	size_t i;

	calcvla(f, e->type);
	switch (e->kind) {
	case EXPRIDENT:
		d = e->u.ident.decl;
		switch (d->kind) {
		case DECLOBJECT: return funcload(f, e->type, e->qual, (struct lvalue){d->value});
		case DECLCONST:  return d->value;
		default:
			fatal("unimplemented declaration kind %d", d->kind);
		}
		break;
	case EXPRCONST:
		t = e->type;
		if (t->prop & PROPINT || t->kind == TYPEPOINTER)
			return mkintconst(e->u.constant.u);
		assert(t->prop & PROPFLOAT);
		return mkfltconst(t->size == 4 ? VALUE_FLTCONST : VALUE_DBLCONST, e->u.constant.f);
	case EXPRBITFIELD:
	case EXPRCOMPOUND:
		lval = funclval(f, e);
		return funcload(f, e->type, e->qual, lval);
	case EXPRINCDEC:
		lval = funclval(f, e->base);
		t = e->type;
		if (t->kind == TYPEPOINTER) {
			if (t->base->kind == TYPEARRAY && t->base->size == 0) {
				r = t->base->u.array.size;
			} else {
				r = mkintconst(t->base->size);
			}
		} else if (t->prop & PROPINT) {
			r = mkintconst(1);
		} else if (t->prop & PROPFLOAT) {
			r = mkfltconst(t->size == 4 ? VALUE_FLTCONST : VALUE_DBLCONST, 1);
		} else {
			fatal("not a scalar");
		}
		if (e->base->qual & QUALATOMIC) {
			struct value *operand = atomictemp(f, t);
			struct value *old = atomictemp(f, t);
			struct value *args[6] = {
				lval.addr, operand, old, mkintconst(t->size),
				mkintconst(!!(e->base->qual & QUALVOLATILE)), NULL,
			};
			int classes[6] = {ptrclass, ptrclass, ptrclass, 'l', 'l', 'l'};
			int operation = e->op == TINC ? 0 : 1;
			if (!isie64())
				error(&tok.loc, "atomic access is not yet supported on this target");
			if (t->prop & PROPFLOAT)
				operation += 5;
			args[5] = mkintconst(operation);
			funcstore(f, t, QUALNONE, (struct lvalue){operand}, r);
			atomicruntimecall(f, "__ie64_atomic_fetch", 0, args, classes, 6);
			l = funcload(f, t, QUALNONE, (struct lvalue){old});
			v = funcinst(f, e->op == TINC ? IADD : ISUB, qbetype(t).base, l, r);
			return e->u.incdec.post ? l : v;
		}
		l = funcload(f, e->base->type, e->base->qual, lval);
		v = funcinst(f, e->op == TINC ? IADD : ISUB, qbetype(t).base, l, r);
		v = funcstore(f, e->type, e->qual, lval, v);
		return e->u.incdec.post ? l : v;
	case EXPRCALL:
		argvals = xreallocarray(NULL, e->u.call.nargs, sizeof(argvals[0]));
		for (arg = e->u.call.args, i = 0; arg; arg = arg->next, ++i) {
			emittype(arg->type);
			argvals[i] = funcexpr(f, arg);
		}
		t = e->type;
		emittype(t);
		if (e->u.call.ie64) {
			op = ie64inst[e->u.call.ie64];
			if (e->u.call.ie64 == IE64B_CAS) {
				funcinst(f, IIE64CASD, 0, argvals[2], NULL);
				v = funcinst(f, op, qbetype(t).base, argvals[0], argvals[1]);
			} else {
				v = funcinst(f, op, qbetype(t).base,
					e->u.call.nargs > 0 ? argvals[0] : NULL,
					e->u.call.nargs > 1 ? argvals[1] : NULL);
			}
			e = e->base;
			if (e->kind == EXPRUNARY && e->op == TBAND) {
				e = e->base;
				if (e->kind == EXPRIDENT && e->u.ident.decl->u.func.isnoreturn)
					funchlt(f);
			}
			return v;
		}
		functype = e->base->type->base;
		struct decl *param = functype->u.func.params;
		for (arg = e->u.call.args, i = 0; arg; arg = arg->next, ++i) {
			struct type *at = param ? param->type : arg->type;
			argvals[i] = ie64abiextend(f, at, argvals[i]);
			if (param)
				param = param->next;
		}
		v = funcinst(f, ICALL, abiclass(t), funcexpr(f, e->base), t->value);
		if (ie64wideint(t) && t->kind != TYPEBITINT)
			v->abiresult = t;
		param = functype->u.func.params;
		for (arg = e->u.call.args, i = 0; arg; arg = arg->next, ++i) {
			if (functype->u.func.isvararg && i == functype->u.func.nparam)
				funcinst(f, IVARARG, 0, NULL, NULL);
			t = param ? param->type : arg->type;
			funcinst(f, IARG, abiclass(t), argvals[i], t->value);
			if (param)
				param = param->next;
		}
		e = e->base;
		if (e->kind == EXPRUNARY && e->op == TBAND) {
			e = e->base;
			if (e->kind == EXPRIDENT && e->u.ident.decl->u.func.isnoreturn)
				funchlt(f);
		}
		return v;
	case EXPRUNARY:
		switch (e->op) {
		case TBAND:
			lval = funclval(f, e->base);
			return lval.addr;
		case TMUL:
			lval = funclval(f, e);
			return funcload(f, e->type, e->qual, lval);
		case TSUB:
			r = funcexpr(f, e->base);
			return funcinst(f, INEG, qbetype(e->type).base, r, NULL);
		}
		fatal("internal error; unknown unary expression");
		break;
	case EXPRCAST:
		if (e->toeval)
			funcexpr(f, e->toeval);
		l = funcexpr(f, e->base);
		return convert(f, e->type, e->base->type, l);
	case EXPRBINARY:
		if (e->op == TLOR || e->op == TLAND) {
			b[0] = mkblock("logic_true");
			b[1] = mkblock("logic_false");
			b[2] = mkblock("logic_join");

			funcbranch(f, e, b[0], b[1]);
			funclabel(f, b[0]);
			funcjmp(f, b[2]);
			funclabel(f, b[1]);

			b[2]->phi.class = 'w';
			b[2]->phi.blk[0] = b[0];
			b[2]->phi.val[0] = mkintconst(1);
			b[2]->phi.blk[1] = b[1];
			b[2]->phi.val[1] = mkintconst(0);
			functemp(f, &b[2]->phi.res);
			funclabel(f, b[2]);

			return &b[2]->phi.res;
		}
		l = funcexpr(f, e->u.binary.l);
		r = funcexpr(f, e->u.binary.r);
		t = e->u.binary.l->type;
		if (t->kind == TYPEPOINTER)
			t = &typeulong;
		switch (e->op) {
		case TMUL:
			op = IMUL;
			break;
		case TDIV:
			op = !(t->prop & PROPINT) || t->u.arith.issigned ? IDIV : IUDIV;
			break;
		case TMOD:
			op = t->u.arith.issigned ? IREM : IUREM;
			break;
		case TADD:
			op = IADD;
			break;
		case TSUB:
			op = ISUB;
			break;
		case TSHL:
			op = ISHL;
			break;
		case TSHR:
			op = t->u.arith.issigned ? ISAR : ISHR;
			break;
		case TBOR:
			op = IOR;
			break;
		case TBAND:
			op = IAND;
			break;
		case TXOR:
			op = IXOR;
			break;
		case TLESS:
			if (t->size <= 4)
				op = t->prop & PROPFLOAT ? ICLTS : t->u.arith.issigned ? ICSLTW : ICULTW;
			else
				op = t->prop & PROPFLOAT ? ICLTD : t->u.arith.issigned ? ICSLTL : ICULTL;
			break;
		case TGREATER:
			if (t->size <= 4)
				op = t->prop & PROPFLOAT ? ICGTS : t->u.arith.issigned ? ICSGTW : ICUGTW;
			else
				op = t->prop & PROPFLOAT ? ICGTD : t->u.arith.issigned ? ICSGTL : ICUGTL;
			break;
		case TLEQ:
			if (t->size <= 4)
				op = t->prop & PROPFLOAT ? ICLES : t->u.arith.issigned ? ICSLEW : ICULEW;
			else
				op = t->prop & PROPFLOAT ? ICLED : t->u.arith.issigned ? ICSLEL : ICULEL;
			break;
		case TGEQ:
			if (t->size <= 4)
				op = t->prop & PROPFLOAT ? ICGES : t->u.arith.issigned ? ICSGEW : ICUGEW;
			else
				op = t->prop & PROPFLOAT ? ICGED : t->u.arith.issigned ? ICSGEL : ICUGEL;
			break;
		case TEQL:
			if (t->size <= 4)
				op = t->prop & PROPFLOAT ? ICEQS : ICEQW;
			else
				op = t->prop & PROPFLOAT ? ICEQD : ICEQL;
			break;
		case TNEQ:
			if (t->size <= 4)
				op = t->prop & PROPFLOAT ? ICNES : ICNEW;
			else
				op = t->prop & PROPFLOAT ? ICNED : ICNEL;
			break;
		}
		if (op == INONE)
			fatal("internal error; unimplemented binary expression");
		v = funcinst(f, op, qbetype(e->type).base, l, r);
		if (t->kind == TYPEBITINT)
			v = funcbits(f, t, v, (struct bitfield){0});
		return v;
	case EXPRCOND:
		b[0] = mkblock("cond_true");
		b[1] = mkblock("cond_false");
		b[2] = mkblock("cond_join");

		v = funcbranch(f, e->base, b[0], b[1]);

		funclabel(f, b[0]);
		if (e->u.cond.t != e->base)
			v = funcexpr(f, e->u.cond.t);
		b[2]->phi.val[0] = convert(f, e->type, e->u.cond.t->type, v);
		b[2]->phi.blk[0] = f->end;
		funcjmp(f, b[2]);

		funclabel(f, b[1]);
		v = funcexpr(f, e->u.cond.f);
		b[2]->phi.val[1] = convert(f, e->type, e->u.cond.f->type, v);
		b[2]->phi.blk[1] = f->end;

		funclabel(f, b[2]);
		if (e->type == &typevoid)
			return NULL;
		functemp(f, &b[2]->phi.res);
		b[2]->phi.class = qbetype(e->type).base;
		return &b[2]->phi.res;
	case EXPRASSIGN:
		r = funcexpr(f, e->u.assign.r);
		if (e->u.assign.l->kind == EXPRTEMP) {
			e->u.assign.l->u.temp.value = r;
		} else {
			lval = funclval(f, e->u.assign.l);
			r = funcstore(f, e->u.assign.l->type, e->u.assign.l->qual, lval, r);
		}
		return r;
	case EXPRCOMMA:
		for (e = e->base; e->next; e = e->next)
			funcexpr(f, e);
		return funcexpr(f, e);
	case EXPRBUILTIN:
		switch (e->u.builtin.kind) {
		case BUILTINATOMICLOAD:
		case BUILTINATOMICSTORE:
		case BUILTINATOMICEXCHANGE:
		case BUILTINATOMICCOMPAREEXCHANGE:
		case BUILTINATOMICFETCHADD:
		case BUILTINATOMICFETCHSUB:
		case BUILTINATOMICFETCHOR:
		case BUILTINATOMICFETCHXOR:
		case BUILTINATOMICFETCHAND:
		case BUILTINATOMICCOMPOUND: {
			struct expr *object = e->base;
			struct expr *value = NULL, *expected = NULL, *success, *failure = NULL;
			struct value *objectaddr, *valuebuf = NULL, *resultbuf = NULL;
			struct value *args[6];
			int classes[6];
			int operation;
			struct type *objecttype = object->type->base;
			enum typequal objectqual = object->type->qual;
			if (!isie64())
				error(&tok.loc, "atomic operations are not yet supported on this target");

			objectaddr = funcexpr(f, object);
			arg = object->next;
			if (e->u.builtin.kind == BUILTINATOMICCOMPAREEXCHANGE) {
				expected = arg;
				arg = arg->next;
			}
			if (e->u.builtin.kind != BUILTINATOMICLOAD) {
				value = arg;
				arg = arg->next;
				valuebuf = atomictemp(f, objecttype);
				funcstore(f, objecttype, QUALNONE, (struct lvalue){valuebuf}, funcexpr(f, value));
			}
			success = arg;
			if (success->next)
				failure = success->next;
			l = funcexpr(f, success);
			r = failure ? funcexpr(f, failure) : mkintconst(0);
			atomicvalidate(f, l, r);
			if (e->u.builtin.kind == BUILTINATOMICLOAD) {
				resultbuf = atomictemp(f, objecttype);
				atomiccall(f, "__ie64_atomic_load", objectaddr, resultbuf, objecttype, objectqual);
				return funcload(f, objecttype, QUALNONE, (struct lvalue){resultbuf});
			}
			if (e->u.builtin.kind == BUILTINATOMICSTORE) {
				atomiccall(f, "__ie64_atomic_store", objectaddr, valuebuf, objecttype, objectqual);
				return NULL;
			}
			resultbuf = atomictemp(f, objecttype);
			args[0] = objectaddr; classes[0] = ptrclass;
			if (e->u.builtin.kind == BUILTINATOMICCOMPAREEXCHANGE) {
				args[1] = funcexpr(f, expected); classes[1] = ptrclass;
				args[2] = valuebuf; classes[2] = ptrclass;
				args[3] = mkintconst(objecttype->size); classes[3] = 'l';
				args[4] = mkintconst(!!(objectqual & QUALVOLATILE)); classes[4] = 'l';
				return atomicruntimecall(f, "__ie64_atomic_compare_exchange",
					qbetype(&typebool).base, args, classes, 5);
			}
			args[1] = valuebuf; classes[1] = ptrclass;
			args[2] = resultbuf; classes[2] = ptrclass;
			args[3] = mkintconst(objecttype->size); classes[3] = 'l';
			args[4] = mkintconst(!!(objectqual & QUALVOLATILE)); classes[4] = 'l';
			if (e->u.builtin.kind == BUILTINATOMICEXCHANGE) {
				atomicruntimecall(f, "__ie64_atomic_exchange", 0, args, classes, 5);
			} else {
				if (e->u.builtin.kind == BUILTINATOMICCOMPOUND) {
					switch (e->op) {
					case TADD: operation = objecttype->prop & PROPFLOAT ? 5 : 0; break;
					case TSUB: operation = objecttype->prop & PROPFLOAT ? 6 : 1; break;
					case TBOR: operation = 2; break;
					case TXOR: operation = 3; break;
					case TBAND: operation = 4; break;
					case TMUL: operation = objecttype->prop & PROPFLOAT ? 15 : 7; break;
					case TDIV: operation = objecttype->prop & PROPFLOAT ? 16
						: objecttype->u.arith.issigned ? 9 : 8; break;
					case TMOD: operation = objecttype->u.arith.issigned ? 11 : 10; break;
					case TSHL: operation = 12; break;
					case TSHR: operation = objecttype->u.arith.issigned ? 14 : 13; break;
					default: fatal("internal error: unsupported atomic compound operation");
					}
				} else {
					operation = e->u.builtin.kind - BUILTINATOMICFETCHADD;
				}
				args[5] = mkintconst(operation); classes[5] = 'l';
				atomicruntimecall(f, "__ie64_atomic_fetch", 0, args, classes, 6);
			}
			l = funcload(f, objecttype, QUALNONE, (struct lvalue){resultbuf});
			if (e->u.builtin.kind == BUILTINATOMICCOMPOUND) {
				r = funcload(f, objecttype, QUALNONE, (struct lvalue){valuebuf});
				switch (e->op) {
				case TMUL: op = IMUL; break;
				case TDIV: op = !(objecttype->prop & PROPINT) || objecttype->u.arith.issigned ? IDIV : IUDIV; break;
				case TMOD: op = objecttype->u.arith.issigned ? IREM : IUREM; break;
				case TADD: op = IADD; break;
				case TSUB: op = ISUB; break;
				case TSHL: op = ISHL; break;
				case TSHR: op = objecttype->u.arith.issigned ? ISAR : ISHR; break;
				case TBOR: op = IOR; break;
				case TBAND: op = IAND; break;
				case TXOR: op = IXOR; break;
				default: fatal("internal error: unsupported atomic compound operation");
				}
				return funcinst(f, op, qbetype(objecttype).base, l, r);
			}
			return l;
		}
		case BUILTINVASTART:
			l = funcexpr(f, e->base);
			funcinst(f, IVASTART, 0, l, NULL);
			break;
		case BUILTINVAARG:
			if (e->toeval)
				funcexpr(f, e->toeval);
			l = funcexpr(f, e->base);
			if (!(e->type->prop & PROPSCALAR)) {
				struct value *layout;

				if (strcmp(targ->name, "ie64") != 0)
					error(&tok.loc, "va_arg with non-scalar type is not yet supported");
				if (e->type->align != 1 && e->type->align != 2
				&& e->type->align != 4 && e->type->align != 8
				&& e->type->align != 16)
					error(&tok.loc, "va_arg aggregate alignment is not supported");
				layout = mkintconst(e->type->size << 8 | e->type->align);
				return funcinst(f, IIE64VAARG, ptrclass, l, layout);
			}
			return funcinst(f, IVAARG, qbetype(e->type).base, l, NULL);
		case BUILTINALLOCA:
			l = funcexpr(f, e->base);
			return funcinst(f, IALLOC16, ptrclass, l, NULL);
		case BUILTINCLZ:
		case BUILTINCLZL:
		case BUILTINCLZLL: {
			struct value *args[1];
			int classes[1];
			if (!isie64())
				error(&tok.loc, "count-leading-zeros builtin is not yet supported on this target");
			args[0] = funcexpr(f, e->base);
			classes[0] = e->u.builtin.kind == BUILTINCLZ ? 'w' : 'l';
			return atomicruntimecall(f,
				e->u.builtin.kind == BUILTINCLZ ? "__ie64_clz32" : "__ie64_clz64",
				'w', args, classes, 1);
		}
		case BUILTINUNREACHABLE:
			funchlt(f);
			return NULL;
		default:
			fatal("internal error: unimplemented builtin");
		}
		return NULL;
	case EXPRTEMP:
		assert(e->u.temp.value);
		return e->u.temp.value;
	case EXPRSIZEOF:
		t = e->u.szof.type;
		assert(t->kind == TYPEARRAY);
		calcvla(f, t);
		/* if the sizeof operand has VLA type, we must evaluate it */
		if (e->base)
			funcexpr(f, e->base);
		return t->u.array.size;
	}
	fatal("unimplemented expression %d", e->kind);
	return NULL;
}

static void
zero(struct func *func, struct value *addr, int align, unsigned long long offset, unsigned long long end)
{
	static const enum instkind store[] = {
		[1] = ISTOREB,
		[2] = ISTOREH,
		[4] = ISTOREW,
		[8] = ISTOREL,
	};
	static struct value z = {.kind = VALUE_INTCONST};
	struct value *tmp;
	int a = 1;

	while (offset < end) {
		if ((align - (offset & align - 1)) & a) {
			tmp = offset ? funcinst(func, IADD, ptrclass, addr, mkintconst(offset)) : addr;
			funcinst(func, store[a], 0, &z, tmp);
			offset += a;
		}
		if (a < align)
			a <<= 1;
	}
}

void
funcinit(struct func *func, struct decl *d, struct init *init, bool hasinit)
{
	struct lvalue dst = {0};
	struct value *src, *v;
	unsigned long long offset = 0, max = 0;
	size_t i, w;

	funcalloc(func, d);
	if (!hasinit)
		return;
	for (; init; init = init->next) {
		zero(func, d->value, d->type->align, offset, init->start);
		dst.bits = init->bits;
		if (init->expr->kind == EXPRSTRING) {
			w = init->expr->type->base->size;
			for (i = 0; i < init->expr->u.string.size && i * w < init->end - init->start; ++i) {
				v = mkintconst(init->start + i * w);
				dst.addr = funcinst(func, IADD, ptrclass, d->value, v);
				switch (w) {
				case 1: v = mkintconst(((unsigned char *)init->expr->u.string.data)[i]); break;
				case 2: v = mkintconst(((uint_least16_t *)init->expr->u.string.data)[i]); break;
				case 4: v = mkintconst(((uint_least32_t *)init->expr->u.string.data)[i]); break;
				}
				funcstore(func, init->expr->type->base, QUALNONE, dst, v);
			}
			offset = init->start + i * w;
		} else {
			if (offset < init->end && (dst.bits.before || dst.bits.after))
				zero(func, d->value, d->type->align, offset, init->end);
			dst.addr = d->value;
			/*
			QBE's memopt does not eliminate the store for ptr + 0,
			so only emit the add if the offset is non-zero
			*/
			if (init->start > 0)
				dst.addr = funcinst(func, IADD, ptrclass, dst.addr, mkintconst(init->start));
			src = funcexpr(func, init->expr);
			funcstore(func, init->expr->type, QUALNONE, dst, src);
			offset = init->end;
		}
		if (max < offset)
			max = offset;
	}
	zero(func, d->value, d->type->align, max, d->type->size);
}

static void
casesearch(struct func *f, int class, struct value *v, struct switchcase *c, struct block *defaultlabel, unsigned long long min, unsigned long long max)
{
	struct value *res, *key;
	struct block *label[3];

	if (!c) {
		funcjmp(f, defaultlabel);
		return;
	}
	if (min == max) {
		funcjmp(f, c->body);
		return;
	}
	label[0] = mkblock("switch_ne");
	label[1] = mkblock("switch_lt");
	label[2] = mkblock("switch_gt");

	key = mkintconst(c->node.key);
	res = funcinst(f, class == 'w' ? ICEQW : ICEQL, 'w', v, key);
	funcjnz(f, res, NULL, c->body, label[0]);
	funclabel(f, label[0]);
	res = funcinst(f, class == 'w' ? ICULTW : ICULTL, 'w', v, key);
	funcjnz(f, res, NULL, label[1], label[2]);
	funclabel(f, label[1]);
	casesearch(f, class, v, c->node.child[0], defaultlabel, min, c->node.key - 1);
	funclabel(f, label[2]);
	casesearch(f, class, v, c->node.child[1], defaultlabel, c->node.key + 1, max);
}

void
funcswitch(struct func *f, struct value *v, struct switchcases *c, struct block *defaultlabel)
{
	casesearch(f, qbetype(c->type).base, v, c->root, defaultlabel, 0, -1);
}

/* emit */

static void
emitname(struct value *v)
{
	static const char sigil[] = {
		[VALUE_TEMP] = '%',
		[VALUE_GLOBAL] = '$',
		[VALUE_TYPE] = ':',
		[VALUE_LABEL] = '@',
	};
	int kind;

	kind = v->kind & 0xf;
	if (kind >= countof(sigil) || !sigil[kind])
		fatal("invalid value");
	if (kind == VALUE_GLOBAL)
		noteref(v);
	putchar(sigil[kind]);
	if (kind == VALUE_GLOBAL && v->id)
		fputs(".L", stdout);
	if (v->kind & VALUE_QUOTE)
		putchar('"');
	if (v->u.name)
		fputs(v->u.name, stdout);
	if (v->kind & VALUE_QUOTE)
		putchar('"');
	if (v->id)
		printf(".%u", v->id);
}

static void
emitvalue(struct value *v)
{
	switch (v->kind & 0xf) {
	case VALUE_INTCONST:
		printf("%llu", v->u.i);
		break;
	case VALUE_FLTCONST:
		printf("s_%.17g", v->u.f);
		break;
	case VALUE_DBLCONST:
		printf("d_%.17g", v->u.f);
		break;
	case VALUE_GLOBAL:
		if (v->kind & VALUE_EXTERN)
			fputs("extern ", stdout);
		if (v->kind & VALUE_THREAD)
			fputs("thread ", stdout);
		/* fallthrough */
	default:
		emitname(v);
		break;
	}
}

static void
emitclass(int class, struct value *v)
{
	if (v && v->kind == VALUE_TYPE)
		emitname(v);
	else if (class)
		putchar(class);
	else
		fatal("type has no QBE representation");
}

/* XXX: need to consider _Alignas on struct members */
static void
emittype(struct type *t)
{
	static unsigned id;
	struct member *m, *other;
	struct type *sub;
	unsigned long long off;

	if (t->value || t->kind != TYPESTRUCT && t->kind != TYPEUNION)
		return;
	t->value = xmalloc(sizeof(*t->value));
	t->value->kind = VALUE_TYPE;
	t->value->u.name = t->u.structunion.tag;
	t->value->id = ++id;
	for (m = t->u.structunion.members; m; m = m->next) {
		for (sub = m->type; sub->kind == TYPEARRAY; sub = sub->base)
			;
		emittype(sub);
	}
	fputs("type ", stdout);
	emitname(t->value);
	if (t == targ->typevalist) {
		printf(" = align %d { %llu }\n", t->align, t->size);
		return;
	}
	fputs(" = { ", stdout);
	for (m = t->u.structunion.members, off = 0; m;) {
		if (t->kind == TYPESTRUCT) {
			/* look for a subsequent member with a larger storage unit */
			for (other = m->next; other; other = other->next) {
				if (other->offset >= ALIGNUP(m->offset + 1, 8))
					break;
				if (other->offset <= m->offset)
					m = other;
			}
			off = m->offset + m->type->size;
		} else {
			fputs("{ ", stdout);
		}
		for (sub = m->type; sub->kind == TYPEARRAY; sub = sub->base)
			;
		emitclass(qbetype(sub).data, sub->value);
		if (m->type->size > sub->size)
			printf(" %llu", m->type->size / sub->size);
		if (t->kind == TYPESTRUCT) {
			fputs(", ", stdout);
			/* skip subsequent members contained within the same storage unit */
			do m = m->next;
			while (m && m->offset < off);
		} else {
			fputs(" } ", stdout);
			m = m->next;
		}
	}
	puts("}");
}

static struct inst **
emitinst(struct inst **instp, struct inst **instend)
{
	int op, first;
	struct inst *inst = *instp;
	enum memtype mt = inst->memtype;

	putchar('\t');
	assert(inst->kind < countof(instname));
	if (inst->res.kind) {
		emitvalue(&inst->res);
		fputs(" =", stdout);
		emitclass(inst->class, inst->arg[1]);
		putchar(' ');
	}
	fputs(instname[inst->kind], stdout);
	if (inst->arg[0]) {
		putchar(' ');
		emitvalue(inst->arg[0]);
	}
	++instp;
	op = inst->kind;
	switch (op) {
	case ICALL:
		putchar('(');
		for (first = 1; instp != instend; ++instp) {
			inst = *instp;
			if (inst->kind == IVARARG) {
				fputs(", ...", stdout);
				continue;
			}
			if (inst->kind != IARG)
				break;
			if (first)
				first = 0;
			else
				fputs(", ", stdout);
			emitclass(inst->class, inst->arg[1]);
			putchar(' ');
			emitvalue(inst->arg[0]);
		}
		putchar(')');
		break;
	default:
		if (inst->arg[1]) {
			fputs(", ", stdout);
			emitvalue(inst->arg[1]);
		}
	}
	if (mt)
		printf(" #!%d", mt);
	putchar('\n');
	return instp;
}

static void
emitjump(struct jump *j)
{
	switch (j->kind) {
	case JUMP_NONE:
		break;
	case JUMP_RET:
		fputs("\tret", stdout);
		if (j->arg) {
			fputc(' ', stdout);
			emitvalue(j->arg);
		}
		putchar('\n');
		break;
	case JUMP_JMP:
		fputs("\tjmp ", stdout);
		emitname(&j->blk[0]->label);
		putchar('\n');
		break;
	case JUMP_JNZ:
		fputs("\tjnz ", stdout);
		emitvalue(j->arg);
		fputs(", ", stdout);
		emitname(&j->blk[0]->label);
		fputs(", ", stdout);
		emitname(&j->blk[1]->label);
		putchar('\n');
		break;
	case JUMP_HLT:
		fputs("\thlt\n", stdout);
		break;
	default:
		assert(0);
	}
}

void
emitfunc(struct func *f, bool global)
{
	struct block *b;
	struct inst **inst, **instend;
	struct decl *p;
	struct value *v;

	if (f->end->jump.kind == JUMP_NONE) {
		v = NULL;
		/* implicitly return 0 from main if we reach the end of the function */
		if (strcmp(f->name, "main") == 0 && f->type->base == &typeint)
			v = mkintconst(0);
		funcret(f, v);
	}
	beginunit(f->decl->value, deferrablefunc(f->decl));
	if (f->decl->weak)
		puts("weak");
	else if (global)
		puts("export");
	if (f->decl->hidden)
		puts("hidden");
	if (f->decl->section)
		printf("section %s\n", f->decl->section);
	fputs("function ", stdout);
	if (f->type->base != &typevoid) {
		emitclass(abiclass(f->type->base), f->type->base->value);
		putchar(' ');
	}
	emitname(f->decl->value);
	putchar('(');
	for (p = f->type->u.func.params, v = f->paramtemps; p; p = p->next, ++v) {
		if (p != f->type->u.func.params)
			fputs(", ", stdout);
		emitclass(abiclass(p->type), p->type->value);
		putchar(' ');
		emitname(v);
	}
	if (f->type->u.func.isvararg) {
		if (f->type->u.func.params)
			fputs(", ", stdout);
		fputs("...", stdout);
	}
	puts(") {");
	for (b = f->start; b; b = b->next) {
		emitname(&b->label);
		putchar('\n');
		if (b->phi.res.kind) {
			putchar('\t');
			emitvalue(&b->phi.res);
			printf(" =%c phi ", b->phi.class);
			emitname(&b->phi.blk[0]->label);
			putchar(' ');
			emitvalue(b->phi.val[0]);
			fputs(", ", stdout);
			emitname(&b->phi.blk[1]->label);
			putchar(' ');
			emitvalue(b->phi.val[1]);
			putchar('\n');
		}
		instend = (struct inst **)((char *)b->insts.val + b->insts.len);
		for (inst = b->insts.val; inst != instend;)
			inst = emitinst(inst, instend);
		emitjump(&b->jump);
	}
	puts("}");
	endunit();
}

static void
dataitem(struct expr *expr, unsigned long long size)
{
	struct decl *decl;
	size_t i, w;
	unsigned c;

	switch (expr->kind) {
	case EXPRUNARY:
		if (expr->op != TBAND)
			fatal("not a address expr");
		expr = expr->base;
		if (expr->kind != EXPRIDENT)
			error(&tok.loc, "initializer is not a constant expression");
		decl = expr->u.ident.decl;
		if (decl->kind == DECLOBJECT && decl->u.obj.storage != SDSTATIC)
			error(&tok.loc, "initializer is not a constant expression");
		emitname(decl->value);
		break;
	case EXPRBINARY:
		if (expr->op != TADD || expr->u.binary.l->kind != EXPRUNARY || expr->u.binary.r->kind != EXPRCONST)
			error(&tok.loc, "initializer is not a constant expression");
		dataitem(expr->u.binary.l, 0);
		fputs(" + ", stdout);
		dataitem(expr->u.binary.r, 0);
		break;
	case EXPRCONST:
		if (expr->type->prop & PROPFLOAT)
			printf("%c_%.17g", expr->type->size == 4 ? 's' : 'd', expr->u.constant.f);
		else
			printf("%llu", expr->u.constant.u);
		break;
	case EXPRSTRING:
		w = expr->type->base->size;
		if (w == 1) {
			fputc('"', stdout);
			for (i = 0; i < expr->u.string.size && i < size; ++i) {
				c = ((unsigned char *)expr->u.string.data)[i];
				if (isprint(c) && c != '"' && c != '\\')
					putchar(c);
				else
					printf("\\%03o", c);
			}
			fputc('"', stdout);
		} else {
			for (i = 0; i < expr->u.string.size && i * w < size; ++i) {
				switch (w) {
				case 2: printf("%" PRIuLEAST16 " ", ((uint_least16_t *)expr->u.string.data)[i]); break;
				case 4: printf("%" PRIuLEAST32 " ", ((uint_least32_t *)expr->u.string.data)[i]); break;
				default: assert(0);
				}
			}
		}
		if (i * w < size)
			printf(", z %llu", size - (unsigned long long)i * w);
		break;
	default:
		error(&tok.loc, "initializer is not a constant expression");
	}
}

void
emitdata(struct decl *d, struct init *init)
{
	struct init *cur;
	struct type *t;
	unsigned long long offset = 0, start, end, bits = 0;
	size_t i;
	int align;

	align = d->u.obj.align;
	for (cur = init; cur; cur = cur->next)
		cur->expr = eval(cur->expr);
	beginunit(d->value, deferrabledata(d));
	if (d->u.obj.storage == SDTHREAD)
		fputs("thread ", stdout);
	if (d->weak)
		fputs("weak ", stdout);
	else if (d->linkage == LINKEXTERN)
		fputs("export ", stdout);
	if (d->hidden)
		fputs("hidden ", stdout);
	if (d->section)
		printf("section %s ", d->section);
	fputs("data ", stdout);
	emitname(d->value);
	printf(" = align %d { ", align);

	while (init) {
		cur = init;
		while ((init = init->next) && init->start * 8 + init->bits.before < cur->end * 8 - cur->bits.after) {
			/*
			XXX: Currently, if multiple union members are
			initialized, these assertions may not hold.
			(https://todo.sr.ht/~mcf/cproc/38)
			*/
			assert(cur->expr->kind == EXPRSTRING);
			assert(init->expr->kind == EXPRCONST);
			i = (init->start - cur->start) / cur->expr->type->base->size;
			switch (cur->expr->type->base->size) {
			case 1: ((unsigned char *)cur->expr->u.string.data)[i]  = init->expr->u.constant.u; break;
			case 2: ((uint_least16_t *)cur->expr->u.string.data)[i] = init->expr->u.constant.u; break;
			case 4: ((uint_least32_t *)cur->expr->u.string.data)[i] = init->expr->u.constant.u; break;
			}
		}
		start = cur->start + cur->bits.before / 8;
		end = cur->end - (cur->bits.after + 7) / 8;
		if (offset < start && bits) {
			printf("b %u, ", (unsigned)bits);  /* unfinished byte from previous bit-field */
			++offset;
			bits = 0;
		}
		if (offset < start)
			printf("z %llu, ", start - offset);
		if (cur->bits.before || cur->bits.after) {
			/* little-endian target specific */
			assert(cur->expr->type->prop & PROPINT);
			assert(cur->expr->kind == EXPRCONST);
			bits |= cur->expr->u.constant.u << cur->bits.before % 8;
			for (offset = start; offset < end; ++offset, bits >>= 8)
				printf("b %u, ", (unsigned)bits & 0xff);
			/*
			clear the upper `after` bits in the last byte,
			or all bits when `after` is 0 (we ended on a
			byte boundary).
			*/
			bits &= 0x7f >> (cur->bits.after + 7) % 8;
		} else {
			t = cur->expr->type;
			if (t->kind == TYPEARRAY)
				t = t->base;
			printf("%c ", qbetype(t).data);
			dataitem(cur->expr, cur->end - cur->start);
			fputs(", ", stdout);
		}
		offset = end;
	}
	if (bits) {
		printf("b %u, ", (unsigned)bits);
		++offset;
	}
	assert(offset <= d->type->size);
	if (offset < d->type->size)
		printf("z %llu ", d->type->size - offset);
	puts("}");
	endunit();
}

void
emitarrayref(struct decl *d, bool fini, int priority)
{
	static unsigned long serial;
	const char *section = fini ? ".fini_array" : ".init_array";

	if (priority != 65535)
		printf("section \"%s.%05d\"\n", section, priority);
	else
		printf("section \"%s\"\n", section);
	printf("data $__cproc_%s_%lu = align 8 { l ", fini ? "dtor" : "ctor", serial++);
	emitname(d->value);
	puts(" }");
}

/*
Write the buffered output, leaving out the deferred units that nothing
reachable from the always-emitted output refers to.
*/
void
emitfinish(void)
{
	struct seg *seg, *next;
	struct sym **stack, *sym;
	size_t i, n, cap;

	closeseg();
	cap = 64;
	n = 0;
	stack = xreallocarray(NULL, cap, sizeof(stack[0]));
	if (symtab.cap) {
		for (i = 0; i < symtab.cap; ++i) {
			if (!symtab.keys[i].str)
				continue;
			sym = symtab.vals[i].p;
			if (!sym->rooted || sym->kept)
				continue;
			sym->kept = true;
			if (n == cap) {
				cap *= 2;
				stack = xreallocarray(stack, cap, sizeof(stack[0]));
			}
			stack[n++] = sym;
		}
	}
	while (n > 0) {
		sym = stack[--n];
		for (i = 0; i < sym->nrefs; ++i) {
			if (sym->refs[i]->kept)
				continue;
			sym->refs[i]->kept = true;
			if (n == cap) {
				cap *= 2;
				stack = xreallocarray(stack, cap, sizeof(stack[0]));
			}
			stack[n++] = sym->refs[i];
		}
	}
	free(stack);
	for (seg = segs; seg; seg = next) {
		next = seg->next;
		if ((!seg->sym || seg->sym->kept) && fwrite(seg->buf, 1, seg->len, realout()) != seg->len)
			fatal("write failed");
		free(seg->buf);
		free(seg);
	}
	segs = NULL;
	segsend = &segs;
}
