#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"
#include "utf.h"
#include "cc.h"

static struct expr *
mkexpr(enum exprkind k, struct type *t, struct expr *b)
{
	struct expr *e;

	e = xmalloc(sizeof(*e));
	e->qual = QUALNONE;
	e->type = t;
	e->lvalue = false;
	e->decayed = false;
	e->kind = k;
	e->base = b;
	e->next = NULL;
	e->toeval = NULL;

	return e;
}

void
delexpr(struct expr *e)
{
	struct expr *sub;

	switch (e->kind) {
	case EXPRCALL:
		delexpr(e->base);
		while (sub = e->u.call.args) {
			e->u.call.args = sub->next;
			delexpr(sub);
		}
		break;
	case EXPRBITFIELD:
	case EXPRINCDEC:
	case EXPRUNARY:
	case EXPRCAST:
		delexpr(e->base);
		break;
	case EXPRBINARY:
		delexpr(e->u.binary.l);
		delexpr(e->u.binary.r);
		break;
	case EXPRCOND:
		delexpr(e->base);
		if (e->u.cond.t != e->base)
			delexpr(e->u.cond.t);
		delexpr(e->u.cond.f);
		break;
	/*
	XXX: compound assignment causes some reuse of expressions,
	so we can't free them without risk of a double-free

	case EXPRASSIGN:
		delexpr(e->assign.l);
		delexpr(e->assign.r);
		break;
	*/
	case EXPRCOMMA:
		while (sub = e->base) {
			e->base = sub->next;
			delexpr(sub);
		}
		break;
	}
	free(e);
}

static struct expr *
mkconstexpr(struct type *t, unsigned long long n)
{
	struct expr *e;

	e = mkexpr(EXPRCONST, t, NULL);
	e->u.constant.u = n;

	return e;
}

static struct expr *mkunaryexpr(enum tokenkind, struct expr *);

/* 6.3.2.1 Conversion of arrays and function designators */
static struct expr *
decay(struct expr *e)
{
	struct type *t;
	enum typequal tq;

	/* XXX: combine with decl.c:adjust in some way? */
	t = e->type;
	tq = e->qual;
	switch (t->kind) {
	case TYPEARRAY:
		/*
		XXX: qualifiers should be applied to the element
		type of array types, not the array type itself

		assert(tq == QUALNONE);
		*/
		e = mkunaryexpr(TBAND, e);
		e->type = mkpointertype(t->base, t->qual | tq);
		e->decayed = true;
		break;
	case TYPEFUNC:
		e = mkunaryexpr(TBAND, e);
		e->decayed = true;
		break;
	}

	return e;
}

static struct expr *
mkunaryexpr(enum tokenkind op, struct expr *base)
{
	struct expr *expr;
	struct type *type;

	switch (op) {
	case TBAND:
		if (base->decayed) {
			expr = base;
			base = base->base;
			free(expr);
		}
		/*
		Allow struct and union types even if they are not lvalues,
		since we take their address when compiling member access.
		*/
		if (!base->lvalue && base->type->kind != TYPEFUNC && base->type->kind != TYPESTRUCT && base->type->kind != TYPEUNION)
			error(&tok.loc, "'&' operand is not an lvalue or function designator");
		if (base->kind == EXPRBITFIELD)
			error(&tok.loc, "cannot take address of bit-field");
		expr = mkexpr(EXPRUNARY, mkpointertype(base->type, base->qual), base);
		expr->op = op;
		return expr;
	case TMUL:
		if (base->type->kind != TYPEPOINTER)
			error(&tok.loc, "cannot dereference non-pointer");
		if (base->kind == EXPRUNARY && base->op == TBAND && base->base->kind != EXPRSTRING) {
			type = base->type->base;
			expr = base->base;
			expr->type = type;
		} else {
			expr = mkexpr(EXPRUNARY, base->type->base, base);
			expr->qual = base->type->qual;
			expr->lvalue = true;
			expr->op = op;
		}
		return decay(expr);
	}
	/* other unary operators get compiled as equivalent binary ones */
	fatal("internal error: unknown unary operator %d", op);
	return NULL;
}

static unsigned
bitfieldwidth(struct expr *e)
{
	if (e->kind != EXPRBITFIELD)
		return -1;
	return e->type->size * 8 - e->u.bitfield.bits.before - e->u.bitfield.bits.after;
}

static struct expr *
exprconvert(struct expr *e, struct type *t)
{
	if (typecompatible(e->type, t))
		return e;
	return mkexpr(EXPRCAST, t, e);
}

static bool
nullpointer(struct expr *e)
{
	if (e->kind != EXPRCONST)
		return false;
	if (e->type->kind == TYPENULLPTR)
		return true;
	if (!(e->type->prop & PROPINT) && (e->type->kind != TYPEPOINTER || e->type->base != &typevoid))
		return false;
	return e->u.constant.u == 0;
}

struct expr *
exprassign(struct expr *e, struct type *t)
{
	struct type *et;
	enum typequal eq, tq;

	et = e->type;
	switch (t->kind) {
	case TYPEBOOL:
		if (!(et->prop & PROPARITH) && et->kind != TYPEPOINTER && et->kind != TYPENULLPTR)
			error(&tok.loc, "assignment to bool must be from arithmetic, pointer, or nullptr_t type");
		break;
	case TYPEPOINTER:
		{
		bool basescompatible;
		if (nullpointer(e))
			break;
		if (et->kind != TYPEPOINTER)
			error(&tok.loc, "assignment to pointer must be from pointer or null pointer constant");
		basescompatible = typecompatible(t->base, et->base);
		if (t->base->kind == TYPEARRAY && et->base->kind == TYPEARRAY) {
			struct type targetpointer = *t, sourcepointer = *et;
			struct type targetarray = *t->base, sourcearray = *et->base;
			targetpointer.qual = sourcepointer.qual = QUALNONE;
			targetarray.qual = sourcearray.qual = QUALNONE;
			targetpointer.base = &targetarray;
			sourcepointer.base = &sourcearray;
			basescompatible = typecompatible(&targetpointer, &sourcepointer);
		}
		if (t->base != &typevoid && et->base != &typevoid && !basescompatible)
			error(&tok.loc, "base types of pointer assignment must be compatible or void");
		eq = et->qual;
		tq = t->qual;
		if (et->base->kind == TYPEARRAY)
			eq |= et->base->qual;
		if (t->base->kind == TYPEARRAY)
			tq |= t->base->qual;
		if ((eq & tq) != eq)
			error(&tok.loc, "assignment to pointer discards qualifiers");
		break;
		}
	case TYPENULLPTR:
		if (!nullpointer(e))
			error(&tok.loc, "assignment to nullptr_t must be from null pointer constant or expression with type nullptr_t");
		break;
	case TYPESTRUCT:
	case TYPEUNION:
		if (!typecompatible(t, et))
			error(&tok.loc, "assignment to %s type must be from compatible type", t->kind == TYPEUNION ? "union" : "struct");
		break;
	default:
		assert(t->prop & PROPARITH);
		if (!(et->prop & PROPARITH))
			error(&tok.loc, "assignment to arithmetic type must be from arithmetic type");
		break;
	}
	return exprconvert(e, t);
}

struct expr *
exprpromote(struct expr *e)
{
	struct type *t;

	t = typepromote(e->type, bitfieldwidth(e));
	return exprconvert(e, t);
}

static struct type *
commonreal(struct expr **e1, struct expr **e2)
{
	struct type *t;

	t = typecommonreal((*e1)->type, bitfieldwidth(*e1), (*e2)->type, bitfieldwidth(*e2));
	*e1 = exprconvert(*e1, t);
	*e2 = exprconvert(*e2, t);

	return t;
}

static struct expr *
mksizeofexpr(struct type *t) {
	struct expr *e;

	if (t->kind == TYPEARRAY && t->size == 0) {
		e = mkexpr(EXPRSIZEOF, &typeulong, NULL);
		e->u.szof.type = t;
	} else {
		e = mkconstexpr(&typeulong, t->size);
	}

	return e;
}

static struct expr *
mkbinaryexpr(struct location *loc, enum tokenkind op, struct expr *l, struct expr *r)
{
	struct expr *e;
	struct type *t = NULL;
	enum typeprop lp, rp;

	lp = l->type->prop;
	rp = r->type->prop;
	switch (op) {
	case TLOR:
	case TLAND:
		if (!(lp & PROPSCALAR))
			error(loc, "left operand of '%s' operator must be scalar", tokenstr(op));
		if (!(rp & PROPSCALAR))
			error(loc, "right operand of '%s' operator must be scalar", tokenstr(op));
		t = &typeint;
		break;
	case TEQL:
	case TNEQ:
		t = &typeint;
		if (lp & PROPARITH && rp & PROPARITH) {
			commonreal(&l, &r);
			break;
		}
		if (l->type->kind != TYPEPOINTER)
			e = l, l = r, r = e;
		if (l->type->kind != TYPEPOINTER)
			error(loc, "invalid operands to '%s' operator", tokenstr(op));
		if (nullpointer(eval(r))) {
			r = exprconvert(r, l->type);
			break;
		}
		if (nullpointer(eval(l))) {
			l = exprconvert(l, r->type);
			break;
		}
		if (r->type->kind != TYPEPOINTER)
			error(loc, "invalid operands to '%s' operator", tokenstr(op));
		if (l->type->base->kind == TYPEVOID)
			e = l, l = r, r = e;
		if (r->type->base->kind == TYPEVOID && l->type->base->kind != TYPEFUNC)
			r = exprconvert(r, l->type);
		else if (!typecompatible(l->type->base, r->type->base))
			error(loc, "pointer operands to '%s' operator are to incompatible types", tokenstr(op));
		break;
	case TLESS:
	case TGREATER:
	case TLEQ:
	case TGEQ:
		t = &typeint;
		if (lp & PROPREAL && rp & PROPREAL) {
			commonreal(&l, &r);
		} else if (l->type->kind == TYPEPOINTER && r->type->kind == TYPEPOINTER) {
			if (!typecompatible(l->type->base, r->type->base) || l->type->base->kind == TYPEFUNC)
				error(loc, "pointer operands to '%s' operator must be to compatible object types", tokenstr(op));
		} else {
			error(loc, "invalid operands to '%s' operator", tokenstr(op));
		}
		break;
	case TBOR:
	case TXOR:
	case TBAND:
		t = commonreal(&l, &r);
		break;
	case TADD:
		if (lp & PROPARITH && rp & PROPARITH) {
			t = commonreal(&l, &r);
			break;
		}
		if (r->type->kind == TYPEPOINTER)
			e = l, l = r, r = e, rp = lp;
		if (l->type->kind != TYPEPOINTER || !(rp & PROPINT))
			error(loc, "invalid operands to '+' operator");
		t = l->type;
		if (t->base->incomplete || t->base->kind == TYPEFUNC)
			error(loc, "pointer operand to '+' must be to complete object type");
		r = mkbinaryexpr(loc, TMUL, exprconvert(r, &typeulong), mksizeofexpr(t->base));
		break;
	case TSUB:
		if (lp & PROPARITH && rp & PROPARITH) {
			t = commonreal(&l, &r);
			break;
		}
		if (l->type->kind != TYPEPOINTER || !(rp & PROPINT) && r->type->kind != TYPEPOINTER)
			error(loc, "invalid operands to '-' operator");
		if (l->type->base->incomplete || l->type->base->kind == TYPEFUNC)
			error(loc, "pointer operand to '-' must be to complete object type");
		if (rp & PROPINT) {
			t = l->type;
			r = mkbinaryexpr(loc, TMUL, exprconvert(r, &typeulong), mksizeofexpr(t->base));
		} else {
			if (!typecompatible(l->type->base, r->type->base))
				error(&tok.loc, "pointer operands to '-' are to incompatible types");
			op = TDIV;
			t = &typelong;
			e = mkbinaryexpr(loc, TSUB, exprconvert(l, &typelong), exprconvert(r, &typelong));
			r = mksizeofexpr(l->type->base);
			l = e;
		}
		break;
	case TMOD:
		if (!(lp & PROPINT) || !(rp & PROPINT))
			error(loc, "operands to '%%' operator must be integer");
		t = commonreal(&l, &r);
		break;
	case TMUL:
	case TDIV:
		if (!(lp & PROPARITH) || !(rp & PROPARITH))
			error(loc, "operands to '%s' operator must be arithmetic", tokenstr(op));
		t = commonreal(&l, &r);
		break;
	case TSHL:
	case TSHR:
		if (!(lp & PROPINT) || !(rp & PROPINT))
			error(loc, "operands to '%s' operator must be integer", tokenstr(op));
		l = exprpromote(l);
		r = exprpromote(r);
		t = l->type;
		break;
	default:
		fatal("internal error: unknown binary operator %d", op);
	}
	e = mkexpr(EXPRBINARY, t, NULL);
	e->op = op;
	e->u.binary.l = l;
	e->u.binary.r = r;

	return e;
}

static struct type *
inttype(unsigned long long val, bool decimal, char *suffix)
{
	enum {
		U = 1,
		L = 2,
		LL = 4,
		WB = 8,
	};
	static struct type *const types[] = {
		&typeint,
		&typeuint,   /* u */
		&typelong,   /* l */
		&typeulong,  /* ul */
		&typellong,  /* ll */
		&typeullong, /* ull */
	};
	int flags, i, step;
	bool sign;
	char *s;
	struct type *t;

	/* parse suffix */
	flags = 0;
	for (s = suffix; *s; ++s) {
		int new;

		switch (*s) {
		case 'u':
		case 'U': new = U; break;
		case 'l':
		case 'L': new = s[1] == s[0] ? (++s, LL) : L; break;
		case 'w': if (s[1] != 'b') goto invalid; ++s, new = WB; break;
		case 'W': if (s[1] != 'B') goto invalid; ++s, new = WB; break;
		default:
		invalid: error(&tok.loc, "invalid integer constant suffix '%s'", suffix);
		}
		if (flags & new || flags & ~U && new & ~U)
			goto invalid;
		flags |= new;
	}

	/* find integer type */
	sign = !(flags & U);
	if (flags & WB) {
		for (i = 1;; ++i) {
			if (i > 64 - sign)
				goto notype;
			if (val <= 0xffffffffffffffff >> 64 - i)
				break;
		}
		return mkbitinttype(i + sign, sign);
	}
	assert(flags < countof(types));
	step = !sign || decimal ? 2 : 1;
	for (i = flags; i < countof(types); i += step) {
		t = types[i];
		if (typehasint(t, val, false))
			return t;
	}
notype:
	error(&tok.loc, "no suitable type for constant '%s'", tok.lit);
}

static int
octval(int c)
{
	if ('0' <= c && c <= '7')
		return c - '0';
	return -1;
}

static int
hexval(int c)
{
	if ('0' <= c && c <= '9')
		return c - '0';
	if ('a' <= c && c <= 'f')
		return c - 'a' + 10;
	if ('A' <= c && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

size_t
decodechar(const char *src, uint_least32_t *chr, bool *hexoct, const char *desc, struct location *loc)
{
	uint_least32_t c;
	size_t n;
	int i, v;
	const unsigned char *s = (const unsigned char *)src;

	*hexoct = false;
	if (*s == '\\') {
		++s;
		switch (*s) {
		case '\'':
		case '"':
		case '?':
		case '\\': c = *s;   ++s; break;
		case 'a':  c = '\a'; ++s; break;
		case 'b':  c = '\b'; ++s; break;
		case 'f':  c = '\f'; ++s; break;
		case 'n':  c = '\n'; ++s; break;
		case 'r':  c = '\r'; ++s; break;
		case 't':  c = '\t'; ++s; break;
		case 'v':  c = '\v'; ++s; break;
		case 'x':
			++s;
			c = 0;
			v = hexval(*s);
			assert(v >= 0);
			do {
				if (c > 0xffffffff / 16)
					error(&tok.loc, "character constant escape is out of range");
				c = c * 16 + v;
				v = hexval(*++s);
			} while (v >= 0);
			*hexoct = true;
			break;
		default:
			c = 0;
			v = octval(*s);
			assert(v >= 0);
			i = 0;
			do {
				if (c > 0xffffffff / 8)
					error(&tok.loc, "character constant escape is out of range");
				c = c * 8 + v;
				v = octval(*++s);
			} while (v >= 0 && ++i < 3);
			*hexoct = true;
		}
	} else {
		n = utf8dec(&c, s, 4);
		if (n == (size_t)-1)
			error(loc, "%s contains invalid UTF-8", desc);
		s += n;
	}
	*chr = c;
	return s - (const unsigned char *)src;
}

static size_t
encodechar8(void *dst, uint_least32_t chr, bool hexoct)
{
	if (!hexoct)
		return utf8enc(dst, chr);
	*(unsigned char *)dst = chr;
	return 1;
}

static size_t
encodechar16(void *dst, uint_least32_t chr, bool hexoct)
{
	if (!hexoct)
		return utf16enc(dst, chr) * sizeof(uint_least16_t);
	*(uint_least16_t *)dst = chr;
	return sizeof(uint_least16_t);
}

static size_t
encodechar32(void *dst, uint_least32_t chr, bool hexoct)
{
	*(uint_least32_t *)dst = chr;
	return sizeof(uint_least32_t);
}

struct type *
stringconcat(struct stringlit *str, bool forceutf8)
{
	static struct array parts;
	struct {
		struct location loc;
		char *str;
	} *p;
	int kind, newkind;
	struct type *t;
	size_t (*encodechar)(void *, uint_least32_t, bool);
	char *src;
	unsigned char *buf, *dst;
	uint_least32_t chr;
	bool hexoct;
	size_t len, width;

	assert(tok.kind == TSTRINGLIT);
	parts.len = 0;
	len = 0;
	kind = 0;
	do {
		src = tok.lit;
		switch (*src) {
		case 'u': if (src[1] == '8') ++src; /* fallthrough */
		case 'L':
		case 'U': newkind = *src, ++src; break;
		case '"': newkind = 0; break;
		default: assert(0);
		}
		if (kind != newkind && kind && newkind)
			error(&tok.loc, "adjacent string literals have differing prefixes");
		if (newkind)
			kind = newkind;
		p = arrayadd(&parts, sizeof(*p));
		p->loc = tok.loc;
		p->str = src + 1;
		len += strlen(src) - 2;
		next();
	} while (tok.kind == TSTRINGLIT);
	if (forceutf8)
		kind = '8';
	++len;  /* null byte */
	switch (kind) {
	case 0: t = &typechar; break;
	case '8': t = &typeuchar; break;
	case 'u': t = &typeushort; break;
	case 'U': t = &typeuint; break;
	case 'L': t = targ->typewchar; break;
	default: assert(0);
	}
	switch (t->size) {
	case 1:
		width = 1;
		encodechar = encodechar8;
		break;
	case 2:
		width = sizeof(uint_least16_t);
		encodechar = encodechar16;
		break;
	case 4:
		width = sizeof(uint_least32_t);
		encodechar = encodechar32;
		break;
	default:
		assert(0);
	}
	buf = xreallocarray(NULL, len, width);
	str->data = buf;
	dst = buf;
	arrayforeach(&parts, p) {
		src = p->str;
		while (*src != '"') {
			src += decodechar(src, &chr, &hexoct, "string literal", &p->loc);
			dst += encodechar(dst, chr, hexoct);
		}
	}
	dst += encodechar(dst, 0, false);
	str->size = (dst - buf) / width;
	return t;
}

static struct expr *
generic(struct scope *s)
{
	struct expr *e, *match = NULL, *def = NULL;
	struct type *t, *want;
	enum typequal qual;

	next();
	expect(TLPAREN, "after '_Generic'");
	e = assignexpr(s);
	expect(TCOMMA, "after generic selector expression");
	want = e->type;
	delexpr(e);
	do {
		if (consume(TDEFAULT)) {
			if (def)
				error(&tok.loc, "multiple default expressions in generic association list");
			expect(TCOLON, "after 'default'");
			def = assignexpr(s);
		} else {
			qual = QUALNONE;
			t = typename(s, &qual, NULL);
			if (!t)
				error(&tok.loc, "expected typename for generic association");
			if (t->kind == TYPEFUNC)
				error(&tok.loc, "generic association must have object type");
			if (t->incomplete)
				error(&tok.loc, "generic association must have complete type");
			if (t->prop & PROPVM)
				error(&tok.loc, "generic association has variably modified type");
			expect(TCOLON, "after type name");
			e = assignexpr(s);
			if (typecompatible(t, want) && qual == QUALNONE) {
				if (match)
					error(&tok.loc, "generic selector matches multiple associations");
				match = e;
			} else {
				delexpr(e);
			}
		}
	} while (consume(TCOMMA));
	expect(TRPAREN, "after generic assocation list");
	if (!match) {
		if (!def)
			error(&tok.loc, "generic selector matches no associations and no default was specified");
		match = def;
	} else if (def) {
		delexpr(def);
	}
	return match;
}

/* 6.5 Expressions */
static struct expr *
primaryexpr(struct scope *s)
{
	struct expr *e;
	struct decl *d;
	struct type *t;
	char *src, *end;
	uint_least32_t chr;
	unsigned long long val;
	bool hexoct, ordinary;
	int base;

	switch (tok.kind) {
	case TSTRINGLIT:
		e = mkexpr(EXPRSTRING, NULL, NULL);
		t = stringconcat(&e->u.string, false);
		e->type = mkarraytype(t, QUALNONE, e->u.string.size);
		e->lvalue = true;
		e = decay(e);
		break;
	case TCHARCONST:
		src = tok.lit;
		ordinary = false;
		switch (*src) {
		case 'L': ++src; t = targ->typewchar; break;
		case 'u': ++src; t = *src == '8' ? ++src, &typeuchar : &typeushort; break;
		case 'U': ++src; t = &typeuint; break;
		default: t = &typeuchar, ordinary = true;
		}
		assert(*src == '\'');
		++src;
		src += decodechar(src, &chr, &hexoct, "character constant", &tok.loc);
		if (hexoct && !typehasint(t, chr, false))
			error(&tok.loc, "character constant escape is out of range");
		val = chr;
		if (ordinary) {
			if (typechar.u.arith.issigned)
				val = (val ^ 0x80) - 0x80;
			t = &typeint;
		}
		e = mkconstexpr(t, val);
		if (*src != '\'')
			error(&tok.loc, "character constant contains more than one character: %c", *src);
		next();
		break;
	case TNUMBER:
		e = mkexpr(EXPRCONST, NULL, NULL);
		if (tok.lit[0] == '0') {
			switch (tok.lit[1]) {
			case 'x': case 'X': base = 16; break;
			case 'b': case 'B': base = 2; break;
			default: base = 8; break;
			}
		} else {
			base = 10;
		}
		if (strpbrk(tok.lit, base == 16 ? ".pP" : ".eE")) {
			/* floating constant */
			e->u.constant.f = strtod(tok.lit, &end);
			if (end == tok.lit)
				error(&tok.loc, "invalid floating constant '%s'", tok.lit);
			if (!end[0])
				e->type = &typedouble;
			else if ((end[0] == 'f' || end[0] == 'F') && !end[1])
				e->type = &typefloat;
			else if ((end[0] == 'l' || end[0] == 'L') && !end[1]) {
				if (targ->nolongdouble)
					error(&tok.loc, "long double is not supported on this target");
				e->type = &typeldouble;
			}
			else
				error(&tok.loc, "invalid floating constant suffix '%s'", end);
		} else {
			src = tok.lit;
			if (base == 2)
				src += 2;
			/* integer constant */
			e->u.constant.u = strtoull(src, &end, base);
			if (end == src)
				error(&tok.loc, "invalid integer constant '%s'", tok.lit);
			e->type = inttype(e->u.constant.u, base == 10, end);
		}
		next();
		break;
	case TTRUE:
	case TFALSE:
		e = mkexpr(EXPRCONST, &typebool, NULL);
		e->u.constant.u = tok.kind == TTRUE;
		next();
		break;
	case TNULLPTR:
		e = mkexpr(EXPRCONST, &typenullptr, NULL);
		e->u.constant.u = 0;
		next();
		break;
	case TLPAREN:
		next();
		e = expr(s);
		expect(TRPAREN, "after expression");
		break;
	case T_GENERIC:
		e = generic(s);
		break;
	default:
		if (tok.kind >= TIDENT) {
			d = scopegetdecl(s, tokenstr(tok.kind), 1);
			if (!d)
				error(&tok.loc, "undeclared identifier: %s", tokenstr(tok.kind));
			e = mkexpr(EXPRIDENT, d->type, NULL);
			e->qual = d->qual;
			e->lvalue = d->kind == DECLOBJECT;
			e->u.ident.decl = d;
			if (d->kind != DECLBUILTIN)
				e = decay(e);
			next();
			break;
		}
		error(&tok.loc, "expected primary expression");
	}

	return e;
}

/* TODO: merge with init.c:designator() */
static void
designator(struct scope *s, struct type *t, unsigned long long *offset)
{
	char *name;
	struct member *m;
	unsigned long long i;

	for (;;) {
		switch (tok.kind) {
		case TLBRACK:
			if (t->kind != TYPEARRAY)
				error(&tok.loc, "index designator is only valid for array types");
			next();
			i = intconstexpr(s, false);
			expect(TRBRACK, "for index designator");
			t = t->base;
			*offset += i * t->size;
			break;
		case TPERIOD:
			if (t->kind != TYPESTRUCT && t->kind != TYPEUNION)
				error(&tok.loc, "member designator only valid for struct/union types");
			next();
			name = expect(TIDENT, "for member designator");
			m = typemember(t, name, offset);
			if (!m)
				error(&tok.loc, "%s has no member named '%s'", t->kind == TYPEUNION ? "union" : "struct", name);
			t = m->type;
			break;
		default:
			return;
		}
	}
}

static struct expr *
builtinatomic(struct scope *s, enum builtinkind kind)
{
	struct expr *object, *value, *expected, *success, *failure, *e;
	struct type *t;
	unsigned allowed;

	object = assignexpr(s);
	if (object->type->kind != TYPEPOINTER || !(object->type->qual & QUALATOMIC))
		error(&tok.loc, "atomic builtin requires a pointer to an atomic object");
	t = object->type->base;
	expected = NULL;
	value = NULL;
	if (kind != BUILTINATOMICLOAD) {
		expect(TCOMMA, "after atomic object");
		if (kind == BUILTINATOMICCOMPAREEXCHANGE) {
			expected = assignexpr(s);
			if (expected->type->kind != TYPEPOINTER || expected->type->qual & QUALCONST
			|| !typecompatible(expected->type->base, t))
				error(&tok.loc, "atomic compare-exchange expected argument has incompatible type");
			expect(TCOMMA, "after compare-exchange expected argument");
		}
		value = assignexpr(s);
		if ((kind == BUILTINATOMICFETCHADD || kind == BUILTINATOMICFETCHSUB)
		&& t->kind == TYPEPOINTER) {
			if (!(value->type->prop & PROPINT))
				error(&tok.loc, "atomic pointer fetch operand must have integer type");
			value = mkbinaryexpr(&tok.loc, TMUL, exprconvert(value, &typelong),
				mksizeofexpr(t->base));
		} else {
			value = exprassign(value, t);
		}
	}
	expect(TCOMMA, "before atomic memory order");
	success = eval(assignexpr(s));
	allowed = kind == BUILTINATOMICLOAD ? (1u<<0 | 1u<<1 | 1u<<2 | 1u<<5)
		: kind == BUILTINATOMICSTORE ? (1u<<0 | 1u<<3 | 1u<<5) : 0x3f;
	if (success->kind == EXPRCONST && (success->u.constant.u > 5
	|| !(allowed & 1u << success->u.constant.u)))
		error(&tok.loc, "invalid memory order for atomic operation");
	failure = NULL;
	if (kind == BUILTINATOMICCOMPAREEXCHANGE) {
		expect(TCOMMA, "before compare-exchange failure order");
		failure = eval(assignexpr(s));
		if (failure->kind == EXPRCONST && (failure->u.constant.u > 5
		|| failure->u.constant.u == 3 || failure->u.constant.u == 4))
			error(&tok.loc, "invalid compare-exchange failure memory order");
		if (success->kind == EXPRCONST && failure->kind == EXPRCONST) {
			static const unsigned failureok[] = {
				1u<<0,
				1u<<0 | 1u<<1,
				1u<<0 | 1u<<1 | 1u<<2,
				1u<<0,
				1u<<0 | 1u<<1 | 1u<<2,
				1u<<0 | 1u<<1 | 1u<<2 | 1u<<5,
			};
			if (!(failureok[success->u.constant.u] & 1u << failure->u.constant.u))
				error(&tok.loc, "compare-exchange failure order is stronger than success order");
		}
	}
	e = mkexpr(EXPRBUILTIN,
		kind == BUILTINATOMICSTORE ? &typevoid
		: kind == BUILTINATOMICCOMPAREEXCHANGE ? &typebool : t,
		object);
	e->u.builtin.kind = kind;
	object->next = expected ? expected : value ? value : success;
	if (expected) {
		expected->next = value;
		value->next = success;
	} else if (value) {
		value->next = success;
	}
	if (failure)
		success->next = failure;
	return e;
}

static struct expr *
builtinfunc(struct scope *s, enum builtinkind kind)
{
	struct expr *e, *toeval;
	struct type *t;
	struct member *m;
	char *name;
	unsigned long long offset;

	switch (kind) {
	case BUILTINATOMICLOAD:
	case BUILTINATOMICSTORE:
	case BUILTINATOMICEXCHANGE:
	case BUILTINATOMICCOMPAREEXCHANGE:
	case BUILTINATOMICFETCHADD:
	case BUILTINATOMICFETCHSUB:
	case BUILTINATOMICFETCHOR:
	case BUILTINATOMICFETCHXOR:
	case BUILTINATOMICFETCHAND:
		e = builtinatomic(s, kind);
		break;
	case BUILTINALLOCA:
		e = exprassign(assignexpr(s), &typeulong);
		e = mkexpr(EXPRBUILTIN, mkpointertype(&typevoid, QUALNONE), e);
		e->u.builtin.kind = BUILTINALLOCA;
		break;
	case BUILTINCONSTANTP:
		e = mkconstexpr(&typeint, eval(condexpr(s))->kind == EXPRCONST);
		break;
	case BUILTINEXPECT:
		/* just a no-op for now */
		/* TODO: check that the expression and the expected value have type 'long' */
		e = assignexpr(s);
		expect(TCOMMA, "after expression");
		delexpr(assignexpr(s));
		break;
	case BUILTINCLZ:
	case BUILTINCLZL:
	case BUILTINCLZLL:
		e = assignexpr(s);
		if (!(e->type->prop & PROPINT))
			error(&tok.loc, "count-leading-zeros argument must have integer type");
		e = exprconvert(e, kind == BUILTINCLZ ? &typeuint : &typeulong);
		e = mkexpr(EXPRBUILTIN, &typeint, e);
		e->u.builtin.kind = kind;
		break;
	case BUILTININFF:
		e = mkexpr(EXPRCONST, &typefloat, NULL);
		/* TODO: use INFINITY here when we can handle musl's math.h */
		e->u.constant.f = strtod("inf", NULL);
		break;
	case BUILTINNANF:
		e = assignexpr(s);
		if (!e->decayed || e->base->kind != EXPRSTRING || e->base->u.string.size > 1)
			error(&tok.loc, "__builtin_nanf currently only supports empty string literals");
		e = mkexpr(EXPRCONST, &typefloat, NULL);
		/* TODO: use NAN here when we can handle musl's math.h */
		e->u.constant.f = strtod("nan", NULL);
		break;
	case BUILTINOFFSETOF:
		t = typename(s, NULL, NULL);
		expect(TCOMMA, "after type name");
		name = expect(TIDENT, "after ','");
		if (t->kind != TYPESTRUCT && t->kind != TYPEUNION)
			error(&tok.loc, "type is not a struct/union type");
		offset = 0;
		m = typemember(t, name, &offset);
		if (!m)
			error(&tok.loc, "struct/union has no member named '%s'", name);
		designator(s, m->type, &offset);
		e = mkconstexpr(&typeulong, offset);
		break;
	case BUILTINTYPESCOMPATIBLEP:
		t = typename(s, NULL, NULL);
		expect(TCOMMA, "after type name");
		e = mkconstexpr(&typeint, typecompatible(t, typename(s, NULL, NULL)));
		break;
	case BUILTINUNREACHABLE:
		e = mkexpr(EXPRBUILTIN, &typevoid, NULL);
		e->u.builtin.kind = BUILTINUNREACHABLE;
		break;
	case BUILTINVAARG:
		e = mkexpr(EXPRBUILTIN, NULL, assignexpr(s));
		e->u.builtin.kind = BUILTINVAARG;
		if (!typesame(e->base->type, typeadjvalist))
			error(&tok.loc, "va_arg argument must have type va_list");
		if (typeadjvalist == targ->typevalist)
			e->base = mkunaryexpr(TBAND, e->base);
		expect(TCOMMA, "after va_list");
		e->type = typename(s, &e->qual, &toeval);
		e->toeval = toeval;
		break;
	case BUILTINVACOPY:
		e = mkexpr(EXPRASSIGN, &typevoid, NULL);
		e->u.assign.l = assignexpr(s);
		if (!typesame(e->u.assign.l->type, typeadjvalist))
			error(&tok.loc, "va_copy destination must have type va_list");
		if (typeadjvalist != targ->typevalist)
			e->u.assign.l = mkunaryexpr(TMUL, e->u.assign.l);
		expect(TCOMMA, "after target va_list");
		e->u.assign.r = assignexpr(s);
		if (!typesame(e->u.assign.r->type, typeadjvalist))
			error(&tok.loc, "va_copy source must have type va_list");
		if (typeadjvalist != targ->typevalist)
			e->u.assign.r = mkunaryexpr(TMUL, e->u.assign.r);
		break;
	case BUILTINVAEND:
		e = assignexpr(s);
		if (!typesame(e->type, typeadjvalist))
			error(&tok.loc, "va_end argument must have type va_list");
		e = mkexpr(EXPRCAST, &typevoid, e);
		break;
	case BUILTINVASTART:
		e = mkexpr(EXPRBUILTIN, &typevoid, assignexpr(s));
		e->u.builtin.kind = BUILTINVASTART;
		if (!typesame(e->base->type, typeadjvalist))
			error(&tok.loc, "va_start argument must have type va_list");
		if (typeadjvalist == targ->typevalist)
			e->base = mkunaryexpr(TBAND, e->base);
		if (consume(TCOMMA))
			delexpr(assignexpr(s));
		break;
	default:
		fatal("internal error; unknown builtin");
	}
	return e;
}

struct ie64builtin {
	const char *name;
	enum ie64builtinkind kind;
	int immarg;
	unsigned long long maximm;
};

static const struct ie64builtin ie64builtins[] = {
	{"__builtin_ie64_nop", IE64B_NOP, -1, 0},
	{"__builtin_ie64_halt", IE64B_HALT, -1, 0},
	{"__builtin_ie64_sei", IE64B_SEI, -1, 0},
	{"__builtin_ie64_cli", IE64B_CLI, -1, 0},
	{"__builtin_ie64_tlbflush", IE64B_TLBFLUSH, -1, 0},
	{"__builtin_ie64_suaen", IE64B_SUAEN, -1, 0},
	{"__builtin_ie64_suadis", IE64B_SUADIS, -1, 0},
	{"__builtin_ie64_eret", IE64B_ERET, -1, 0},
	{"__builtin_ie64_rti", IE64B_RTI, -1, 0},
	{"__builtin_ie64_smode", IE64B_SMODE, -1, 0},
	{"__builtin_ie64_fmovsr", IE64B_FMOVSR, -1, 0},
	{"__builtin_ie64_fmovcr", IE64B_FMOVCR, -1, 0},
	{"__builtin_ie64_tlbinval", IE64B_TLBINVAL, -1, 0},
	{"__builtin_ie64_wait", IE64B_WAIT, 0, 0xffffffffu},
	{"__builtin_ie64_syscall", IE64B_SYSCALL, 0, 0xffffffffu},
	{"__builtin_ie64_mfcr", IE64B_MFCR, 0, 15},
	{"__builtin_ie64_fmovecr", IE64B_FMOVECR, 0, 15},
	{"__builtin_ie64_dmovecr", IE64B_DMOVECR, 0, 15},
	{"__builtin_ie64_fmovsc", IE64B_FMOVSC, -1, 0},
	{"__builtin_ie64_fmovcc", IE64B_FMOVCC, -1, 0},
	{"__builtin_ie64_mtcr", IE64B_MTCR, 0, 15},
	{"__builtin_ie64_fmod", IE64B_FMOD, -1, 0},
	{"__builtin_ie64_dmod", IE64B_DMOD, -1, 0},
	{"__builtin_ie64_fabs", IE64B_FABS, -1, 0},
	{"__builtin_ie64_dabs", IE64B_DABS, -1, 0},
	{"__builtin_ie64_fint", IE64B_FINT, -1, 0},
	{"__builtin_ie64_dint", IE64B_DINT, -1, 0},
	{"__builtin_ie64_fcvtsd", IE64B_FCVTSD, -1, 0},
	{"__builtin_ie64_fcvtds", IE64B_FCVTDS, -1, 0},
	{"__builtin_ie64_fsin", IE64B_FSIN, -1, 0},
	{"__builtin_ie64_fcos", IE64B_FCOS, -1, 0},
	{"__builtin_ie64_ftan", IE64B_FTAN, -1, 0},
	{"__builtin_ie64_fatan", IE64B_FATAN, -1, 0},
	{"__builtin_ie64_flog", IE64B_FLOG, -1, 0},
	{"__builtin_ie64_fexp", IE64B_FEXP, -1, 0},
	{"__builtin_ie64_fpow", IE64B_FPOW, -1, 0},
	{"__builtin_ie64_fsqrt", IE64B_FSQRT, -1, 0},
	{"__builtin_ie64_dsin", IE64B_DSIN, -1, 0},
	{"__builtin_ie64_dcos", IE64B_DCOS, -1, 0},
	{"__builtin_ie64_dtan", IE64B_DTAN, -1, 0},
	{"__builtin_ie64_datan", IE64B_DATAN, -1, 0},
	{"__builtin_ie64_dlog", IE64B_DLOG, -1, 0},
	{"__builtin_ie64_dexp", IE64B_DEXP, -1, 0},
	{"__builtin_ie64_dpow", IE64B_DPOW, -1, 0},
	{"__builtin_ie64_dsqrt", IE64B_DSQRT, -1, 0},
	{"__builtin_ie64_xchg", IE64B_XCHG, -1, 0},
	{"__builtin_ie64_faa", IE64B_FAA, -1, 0},
	{"__builtin_ie64_fand", IE64B_FAND, -1, 0},
	{"__builtin_ie64_for", IE64B_FOR, -1, 0},
	{"__builtin_ie64_fxor", IE64B_FXOR, -1, 0},
	{"__builtin_ie64_cas", IE64B_CAS, -1, 0},
	{"__builtin_ie64_frsqrt", IE64B_FRSQRT, -1, 0},
	{"__builtin_ie64_fmin", IE64B_FMIN, -1, 0},
	{"__builtin_ie64_fmax", IE64B_FMAX, -1, 0},
	{"__builtin_ie64_fsincos", IE64B_FSINCOS, -1, 0},
	{"__builtin_ie64_fround", IE64B_FROUND, -1, 0},
	{"__builtin_ie64_ftrunc", IE64B_FTRUNC, -1, 0},
	{"__builtin_ie64_ffloor", IE64B_FFLOOR, -1, 0},
	{"__builtin_ie64_fceil", IE64B_FCEIL, -1, 0},
	{"__builtin_ie64_fma", IE64B_FMA, -1, 0},
	{"__builtin_ie64_drsqrt", IE64B_DRSQRT, -1, 0},
	{"__builtin_ie64_dmin", IE64B_DMIN, -1, 0},
	{"__builtin_ie64_dmax", IE64B_DMAX, -1, 0},
	{"__builtin_ie64_dsincos", IE64B_DSINCOS, -1, 0},
	{"__builtin_ie64_dround", IE64B_DROUND, -1, 0},
	{"__builtin_ie64_dtrunc", IE64B_DTRUNC, -1, 0},
	{"__builtin_ie64_dfloor", IE64B_DFLOOR, -1, 0},
	{"__builtin_ie64_dceil", IE64B_DCEIL, -1, 0},
	{"__builtin_ie64_dfma", IE64B_DFMA, -1, 0},
	{"__builtin_ie64_fipr", IE64B_FIPR, -1, 0},
	{"__builtin_ie64_ftrv", IE64B_FTRV, -1, 0},
};

static const char *const ie64builtinsig[IE64B_COUNT] = {
	[IE64B_NOP] = "v", [IE64B_HALT] = "v",
	[IE64B_SEI] = "v", [IE64B_CLI] = "v",
	[IE64B_TLBFLUSH] = "v", [IE64B_SUAEN] = "v",
	[IE64B_SUADIS] = "v", [IE64B_ERET] = "v", [IE64B_RTI] = "v",
	[IE64B_SMODE] = "U", [IE64B_FMOVSR] = "U", [IE64B_FMOVCR] = "U",
	[IE64B_TLBINVAL] = "vU", [IE64B_WAIT] = "vu",
	[IE64B_SYSCALL] = "Uu", [IE64B_MFCR] = "Uu",
	[IE64B_FMOVECR] = "fu", [IE64B_DMOVECR] = "du",
	[IE64B_FMOVSC] = "vu", [IE64B_FMOVCC] = "vu",
	[IE64B_MTCR] = "vuU", [IE64B_FMOD] = "fff", [IE64B_DMOD] = "ddd",
	[IE64B_FABS] = "ff", [IE64B_DABS] = "dd",
	[IE64B_FINT] = "ff", [IE64B_DINT] = "dd",
	[IE64B_FCVTSD] = "df", [IE64B_FCVTDS] = "fd",
	[IE64B_FSIN] = "ff", [IE64B_FCOS] = "ff", [IE64B_FTAN] = "ff",
	[IE64B_FATAN] = "ff", [IE64B_FLOG] = "ff", [IE64B_FEXP] = "ff",
	[IE64B_FPOW] = "fff", [IE64B_FSQRT] = "ff",
	[IE64B_DSIN] = "dd", [IE64B_DCOS] = "dd", [IE64B_DTAN] = "dd",
	[IE64B_DATAN] = "dd", [IE64B_DLOG] = "dd", [IE64B_DEXP] = "dd",
	[IE64B_DPOW] = "ddd", [IE64B_DSQRT] = "dd",
	[IE64B_XCHG] = "UpU", [IE64B_FAA] = "UpU",
	[IE64B_FAND] = "UpU", [IE64B_FOR] = "UpU", [IE64B_FXOR] = "UpU",
	[IE64B_CAS] = "UpUU",
	[IE64B_FRSQRT] = "ff",
	[IE64B_FMIN] = "fff",
	[IE64B_FMAX] = "fff",
	[IE64B_FSINCOS] = "vfPP",
	[IE64B_FROUND] = "ff",
	[IE64B_FTRUNC] = "ff",
	[IE64B_FFLOOR] = "ff",
	[IE64B_FCEIL] = "ff",
	[IE64B_FMA] = "ffff",
	[IE64B_DRSQRT] = "dd",
	[IE64B_DMIN] = "ddd",
	[IE64B_DMAX] = "ddd",
	[IE64B_DSINCOS] = "vdDD",
	[IE64B_DROUND] = "dd",
	[IE64B_DTRUNC] = "dd",
	[IE64B_DFLOOR] = "dd",
	[IE64B_DCEIL] = "dd",
	[IE64B_DFMA] = "dddd",
	[IE64B_FIPR] = "fcc", [IE64B_FTRV] = "vcP",
};

static bool
ie64builtintype(struct type *t, int code)
{
	switch (code) {
	case 'v': return t == &typevoid;
	case 'u': return typesame(t, &typeuint);
	case 'U': return typesame(t, &typeulong);
	case 'f': return typesame(t, &typefloat);
	case 'd': return typesame(t, &typedouble);
	case 'p':
		return t->kind == TYPEPOINTER && typesame(t->base, &typeulong)
			&& t->qual == QUALVOLATILE;
	case 'c':  /* const float * */
		return t->kind == TYPEPOINTER && typesame(t->base, &typefloat)
			&& t->qual == QUALCONST;
	case 'D':
		return t->kind == TYPEPOINTER && typesame(t->base, &typedouble)
			&& t->qual == QUALNONE;
	case 'P':  /* float * */
		return t->kind == TYPEPOINTER && typesame(t->base, &typefloat)
			&& t->qual == QUALNONE;
	default: fatal("internal error: invalid IE64 builtin signature");
	}
}

static bool
ie64builtindecl(struct type *t, const char *sig)
{
	struct decl *p;

	if (t->kind != TYPEFUNC || t->u.func.isvararg
	|| !ie64builtintype(t->base, *sig++))
		return false;
	for (p=t->u.func.params; p && *sig; p=p->next, sig++)
		if (!ie64builtintype(p->type, *sig))
			return false;
	return !p && !*sig;
}

static struct decl *
calleedecl(struct expr *e)
{
	if (e->kind == EXPRUNARY && e->op == TBAND)
		e = e->base;
	return e->kind == EXPRIDENT ? e->u.ident.decl : NULL;
}

static void
recognizeie64builtin(struct expr *call)
{
	const struct ie64builtin *b;
	struct decl *d;
	struct expr **argp, *arg, *next;
	int n;

	if (strcmp(targ->name, "ie64") != 0 || !(d = calleedecl(call->base)))
		return;
	for (b=ie64builtins; b<ie64builtins + countof(ie64builtins); b++)
		if (strcmp(d->name, b->name) == 0)
			break;
	if (b == ie64builtins + countof(ie64builtins))
		return;
	if ((b->kind == IE64B_FIPR || b->kind == IE64B_FTRV) && !ie64fpvec)
		error(&tok.loc, "%s needs the -mfpvec option", b->name);
	if (b->kind >= IE64B_FRSQRT && b->kind <= IE64B_DFMA) {
		bool fused = b->kind == IE64B_FMA || b->kind == IE64B_DFMA;
		if (fused ? !ie64fma : !ie64fpext)
			error(&tok.loc, "%s needs the %s option", b->name, fused ? "-mfma" : "-mfpext");
	}
	if (!ie64builtindecl(d->type, ie64builtinsig[b->kind]))
		error(&tok.loc, "%s has an incompatible declaration", b->name);
	call->u.call.ie64 = b->kind;
	if (b->immarg < 0)
		return;
	argp = &call->u.call.args;
	for (n=0; n<b->immarg; n++)
		argp = &(*argp)->next;
	arg = *argp;
	next = arg->next;
	arg = eval(arg);
	arg->next = next;
	*argp = arg;
	if (arg->kind != EXPRCONST || !(arg->type->prop & PROPINT))
		error(&tok.loc, "%s argument %d must be an integer constant expression",
			b->name, b->immarg + 1);
	if (arg->u.constant.u > b->maximm)
		error(&tok.loc, "%s argument %d is outside the supported immediate range",
			b->name, b->immarg + 1);
}

static struct expr *
mkincdecexpr(enum tokenkind op, struct expr *base, bool post)
{
	struct expr *e;

	if (!base->lvalue)
		error(&tok.loc, "operand of '%s' operator must be an lvalue", tokenstr(op));
	if (base->qual & QUALCONST)
		error(&tok.loc, "operand of '%s' operator is const qualified", tokenstr(op));
	e = mkexpr(EXPRINCDEC, base->type, base);
	e->op = op;
	e->u.incdec.post = post;
	return e;
}

static struct expr *
postfixexpr(struct scope *s, struct expr *r)
{
	struct expr *e, *arr, *idx, *tmp, **end;
	struct type *t;
	struct decl *p;
	struct member *m;
	unsigned long long offset;
	enum typequal tq;
	enum tokenkind op;
	bool lvalue;

	if (!r)
		r = primaryexpr(s);
	for (;;) {
		switch (tok.kind) {
		case TLBRACK:  /* subscript */
			next();
			arr = r;
			idx = expr(s);
			if (arr->type->kind != TYPEPOINTER) {
				if (idx->type->kind != TYPEPOINTER)
					error(&tok.loc, "either array or index must be pointer type");
				tmp = arr;
				arr = idx;
				idx = tmp;
			}
			if (arr->type->base->incomplete)
				error(&tok.loc, "array is pointer to incomplete type");
			if (!(idx->type->prop & PROPINT))
				error(&tok.loc, "index is not an integer type");
			e = mkunaryexpr(TMUL, mkbinaryexpr(&tok.loc, TADD, arr, idx));
			expect(TRBRACK, "after array index");
			break;
		case TLPAREN:  /* function call */
			next();
			if (r->kind == EXPRIDENT && r->u.ident.decl->kind == DECLBUILTIN) {
				e = builtinfunc(s, r->u.ident.decl->u.builtin);
				expect(TRPAREN, "after builtin parameters");
				break;
			}
			if (r->type->kind != TYPEPOINTER || r->type->base->kind != TYPEFUNC)
				error(&tok.loc, "called object is not a function");
			t = r->type->base;
			e = mkexpr(EXPRCALL, t->base, r);
			e->u.call.args = NULL;
			e->u.call.nargs = 0;
			e->u.call.ie64 = IE64B_NONE;
			p = t->u.func.params;
			end = &e->u.call.args;
			while (tok.kind != TRPAREN) {
				if (e->u.call.args)
					expect(TCOMMA, "or ')' after function call argument");
				if (!p && !t->u.func.isvararg)
					error(&tok.loc, "too many arguments for function call");
				*end = assignexpr(s);
				if (t->u.func.isvararg && !p)
					*end = exprpromote(*end);
				else
					*end = exprassign(*end, p->type);
				end = &(*end)->next;
				++e->u.call.nargs;
				if (p)
					p = p->next;
			}
			if (p && !t->u.func.isvararg)
				error(&tok.loc, "not enough arguments for function call");
			e = decay(e);
			recognizeie64builtin(e);
			next();
			break;
		case TPERIOD:
			r = mkunaryexpr(TBAND, r);
			/* fallthrough */
		case TARROW:
			op = tok.kind;
			if (r->type->kind != TYPEPOINTER)
				error(&tok.loc, "'%s' operator must be applied to pointer to struct/union", tokenstr(op));
			t = r->type->base;
			tq = r->type->qual;
			if (t->kind != TYPESTRUCT && t->kind != TYPEUNION)
				error(&tok.loc, "'%s' operator must be applied to pointer to struct/union", tokenstr(op));
			next();
			if (tok.kind < TIDENT)
				error(&tok.loc, "expected identifier after '%s' operator", tokenstr(op));
			lvalue = op == TARROW || r->base->lvalue;
			offset = 0;
			m = typemember(t, tokenstr(tok.kind), &offset);
			if (!m)
				error(&tok.loc, "struct/union has no member named '%s'", tok.lit);
			r = mkbinaryexpr(&tok.loc, TADD, exprconvert(r, &typeulong), mkconstexpr(&typeulong, offset));
			r->type = mkpointertype(m->type, tq | m->qual);
			r = mkunaryexpr(TMUL, r);
			r->lvalue = lvalue;
			if (m->bits.before || m->bits.after) {
				e = mkexpr(EXPRBITFIELD, r->type, r);
				e->lvalue = lvalue;
				e->u.bitfield.bits = m->bits;
			} else {
				e = r;
			}
			next();
			break;
		case TINC:
		case TDEC:
			e = mkincdecexpr(tok.kind, r, true);
			next();
			break;
		default:
			return r;
		}
		r = e;
	}
}

static struct expr *castexpr(struct scope *);

static struct expr *
unaryexpr(struct scope *s)
{
	enum tokenkind op;
	struct expr *e, *l;
	struct type *t;

	op = tok.kind;
	switch (op) {
	case TINC:
	case TDEC:
		next();
		l = unaryexpr(s);
		e = mkincdecexpr(op, l, false);
		break;
	case TBAND:
	case TMUL:
		next();
		return mkunaryexpr(op, castexpr(s));
	case TADD:
		next();
		e = castexpr(s);
		if (!(e->type->prop & PROPARITH))
			error(&tok.loc, "operand of unary '+' operator must have arithmetic type");
		if (e->type->prop & PROPINT)
			e = exprpromote(e);
		break;
	case TSUB:
		next();
		e = castexpr(s);
		if (!(e->type->prop & PROPARITH))
			error(&tok.loc, "operand of unary '-' operator must have arithmetic type");
		if (e->type->prop & PROPINT)
			e = exprpromote(e);
		e = mkexpr(EXPRUNARY, e->type, e);
		e->op = op;
		break;
	case TBNOT:
		next();
		e = castexpr(s);
		if (!(e->type->prop & PROPINT))
			error(&tok.loc, "operand of '~' operator must have integer type");
		e = exprpromote(e);
		e = mkbinaryexpr(&tok.loc, TXOR, e, mkconstexpr(e->type, -1));
		break;
	case TLNOT:
		next();
		e = castexpr(s);
		if (!(e->type->prop & PROPSCALAR))
			error(&tok.loc, "operator '!' must have scalar operand");
		e = mkbinaryexpr(&tok.loc, TEQL, e, mkconstexpr(&typeint, 0));
		break;
	case TSIZEOF:
	case TALIGNOF:
	case T__ALIGNOF__:
		next();
		if (consume(TLPAREN)) {
			t = typename(s, NULL, NULL);
			if (t) {
				expect(TRPAREN, "after type name");
				/* might be part of a compound literal */
				if (op == TSIZEOF && tok.kind == TLBRACE)
					parseinit(s, t);
				e = NULL;
			} else {
				e = expr(s);
				expect(TRPAREN, "after expression");
				if (op == TSIZEOF)
					e = postfixexpr(s, e);
			}
		} else if (op == TSIZEOF) {
			t = NULL;
			e = unaryexpr(s);
		} else {
			error(&tok.loc, "expected ')' after 'alignof'");
		}
		if (!t) {
			if (e->decayed)
				e = e->base;
			if (e->kind == EXPRBITFIELD)
				error(&tok.loc, "%s operator applied to bitfield expression", tokenstr(op));
			t = e->type;
		}
		if (t->incomplete)
			error(&tok.loc, "%s operator applied to incomplete type", tokenstr(op));
		if (t->kind == TYPEFUNC)
			error(&tok.loc, "%s operator applied to function type", tokenstr(op));
		if (t->kind == TYPEARRAY && t->size == 0 && op == TSIZEOF) {
			e = mkexpr(EXPRSIZEOF, &typeulong, e);
			e->u.szof.type = t;
		} else {
			/*
			XXX: __alignof__ is not the same as alignof on 32-bit archs
			this needs to be considered if we gain support for those archs
			*/
			e = mkconstexpr(&typeulong, op == TSIZEOF ? t->size : t->align);
		}
		break;
	default:
		e = postfixexpr(s, NULL);
	}

	return e;
}

static struct expr *
castexpr(struct scope *s)
{
	struct type *t, *ct;
	struct decl *d;
	enum typequal tq;
	struct expr *r, *e, **end, *toeval;
	enum tokenkind compoundsc;

	ct = NULL;
	end = &r;
	while (consume(TLPAREN)) {
		compoundsc = TNONE;
		if (tok.kind == TSTATIC || tok.kind == TREGISTER || tok.kind == TCONSTEXPR
		|| tok.kind == TTHREAD_LOCAL) {
			compoundsc = tok.kind;
			next();
		}
		tq = QUALNONE;
		t = typename(s, &tq, &toeval);
		if (!t) {
			if (compoundsc != TNONE)
				error(&tok.loc, "storage class requires a compound literal");
			e = expr(s);
			expect(TRPAREN, "after expression to match '('");
			e = postfixexpr(s, e);
			goto done;
		}
		expect(TRPAREN, "after type name");
		if (tok.kind == TLBRACE) {
			struct init *init;
			struct init *part;
			if (compoundsc == TTHREAD_LOCAL)
				error(&tok.loc, "thread_local compound literals are not supported by this target");
			if (compoundsc == TCONSTEXPR)
				tq |= QUALCONST;
			e = mkexpr(EXPRCOMPOUND, t, NULL);
			e->toeval = toeval;
			e->qual = tq;
			e->lvalue = true;
			d = mkdecl(NULL, DECLOBJECT, t, tq, LINKNONE);
			d->u.obj.storage = s == &filescope || compoundsc == TSTATIC
				|| compoundsc == TCONSTEXPR ? SDSTATIC : SDAUTO;
			e->u.compound.decl = d;
			init = parseinit(s, t);
			if (compoundsc == TCONSTEXPR)
				for (part = init; part; part = part->next)
					if (eval(part->expr)->kind != EXPRCONST)
						error(&tok.loc, "constexpr compound literal initializer is not constant");
			e->u.compound.init = init;
			if (s != &filescope && d->u.obj.storage == SDSTATIC) {
				d->value = mkglobal(d);
				emitdata(d, init);
				d->defined = true;
			}
			e = postfixexpr(s, decay(e));
			goto done;
		}
		if (compoundsc != TNONE)
			error(&tok.loc, "storage class is only valid on a compound literal");
		if (t != &typevoid && !(t->prop & PROPSCALAR))
			error(&tok.loc, "cast type must be scalar");
		e = mkexpr(EXPRCAST, t, NULL);
		e->toeval = toeval;
		*end = e;
		end = &e->base;
		ct = t;
	}
	e = unaryexpr(s);

done:
	if (ct && ct != &typevoid && !(e->type->prop & PROPSCALAR))
		error(&tok.loc, "cast operand must have scalar type");
	*end = e;
	return r;
}

static int
precedence(enum tokenkind t)
{
	switch (t) {
	case TLOR:     return 0;
	case TLAND:    return 1;
	case TBOR:     return 2;
	case TXOR:     return 3;
	case TBAND:    return 4;
	case TEQL:
	case TNEQ:     return 5;
	case TLESS:
	case TGREATER:
	case TLEQ:
	case TGEQ:     return 6;
	case TSHL:
	case TSHR:     return 7;
	case TADD:
	case TSUB:     return 8;
	case TMUL:
	case TDIV:
	case TMOD:     return 9;
	}
	return -1;
}

static struct expr *
binaryexpr(struct scope *s, struct expr *l, int i)
{
	struct expr *r;
	struct location loc;
	enum tokenkind op;
	int j, k;

	if (!l)
		l = castexpr(s);
	while ((j = precedence(tok.kind)) >= i) {
		op = tok.kind;
		loc = tok.loc;
		next();
		r = castexpr(s);
		while ((k = precedence(tok.kind)) > j)
			r = binaryexpr(s, r, k);
		l = mkbinaryexpr(&loc, op, l, r);
	}
	return l;
}

struct expr *
condexpr(struct scope *s)
{
	struct expr *e, *l, *r;
	struct type *t, *lt, *rt;
	enum typequal tq;

	e = binaryexpr(s, NULL, 0);
	if (!consume(TQUESTION))
		return e;
	l = tok.kind == TCOLON ? e : expr(s);
	expect(TCOLON, "in conditional expression");
	r = condexpr(s);

	lt = l->type;
	rt = r->type;
	if (lt == rt) {
		t = lt;
	} else if (lt->prop & PROPARITH && rt->prop & PROPARITH) {
		t = typecommonreal(lt, bitfieldwidth(l), rt, bitfieldwidth(r));
	} else if (lt == &typevoid && rt == &typevoid) {
		t = &typevoid;
	} else {
		l = eval(l);
		r = eval(r);
		if (nullpointer(l) && rt->kind == TYPEPOINTER) {
			t = rt;
		} else if (nullpointer(r) && lt->kind == TYPEPOINTER) {
			t = lt;
		} else if (lt->kind == TYPEPOINTER && rt->kind == TYPEPOINTER) {
			tq = lt->qual | rt->qual;
			lt = lt->base;
			rt = rt->base;
			if (lt == &typevoid || rt == &typevoid) {
				t = &typevoid;
			} else if (typecompatible(lt, rt)) {
				t = typecomposite(lt, rt);
			} else {
				error(&tok.loc, "operands of conditional operator must have compatible types");
			}
			t = mkpointertype(t, tq);
		} else {
			error(&tok.loc, "invalid operands to conditional operator");
		}
	}
	e = eval(e);
	if (e->kind == EXPRCONST && e->type->prop & PROPINT)
		return exprconvert(e->u.constant.u ? l : r, t);
	e = mkexpr(EXPRCOND, t, e);
	e->u.cond.t = l;
	e->u.cond.f = r;
	return e;
}

unsigned long long
intconstexpr(struct scope *s, bool allowneg)
{
	struct expr *e;

	e = eval(condexpr(s));
	if (e->kind != EXPRCONST || !(e->type->prop & PROPINT))
		error(&tok.loc, "not an integer constant expression");
	if (!allowneg && e->type->u.arith.issigned && e->u.constant.u >> 63)
		error(&tok.loc, "integer constant expression cannot be negative");
	return e->u.constant.u;
}

static struct expr *
mkassignexpr(struct expr *l, struct expr *r)
{
	struct expr *e;

	e = mkexpr(EXPRASSIGN, l->type, NULL);
	e->u.assign.l = l;
	e->u.assign.r = exprassign(r, l->type);
	return e;
}

struct expr *
assignexpr(struct scope *s)
{
	struct expr *e, *l, *r, *tmp, *bit;
	enum tokenkind op;

	l = condexpr(s);
	if (l->kind == EXPRBINARY || l->kind == EXPRCOMMA || l->kind == EXPRCAST)
		return l;
	switch (tok.kind) {
	case TASSIGN:     op = TNONE; break;
	case TMULASSIGN:  op = TMUL;  break;
	case TDIVASSIGN:  op = TDIV;  break;
	case TMODASSIGN:  op = TMOD;  break;
	case TADDASSIGN:  op = TADD;  break;
	case TSUBASSIGN:  op = TSUB;  break;
	case TSHLASSIGN:  op = TSHL;  break;
	case TSHRASSIGN:  op = TSHR;  break;
	case TBANDASSIGN: op = TBAND; break;
	case TXORASSIGN:  op = TXOR;  break;
	case TBORASSIGN:  op = TBOR;  break;
	default:
		return l;
	}
	if (!l->lvalue)
		error(&tok.loc, "left side of assignment expression is not an lvalue");
	next();
	r = assignexpr(s);
	if (!op)
		return mkassignexpr(l, r);
	if (l->qual & QUALATOMIC) {
		struct expr *binary, *object, *order;
		binary = mkbinaryexpr(&tok.loc, op, l, r);
		object = mkunaryexpr(TBAND, l);
		order = mkconstexpr(&typeint, 5);
		e = mkexpr(EXPRBUILTIN, l->type, object);
		e->u.builtin.kind = BUILTINATOMICCOMPOUND;
		e->op = op;
		object->next = binary->u.binary.r;
		binary->u.binary.r->next = order;
		return e;
	}
	/* rewrite `E1 OP= E2` as `T = &E1, *T = *T OP E2`, where T is a temporary slot */
	if (l->kind == EXPRBITFIELD) {
		bit = l;
		l = l->base;
	} else {
		bit = NULL;
	}
	tmp = mkexpr(EXPRTEMP, mkpointertype(l->type, l->qual), NULL);
	tmp->lvalue = true;
	tmp->u.temp.value = NULL;
	tmp->u.temp.origin = bit ? NULL : l;
	e = mkassignexpr(tmp, mkunaryexpr(TBAND, l));
	l = mkunaryexpr(TMUL, tmp);
	if (bit) {
		bit->base = l;
		l = bit;
	}
	r = mkbinaryexpr(&tok.loc, op, l, r);
	e->next = mkassignexpr(l, r);
	return mkexpr(EXPRCOMMA, l->type, e);
}

struct expr *
expr(struct scope *s)
{
	struct expr *r, *e, **end;

	end = &r;
	for (;;) {
		e = assignexpr(s);
		*end = e;
		end = &e->next;
		if (tok.kind != TCOMMA)
			break;
		next();
	}
	if (!r->next)
		return r;
	return mkexpr(EXPRCOMMA, e->type, r);
}
