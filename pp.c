#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include "util.h"
#include "cc.h"

struct macroparam {
	char *name;
	enum {
		PARAMTOK = 1<<0,  /* the parameter is used normally */
		PARAMSTR = 1<<1,  /* the parameter is used with the '#' operator */
		PARAMVAR = 1<<2,  /* the parameter is __VA_ARGS__ */
	} flags;
};

struct macroarg {
	struct token *token;
	size_t ntoken;
	struct token *rawtoken;
	size_t nraw;
	/* stringized argument */
	struct token str;
};

struct macro {
	enum {
		MACROOBJ,
		MACROFUNC,
	} kind;
	char *name;
	/* whether or not this macro is ineligible for expansion */
	bool hide;
	/* parameters of function-like macro */
	struct macroparam *param;
	size_t nparam;
	/* argument tokens of macro invocation */
	struct macroarg *arg;
	/* replacement list */
	struct token *token;
	size_t ntoken;
	struct array paste;
};

struct frame {
	struct token *token;
	size_t ntoken;
	struct macro *macro;
	bool owner;
};

enum ppflags ppflags;
bool ppwarningsareerrors;

static struct array ctx;
static struct array macros;
/* number of macros currently undergoing expansion */
static size_t macrodepth;
static struct array includepaths[4];
struct dependency { char *path; bool system; };
static struct array dependencies;
struct conditional { bool parent, active, seen, hadelse; };
static struct array conditionals;
static struct array includeconditionals;

static size_t
conditionalbase(void)
{
	size_t *depth;

	depth = arraylast(&includeconditionals, sizeof *depth);
	return depth ? *depth : 0;
}

static bool
ppactive(void)
{
	struct conditional *c;

	if (!conditionals.len)
		return true;
	c = arraylast(&conditionals, sizeof *c);
	return c->active;
}

void
ppaddinclude(enum ppinclude kind, const char *path)
{
	char *copy = xmalloc(strlen(path) + 1);
	strcpy(copy, path);
	arrayaddptr(&includepaths[kind], copy);
}

static char *
includeopen(const char *name, const struct location *loc, bool quote, bool *system)
{
	char path[PATH_MAX], **dir, *copy;
	const char *slash;
	FILE *f;
	enum ppinclude first, kind;

	if (quote && name[0] == '/') {
		if ((f = fopen(name, "r"))) {
			fclose(f);
			if (system) *system = false;
			copy = xmalloc(strlen(name) + 1);
			return strcpy(copy, name);
		}
		return NULL;
	}

	if (quote && loc->file) {
		slash = strrchr(loc->file, '/');
		if (slash) {
			if ((size_t)(slash - loc->file) + 1 + strlen(name) + 1 > sizeof path)
				goto includepaths;
			memcpy(path, loc->file, slash - loc->file + 1);
			strcpy(path + (slash - loc->file + 1), name);
		} else if (snprintf(path, sizeof path, "%s", name) >= (int)sizeof path) {
			goto includepaths;
		}
		if ((f = fopen(path, "r"))) {
			fclose(f);
			if (system) *system = false;
			copy = xmalloc(strlen(path) + 1);
			return strcpy(copy, path);
		}
	}
	includepaths:
	first = quote ? PPINCLUDE_QUOTE : PPINCLUDE_USER;
	for (kind = first; kind <= PPINCLUDE_AFTER; ++kind) {
		arrayforeach (&includepaths[kind], dir) {
			if (snprintf(path, sizeof path, "%s/%s", *dir, name) >= (int)sizeof path)
				continue;
			if ((f = fopen(path, "r"))) {
				fclose(f);
				if (system) *system = kind >= PPINCLUDE_SYSTEM;
				copy = xmalloc(strlen(path) + 1);
				return strcpy(copy, path);
			}
		}
	}
	return NULL;
}

void
ppadddependency(const char *path, bool system)
{
	struct dependency *d;

	arrayforeach (&dependencies, d)
		if (strcmp(d->path, path) == 0) {
			d->system &= system;
			return;
		}
	d = arrayadd(&dependencies, sizeof *d);
	d->path = xmalloc(strlen(path) + 1);
	strcpy(d->path, path);
	d->system = system;
}

static void
depprint(FILE *f, const char *s)
{
	for (; *s; ++s) {
		if (*s == ' ' || *s == '\t' || *s == '#' || *s == '\\')
			fputc('\\', f);
		if (*s == '$')
			fputc('$', f);
		fputc(*s, f);
	}
}

void
ppwritedependencies(FILE *f, const char *primary, char **targets, size_t ntarget,
	bool omit_system, bool phony)
{
	struct dependency *d;
	size_t i;

	if (ntarget) {
		for (i = 0; i < ntarget; ++i) {
			if (i) fputc(' ', f);
			depprint(f, targets[i]);
		}
	} else {
		depprint(f, primary);
	}
	fputs(": ", f);
	depprint(f, primary);
	arrayforeach (&dependencies, d) {
		if (omit_system && d->system) continue;
		fputc(' ', f);
		depprint(f, d->path);
	}
	fputc('\n', f);
	if (phony) arrayforeach (&dependencies, d) {
		if (omit_system && d->system) continue;
		depprint(f, d->path);
		fputs(":\n", f);
	}
}

void
ppinit(void)
{
	next();
}

static void
macrodel(struct macro *m)
{
	free(m->param);
	free(m->token);
	free(m->paste.val);
	free(m);
}

/* check if two macro definitions are equal, as in C11 6.10.3p2 */
static bool
macroequal(struct macro *m1, struct macro *m2)
{
	struct macroparam *p1, *p2;
	struct token *t1, *t2;

	if (m1->kind != m2->kind)
		return false;
	if (m1->kind == MACROFUNC) {
		if (m1->nparam != m2->nparam)
			return false;
		for (p1 = m1->param, p2 = m2->param; p1 < m1->param + m1->nparam; ++p1, ++p2) {
			if (strcmp(p1->name, p2->name) != 0 || p1->flags != p2->flags)
				return false;
		}
	}
	if (m1->ntoken != m2->ntoken)
		return false;
	for (t1 = m1->token, t2 = m2->token; t1 < m1->token + m1->ntoken; ++t1, ++t2) {
		if (t1->kind != t2->kind)
			return false;
		if (t1->lit && strcmp(t1->lit, t2->lit) != 0)
			return false;
	}
	return true;
}

/* find the index of a macro parameter with the given name */
static size_t
macroparam(struct macro *m, struct token *t)
{
	size_t i;

	if (t->kind >= TPPIDENT) {
		for (i = 0; i < m->nparam; ++i) {
			if (strcmp(m->param[i].name, tokenstr(t->kind)) == 0)
				return i;
		}
	}
	return -1;
}

/* lookup a macro by name */
static struct macro *
macroget(enum tokenkind ident)
{
	return arraygetptr(&macros, ident - TPPIDENT);
}

static void
macrodone(struct macro *m)
{
	m->hide = false;
	if (m->kind == MACROFUNC && m->nparam > 0) {
		free(m->arg[0].token);
		free(m->arg[0].rawtoken);
		free(m->arg);
	}
	--macrodepth;
}

static bool
macrovarargs(struct macro *m)
{
	return m->kind == MACROFUNC && m->nparam > 0 && m->param[m->nparam - 1].flags & PARAMVAR;
}

static struct token *
framenext(struct frame *f)
{
	return f->ntoken--, f->token++;
}

/* push a new context frame */
static struct frame *
ctxpush(struct token *t, size_t n, struct macro *m, bool space)
{
	struct frame *f;

	f = arrayadd(&ctx, sizeof(*f));
	f->token = t;
	f->ntoken = n;
	f->macro = m;
	f->owner = m != NULL;
	if (n > 0)
		t[0].space = space;
	return f;
}

/* get the next token from the context */
static struct token *
ctxnext(void)
{
	struct frame *f;
	struct token *t;
	struct macro *m;
	bool space;
	size_t i;

again:
	for (f = arraylast(&ctx, sizeof(*f)); ctx.len; --f, ctx.len -= sizeof(*f)) {
		if (f->ntoken)
			break;
		if (f->macro && f->owner)
			macrodone(f->macro);
	}
	if (ctx.len == 0)
		return NULL;
	m = f->macro;
	if (m && m->kind == MACROFUNC) {
		if (f->token->kind >= TPPIDENT && strcmp(tokenstr(f->token->kind), "__VA_OPT__") == 0) {
			struct frame *nested;
			struct token *start;
			size_t n = 0, depth = 1;
			space = f->token->space;
			if (!macrovarargs(m))
				error(&f->token->loc, "__VA_OPT__ requires a variadic macro");
			framenext(f);
			if (!f->ntoken || f->token->kind != TLPAREN)
				error(&tok.loc, "__VA_OPT__ requires '('");
			framenext(f);
			start = f->token;
			while (f->ntoken && depth) {
				if (f->token->kind == TLPAREN) ++depth;
				else if (f->token->kind == TRPAREN) --depth;
				if (depth) ++n;
				framenext(f);
			}
			if (depth)
				error(&tok.loc, "unterminated __VA_OPT__");
			if (m->arg[m->nparam-1].nraw == 0)
				goto again;
			nested = ctxpush(start, n, NULL, space);
			nested->macro = m;
			nested->owner = false;
			f = nested;
		}
		if (f->ntoken >= 3 && f->token[1].kind == THASHHASH) {
			static struct array spelling;
			struct token *part, *result;
			struct token *leading = NULL;
			size_t nleading = 0;
			bool resultspace = false;
			struct token *trailing = NULL;
			size_t ntrailing = 0;
			const char *s;
			spelling.len = 0;
			space = f->token->space;
			for (;;) {
				part = framenext(f);
				i = macroparam(m, part);
				if (i != (size_t)-1) {
					if (m->arg[i].nraw == 0)
						goto pastepartdone;
					if (m->arg[i].nraw > 1) {
						if (spelling.len == 0) {
							leading = m->arg[i].rawtoken;
							nleading = m->arg[i].nraw - 1;
							part = m->arg[i].rawtoken + nleading;
							resultspace = part->space;
						} else {
							if (trailing)
								error(&part->loc, "ambiguous multi-token paste argument");
							trailing = m->arg[i].rawtoken + 1;
							ntrailing = m->arg[i].nraw - 1;
							part = m->arg[i].rawtoken;
						}
					} else {
						part = m->arg[i].rawtoken;
					}
				}
				s = part->lit ? part->lit : tokenstr(part->kind);
				arrayaddbuf(&spelling, s, strlen(s));
			pastepartdone:
				if (!f->ntoken || f->token->kind != THASHHASH)
					break;
				framenext(f);
				if (!f->ntoken)
					error(&part->loc, "'##' cannot end a macro replacement list");
			}
			if (spelling.len == 0)
				goto again;
			if (!scanvalidpp(spelling.val, spelling.len))
				error(&part->loc, "token paste does not form one preprocessing token");
			result = arrayadd(&m->paste, sizeof *result);
			*result = (struct token){
				.kind = tokenget(spelling.val, spelling.len),
				.space = space,
				.loc = part->loc,
			};
			s = tokenstr(result->kind);
			/* Interned identifiers cover pasted identifiers, keywords and
			 * punctuators.  Literal tokens also need their lexical kind and
			 * spelling restored because consumers do not interpret a generic
			 * preprocessing identifier as a literal. */
			if (s[0] == '"'
			|| ((s[0] == 'L' || s[0] == 'U') && s[1] == '"')
			|| (s[0] == 'u' && (s[1] == '"' || (s[1] == '8' && s[2] == '"')))) {
				result->kind = TSTRINGLIT;
				result->lit = (char *)s;
			} else if (s[0] == '\''
			|| ((s[0] == 'L' || s[0] == 'U' || s[0] == 'u') && s[1] == '\'')) {
				result->kind = TCHARCONST;
				result->lit = (char *)s;
			} else if (isdigit((unsigned char)s[0])
			|| (s[0] == '.' && isdigit((unsigned char)s[1]))) {
				result->kind = TNUMBER;
				result->lit = (char *)s;
			}
			if (trailing)
				ctxpush(trailing, ntrailing, NULL, false);
			if (leading) {
				ctxpush(result, 1, NULL, resultspace);
				ctxpush(leading, nleading, NULL, space);
				goto again;
			}
			return result;
		}
		/* try to expand macro parameter */
		space = f->token->space;
		switch (f->token->kind) {
		case THASH:
			framenext(f);
			t = framenext(f);
			assert(t);
			i = macroparam(m, t);
			assert(i != -1);
			f = ctxpush(&m->arg[i].str, 1, NULL, space);
			break;
		default:
			if (f->token->kind >= TPPIDENT) {
				i = macroparam(m, f->token);
				if (i == -1)
					break;
				framenext(f);
				if (m->arg[i].ntoken == 0)
					goto again;
				f = ctxpush(m->arg[i].token, m->arg[i].ntoken, NULL, space);
			}
			break;
		}
		/* XXX: token concatenation */
	}
	return framenext(f);
}

static void
define(void)
{
	struct token *t;
	enum tokenkind ident, prev;
	struct macro *m, *other;
	struct macroparam *p;
	struct array params = {0}, repl = {0};
	size_t i;

	m = xmalloc(sizeof(*m));
	m->paste = (struct array){0};
	ident = tok.kind;
	m->name = tokencheck(&tok, TPPIDENT, "after #define");
	m->hide = false;
	t = arrayadd(&repl, sizeof(*t));
	scan(t);
	if (t->kind == TLPAREN && !t->space) {
		m->kind = MACROFUNC;
		/* read macro parameter names */
		p = NULL;
		while (scan(&tok), tok.kind != TRPAREN) {
			if (p) {
				if (p->flags & PARAMVAR)
					tokencheck(&tok, TRPAREN, "after '...'");
				tokencheck(&tok, TCOMMA, "or ')' after macro parameter");
				scan(&tok);
			}
			p = arrayadd(&params, sizeof(*p));
			p->flags = 0;
			if (tok.kind == TELLIPSIS) {
				p->name = "__VA_ARGS__";
				p->flags |= PARAMVAR;
			} else {
				p->name = tokencheck(&tok, TPPIDENT, "of macro parameter name or '...'");
			}
		}
		scan(t);  /* first token in replacement list */
	} else {
		m->kind = MACROOBJ;
	}
	m->param = params.val;
	m->nparam = params.len / sizeof(m->param[0]);

	/* read macro body */
	i = macroparam(m, t);
	while (t->kind != TNEWLINE && t->kind != TEOF) {
		prev = t->kind;
		t = arrayadd(&repl, sizeof(*t));
		scan(t);
		if (t->kind == T__VA_ARGS__ && !macrovarargs(m))
			error(&t->loc, "__VA_ARGS__ can only be used in variadic function-like macros");
		if (m->kind != MACROFUNC)
			continue;
		if (i != -1)
			m->param[i].flags |= PARAMTOK;
		i = macroparam(m, t);
		if (prev == THASH) {
			tokencheck(t, TPPIDENT, "after '#' operator");
			if (i == -1)
				error(&t->loc, "'%s' is not a macro parameter name", t->lit);
			m->param[i].flags |= PARAMSTR;
			i = -1;
		}
	}
	m->token = repl.val;
	m->ntoken = repl.len / sizeof(*t) - 1;
	tok = *t;

	i = ident - TPPIDENT;
	other = arraysetptr(&macros, i, m);
	if (other) {
		if (!macroequal(m, other))
			error(&tok.loc, "redefinition of macro '%s'", m->name);
		macrodel(other);
	}
}

static void
undef(void)
{
	enum tokenkind ident;
	struct macro *m;

	ident = tok.kind;
	tokencheck(&tok, TPPIDENT, "after #undef");
	m = macroget(ident);
	if (m) {
		macrodel(m);
		((void **)macros.val)[ident - TPPIDENT] = NULL;
	}
	scan(&tok);
}

struct ppexpr { struct token *t; size_t n, i; };

static int64_t ppexprcond(struct ppexpr *);
static bool ppexprunsigned;
static bool ppevaluate;
static int64_t ppvalue(int64_t value, bool isunsigned) { ppexprunsigned = isunsigned; return value; }
static struct token *rawnext(void);
static bool expand(struct token *);

static int64_t
ppheaderquery(struct ppexpr *e, const struct token *op, bool embed)
{
	char name[PATH_MAX], *p = name, *path;
	bool quote = false;
	struct stat st;

	if (e->i >= e->n || e->t[e->i++].kind != TLPAREN)
		error(&op->loc, "%s requires a parenthesized header name", tokenstr(op->kind));
	if (e->i < e->n && e->t[e->i].kind == TSTRINGLIT) {
		struct token *h = &e->t[e->i++];
		size_t n = strlen(h->lit);
		if (n < 2 || h->lit[0] != '"' || h->lit[n-1] != '"' || n - 1 >= sizeof name)
			error(&h->loc, "invalid quoted header name");
		memcpy(name, h->lit + 1, n - 2);
		name[n - 2] = '\0';
		quote = true;
	} else if (e->i < e->n && e->t[e->i++].kind == TLESS) {
		while (e->i < e->n && e->t[e->i].kind != TGREATER) {
			struct token *h = &e->t[e->i++];
			const char *s = h->lit ? h->lit : tokenstr(h->kind);
			size_t n = strlen(s);
			if ((size_t)(p - name) + n + 1 > sizeof name)
				error(&h->loc, "header name is too long");
			memcpy(p, s, n);
			p += n;
		}
		if (e->i >= e->n || e->t[e->i++].kind != TGREATER)
			error(&op->loc, "unterminated angle header name");
		*p = '\0';
	} else {
		error(&op->loc, "%s requires a header name", tokenstr(op->kind));
	}
	if (e->i >= e->n || e->t[e->i++].kind != TRPAREN)
		error(&op->loc, "missing ')' after %s", tokenstr(op->kind));
	path = includeopen(name, &op->loc, quote, NULL);
	if (!path)
		return 0;
	if (!embed) {
		free(path);
		return 1;
	}
	if (stat(path, &st) != 0)
		fatal("stat %s:", path);
	free(path);
	return st.st_size ? 1 : 2;
}

static int64_t
ppattributequery(struct ppexpr *e, const struct token *op)
{
	const char *name;

	if (e->i >= e->n || e->t[e->i++].kind != TLPAREN)
		error(&op->loc, "__has_c_attribute requires '('");
	if (e->i >= e->n || e->t[e->i].kind < TPPIDENT)
		error(&op->loc, "__has_c_attribute requires an attribute token");
	name = tokenstr(e->t[e->i++].kind);
	if (e->i < e->n && e->t[e->i].kind == TCOLONCOLON) {
		++e->i;
		if (e->i >= e->n || e->t[e->i].kind < TPPIDENT)
			error(&op->loc, "missing attribute after namespace");
		name = tokenstr(e->t[e->i++].kind);
	}
	if (e->i >= e->n || e->t[e->i++].kind != TRPAREN)
		error(&op->loc, "missing ')' after __has_c_attribute");
	if (strcmp(name, "deprecated") == 0) return 201904;
	if (strcmp(name, "fallthrough") == 0) return 201910;
	if (strcmp(name, "maybe_unused") == 0) return 202106;
	if (strcmp(name, "nodiscard") == 0) return 202003;
	if (strcmp(name, "noreturn") == 0) return 202202;
	if (strcmp(name, "reproducible") == 0 || strcmp(name, "unsequenced") == 0)
		return 202207;
	return 0;
}

static int64_t
ppgnuattributequery(struct ppexpr *e, const struct token *op)
{
	const char *name;
	char normalized[64];
	size_t length;

	if (e->i >= e->n || e->t[e->i++].kind != TLPAREN)
		error(&op->loc, "__has_attribute requires '('");
	if (e->i >= e->n || e->t[e->i].kind < TPPIDENT)
		error(&op->loc, "__has_attribute requires an attribute token");
	name = tokenstr(e->t[e->i++].kind);
	if (e->i >= e->n || e->t[e->i++].kind != TRPAREN)
		error(&op->loc, "missing ')' after __has_attribute");
	length = strlen(name);
	if (length >= 4 && name[0] == '_' && name[1] == '_'
	&& name[length - 2] == '_' && name[length - 1] == '_') {
		length -= 4;
		if (length >= sizeof normalized)
			return 0;
		memcpy(normalized, name + 2, length);
		normalized[length] = '\0';
		name = normalized;
	}
	return strcmp(name, "aligned") == 0 || strcmp(name, "constructor") == 0
		|| strcmp(name, "destructor") == 0 || strcmp(name, "packed") == 0
		|| strcmp(name, "section") == 0 || strcmp(name, "weak") == 0
		|| strcmp(name, "visibility") == 0;
}

static int64_t
ppexprfunc(struct ppexpr *e, struct macro *m, const struct token *name)
{
	struct array repl = {0};
	struct token *start[64], *t;
	size_t count[64], i, depth, nargs;
	struct ppexpr sub;
	int64_t v;

	if (m->nparam > 64)
		error(&name->loc, "too many parameters in macro '%s'", m->name);
	if (e->i >= e->n || e->t[e->i++].kind != TLPAREN)
		return 0;
	nargs = 0;
	if (e->i < e->n && e->t[e->i].kind != TRPAREN) {
		for (;;) {
			if (nargs == 64)
				error(&name->loc, "too many arguments for macro '%s'", m->name);
			start[nargs] = &e->t[e->i];
			depth = 0;
			while (e->i < e->n) {
				int kind = e->t[e->i].kind;
				if (kind == TLPAREN) ++depth;
				else if (kind == TRPAREN) {
					if (depth == 0) break;
					--depth;
				} else if (kind == TCOMMA && depth == 0) break;
				++e->i;
			}
			count[nargs] = &e->t[e->i] - start[nargs];
			++nargs;
			if (e->i >= e->n || e->t[e->i].kind != TCOMMA)
				break;
			++e->i;
		}
	}
	if (e->i >= e->n || e->t[e->i++].kind != TRPAREN)
		error(&name->loc, "unterminated invocation of macro '%s'", m->name);
	if (nargs != m->nparam)
		error(&name->loc, "macro '%s' requires %zu arguments", m->name, m->nparam);
	for (t = m->token; t < m->token + m->ntoken; ++t) {
		i = macroparam(m, t);
		if (i == (size_t)-1)
			arrayaddbuf(&repl, t, sizeof *t);
		else if (count[i])
			arrayaddbuf(&repl, start[i], count[i] * sizeof *t);
	}
	m->hide = true;
	sub = (struct ppexpr){repl.val, repl.len / sizeof *t, 0};
	v = ppexprcond(&sub);
	m->hide = false;
	if (sub.i != sub.n)
		error(&name->loc, "invalid replacement for macro '%s' in #if expression", m->name);
	free(repl.val);
	return v;
}

static int64_t
ppexprprimary(struct ppexpr *e)
{
	struct token *t;
	struct macro *m;
	struct ppexpr sub;
	int64_t v;

	if (e->i >= e->n)
		error(&tok.loc, "missing operand in #if expression");
	t = &e->t[e->i++];
	if (t->kind == TLPAREN) {
		v = ppexprcond(e);
		if (e->i >= e->n || e->t[e->i++].kind != TRPAREN)
			error(&t->loc, "missing ')' in #if expression");
		return v;
	}
	if (t->kind == TNUMBER) {
		uint64_t value = strtoull(t->lit, NULL, 0);
		const char *suffix = t->lit + strlen(t->lit);
		while (suffix > t->lit && (suffix[-1] == 'u' || suffix[-1] == 'U'
		|| suffix[-1] == 'l' || suffix[-1] == 'L')) {
			if (suffix[-1] == 'u' || suffix[-1] == 'U')
				return ppvalue((int64_t)value, true);
			suffix--;
		}
		return ppvalue((int64_t)value, value > INT64_MAX);
	}
	if (t->kind == TCHARCONST) {
		const char *src = t->lit;
		uint_least32_t chr;
		bool hexoct;
		if (*src == 'L' || *src == 'U') ++src;
		else if (*src == 'u') src += src[1] == '8' ? 2 : 1;
		++src;
		src += decodechar(src, &chr, &hexoct, "character constant", &t->loc);
		if (*src != '\'')
			error(&t->loc, "character constant contains more than one character");
		return ppvalue(chr, false);
	}
	if (t->kind == TTRUE)
		return ppvalue(1, false);
	if (t->kind == TFALSE)
		return ppvalue(0, false);
	if (t->kind >= TPPIDENT && strcmp(tokenstr(t->kind), "__has_include") == 0)
		return ppvalue(ppheaderquery(e, t, false), false);
	if (t->kind >= TPPIDENT && strcmp(tokenstr(t->kind), "__has_embed") == 0)
		return ppvalue(ppheaderquery(e, t, true), false);
	if (t->kind >= TPPIDENT && strcmp(tokenstr(t->kind), "__has_c_attribute") == 0)
		return ppvalue(ppattributequery(e, t), false);
	if (t->kind >= TPPIDENT && strcmp(tokenstr(t->kind), "__has_attribute") == 0)
		return ppvalue(ppgnuattributequery(e, t), false);
	if (t->kind >= TPPIDENT && strcmp(tokenstr(t->kind), "defined") == 0) {
		bool paren = e->i < e->n && e->t[e->i].kind == TLPAREN;
		if (paren) ++e->i;
		if (e->i >= e->n || e->t[e->i].kind < TPPIDENT)
			error(&t->loc, "defined requires an identifier");
		v = macroget(e->t[e->i++].kind) != NULL;
		if (paren && (e->i >= e->n || e->t[e->i++].kind != TRPAREN))
			error(&t->loc, "missing ')' after defined");
		return ppvalue(v, false);
	}
	if (t->kind >= TPPIDENT && (m = macroget(t->kind)) && !m->hide
	&& m->kind == MACROFUNC && e->i < e->n && e->t[e->i].kind == TLPAREN)
		return ppexprfunc(e, m, t);
	if (t->kind >= TPPIDENT && (m = macroget(t->kind)) && m->kind == MACROOBJ) {
		sub = (struct ppexpr){m->token, m->ntoken, 0};
		v = ppexprcond(&sub);
		if (sub.i != sub.n)
			error(&t->loc, "invalid macro replacement in #if expression");
		return v;
	}
	if (t->kind >= TPPIDENT)
		return ppvalue(0, false);
	error(&t->loc, "invalid token in #if expression");
}

static int64_t ppexprunary(struct ppexpr *e) {
	if (e->i < e->n) {
		int k = e->t[e->i].kind;
		if (k == TLNOT || k == TBNOT || k == TSUB || k == TADD) {
			int64_t v; bool u;
			++e->i; v = ppexprunary(e); u = ppexprunsigned;
			if (k == TLNOT) return ppvalue(!v, false);
			return ppvalue(k == TBNOT ? ~v : k == TSUB ? -v : v, u);
		}
	}
	return ppexprprimary(e);
}
#define PPBIN(name,next,op1,op2,op3,body) \
static int64_t name(struct ppexpr *e) { int64_t v=next(e); bool vu=ppexprunsigned; \
	while(e->i<e->n){int k=e->t[e->i].kind;if(k!=op1&&k!=op2&&k!=op3)break; \
		++e->i;int64_t r=next(e);bool ru=ppexprunsigned;(void)ru;body;}return ppvalue(v,vu); }
static int64_t
ppexprmul(struct ppexpr *e)
{
	int64_t v = ppexprunary(e);
	bool vu = ppexprunsigned;
	while (e->i < e->n) {
		struct token *op = &e->t[e->i];
		int k = op->kind;
		int64_t r;
		bool ru, u;
		if (k != TMUL && k != TDIV && k != TMOD)
			break;
		++e->i;
		r = ppexprunary(e);
		ru = ppexprunsigned;
		u = vu || ru;
		if (k == TMUL)
			v = (int64_t)((uint64_t)v * (uint64_t)r);
		else if (!r) {
			if (ppevaluate)
				error(&op->loc, k == TDIV ? "division by zero in #if expression" : "remainder by zero in #if expression");
			v = 0;
		} else if (k == TDIV)
			if (!u && v == INT64_MIN && r == -1) {
				if (ppevaluate)
					error(&op->loc, "signed division overflow in #if expression");
				v = INT64_MIN;
			} else
				v = u ? (int64_t)((uint64_t)v / (uint64_t)r) : v / r;
		else if (!u && v == INT64_MIN && r == -1) {
			if (ppevaluate)
				error(&op->loc, "signed remainder overflow in #if expression");
			v = 0;
		} else
			v = u ? (int64_t)((uint64_t)v % (uint64_t)r) : v % r;
		vu = u;
	}
	return ppvalue(v, vu);
}
PPBIN(ppexpradd,ppexprmul,TADD,TSUB,TNONE,
	{bool u=vu||ru;v=k==TADD?(int64_t)((uint64_t)v+(uint64_t)r):(int64_t)((uint64_t)v-(uint64_t)r);vu=u;})
PPBIN(ppexprshift,ppexpradd,TSHL,TSHR,TNONE,
	{v=k==TSHL?(int64_t)((uint64_t)v<<r):vu?(int64_t)((uint64_t)v>>r):v>>r;})
PPBIN(ppexprrel,ppexprshift,TLESS,TGREATER,TLEQ,
	{bool u=vu||ru;if(k==TLESS)v=u?(uint64_t)v<(uint64_t)r:v<r;
	else if(k==TGREATER)v=u?(uint64_t)v>(uint64_t)r:v>r;
	else v=u?(uint64_t)v<=(uint64_t)r:v<=r;vu=false;})
static int64_t ppexprrel2(struct ppexpr *e){int64_t v=ppexprrel(e);bool vu=ppexprunsigned;
	while(e->i<e->n&&e->t[e->i].kind==TGEQ){++e->i;int64_t r=ppexprrel(e);bool u=vu||ppexprunsigned;
		v=u?(uint64_t)v>=(uint64_t)r:v>=r;vu=false;}return ppvalue(v,vu);}
PPBIN(ppexpraeq,ppexprrel2,TEQL,TNEQ,TNONE,{v=k==TEQL?v==r:v!=r;vu=false;})
PPBIN(ppexprand,ppexpraeq,TBAND,TNONE,TNONE,{v&=r;vu=vu||ru;})
PPBIN(ppexprxor,ppexprand,TXOR,TNONE,TNONE,{v^=r;vu=vu||ru;})
PPBIN(pprexpbor,ppexprxor,TBOR,TNONE,TNONE,{v|=r;vu=vu||ru;})
static int64_t
ppexprland(struct ppexpr *e)
{
	int64_t v = pprexpbor(e);
	bool vu = ppexprunsigned;
	while (e->i < e->n && e->t[e->i].kind == TLAND) {
		bool outer = ppevaluate;
		int64_t r;
		++e->i;
		ppevaluate = outer && v != 0;
		r = pprexpbor(e);
		ppevaluate = outer;
		v = v && r;
		vu = false;
	}
	return ppvalue(v, vu);
}

static int64_t
ppexprlor(struct ppexpr *e)
{
	int64_t v = ppexprland(e);
	bool vu = ppexprunsigned;
	while (e->i < e->n && e->t[e->i].kind == TLOR) {
		bool outer = ppevaluate;
		int64_t r;
		++e->i;
		ppevaluate = outer && v == 0;
		r = ppexprland(e);
		ppevaluate = outer;
		v = v || r;
		vu = false;
	}
	return ppvalue(v, vu);
}
#undef PPBIN

static int64_t
ppexprcond(struct ppexpr *e)
{
	int64_t condition, left, right;
	bool leftunsigned, rightunsigned;

	condition = ppexprlor(e);
	if (e->i >= e->n || e->t[e->i].kind != TQUESTION)
		return condition;
	++e->i;
	{
		bool outer = ppevaluate;
		ppevaluate = outer && condition != 0;
		left = ppexprcond(e);
		ppevaluate = outer;
	}
	leftunsigned = ppexprunsigned;
	if (e->i >= e->n || e->t[e->i++].kind != TCOLON)
		error(&e->t[e->i - 1].loc, "missing ':' in #if conditional expression");
	{
		bool outer = ppevaluate;
		ppevaluate = outer && condition == 0;
		right = ppexprcond(e);
		ppevaluate = outer;
	}
	rightunsigned = ppexprunsigned;
	return ppvalue(condition ? left : right, leftunsigned || rightunsigned);
}

static bool
readifexpr(bool evaluate)
{
	struct array tokens = {0}, stream = {0}, expanded = {0};
	struct token *t;
	struct ppexpr e;
	int64_t value;

	for (scan(&tok); tok.kind != TNEWLINE && tok.kind != TEOF; scan(&tok)) {
		t = arrayadd(&tokens, sizeof *t);
		*t = tok;
	}
	if (!evaluate) {
		free(tokens.val);
		return false;
	}
	arrayaddbuf(&stream, tokens.val, tokens.len);
	t = arrayadd(&stream, sizeof *t);
	*t = (struct token){.kind = TEOF};
	ctxpush(stream.val, stream.len / sizeof *t, NULL, false);
	for (;;) {
		t = rawnext();
		if (t->kind == TEOF)
			break;
		if (t->kind >= TPPIDENT && strcmp(tokenstr(t->kind), "defined") == 0) {
			arrayaddbuf(&expanded, t, sizeof *t);
			t = rawnext();
			arrayaddbuf(&expanded, t, sizeof *t);
			if (t->kind == TLPAREN) {
				t = rawnext();
				arrayaddbuf(&expanded, t, sizeof *t);
				t = rawnext();
				arrayaddbuf(&expanded, t, sizeof *t);
			}
			continue;
		}
		if (!expand(t))
			arrayaddbuf(&expanded, t, sizeof *t);
	}
	(void)ctxnext();
	e = (struct ppexpr){expanded.val, expanded.len / sizeof *t, 0};
	ppevaluate = true;
	value = ppexprcond(&e);
	if (e.i != e.n)
		error(&e.t[e.i].loc, "unsupported operator in #if expression");
	free(expanded.val);
	free(stream.val);
	free(tokens.val);
	return value != 0;
}

static void
directive(void)
{
	struct location newloc;
	enum ppflags oldflags;
	enum tokenkind kind;
	char *include = NULL;

	scan(&tok);
	if (tok.kind == TNEWLINE)
		return;  /* empty directive */
	oldflags = ppflags;
	ppflags |= PPNEWLINE;
	kind = tok.kind;
	if (!ppactive() && kind != TIF && kind != TIFDEF && kind != TIFNDEF
	&& kind != TELIF && kind != TELIFDEF && kind != TELIFNDEF
	&& kind != TELSE && kind != TENDIF) {
		while (tok.kind != TNEWLINE && tok.kind != TEOF)
			scan(&tok);
		goto done;
	}
	switch (kind) {
	case TNUMBER:
		goto line;  /* gcc line markers */
	case TIF:
	{
		struct conditional *c;
		bool parent = true;
		if (conditionals.len) {
			c = arraylast(&conditionals, sizeof *c);
			parent = c->active;
		}
		bool value = readifexpr(parent);
		c = arrayadd(&conditionals, sizeof *c);
		c->parent = parent;
		c->active = c->parent && value;
		c->seen = value;
		c->hadelse = false;
		break;
	}
	case TIFDEF:
	case TIFNDEF:
	{
		struct conditional *c;
		bool value;
		scan(&tok);
		tokencheck(&tok, TPPIDENT, "after conditional directive");
		value = macroget(tok.kind) != NULL;
		if (kind == TIFNDEF)
			value = !value;
		c = arrayadd(&conditionals, sizeof *c);
		c->parent = conditionals.len == sizeof *c
			? true : (c-1)->active;
		c->active = c->parent && value;
		c->seen = value;
		c->hadelse = false;
		scan(&tok);
		break;
	}
	case TELIF:
	{
		struct conditional *c;
		bool value;
		if (conditionals.len <= conditionalbase())
			error(&tok.loc, "#elif without matching #if");
		c = arraylast(&conditionals, sizeof *c);
		if (c->hadelse)
			error(&tok.loc, "#elif after #else");
		value = readifexpr(c->parent && !c->seen);
		c->active = c->parent && !c->seen && value;
		c->seen |= value;
		break;
	}
	case TELIFDEF:
	case TELIFNDEF:
	{
		struct conditional *c;
		bool value;
		if (conditionals.len <= conditionalbase())
			error(&tok.loc, "conditional elif without matching #if");
		c = arraylast(&conditionals, sizeof *c);
		if (c->hadelse)
			error(&tok.loc, "conditional elif after #else");
		scan(&tok);
		tokencheck(&tok, TPPIDENT, "after conditional elif directive");
		value = macroget(tok.kind) != NULL;
		if (kind == TELIFNDEF) value = !value;
		c->active = c->parent && !c->seen && value;
		c->seen |= value;
		scan(&tok);
		break;
	}
	case TENDIF:
		if (conditionals.len <= conditionalbase())
			error(&tok.loc, "#endif without matching #if");
		conditionals.len -= sizeof(struct conditional);
		scan(&tok);
		break;
	case TELSE:
	{
		struct conditional *c;
		if (conditionals.len <= conditionalbase())
			error(&tok.loc, "#else without matching #if");
		c = arraylast(&conditionals, sizeof *c);
		if (c->hadelse)
			error(&tok.loc, "duplicate #else");
		c->active = c->parent && !c->seen;
		c->seen = true;
		c->hadelse = true;
		scan(&tok);
		break;
	}
	case TINCLUDE:
	{
		struct location loc = tok.loc;
		char name[PATH_MAX], *p = name;
		bool quote = false;
		struct array stream = {0}, expanded = {0};
		struct token *item;
		size_t i = 0;

		scan(&tok);
		while (tok.kind != TNEWLINE && tok.kind != TEOF) {
			arrayaddbuf(&stream, &tok, sizeof tok);
			scan(&tok);
		}
		item = arrayadd(&stream, sizeof *item);
		*item = (struct token){.kind = TEOF};
		ctxpush(stream.val, stream.len / sizeof *item, NULL, false);
		for (;;) {
			item = rawnext();
			if (item->kind == TEOF)
				break;
			if (!expand(item))
				arrayaddbuf(&expanded, item, sizeof *item);
		}
		(void)ctxnext();
		item = expanded.val;
		if (i < expanded.len / sizeof *item && item[i].kind == TSTRINGLIT) {
			size_t n = strlen(item[i].lit);
			if (n < 2 || item[i].lit[0] != '"' || item[i].lit[n-1] != '"' || n-1 >= sizeof name)
				error(&item[i].loc, "invalid quoted header name");
			memcpy(name, item[i].lit + 1, n - 2);
			name[n-2] = '\0';
			quote = true;
			++i;
		} else if (i < expanded.len / sizeof *item && item[i].kind == TLESS) {
			++i;
			while (i < expanded.len / sizeof *item && item[i].kind != TGREATER) {
				const char *s = item[i].lit ? item[i].lit : tokenstr(item[i].kind);
				size_t n = strlen(s);
				if ((size_t)(p-name) + n + 1 > sizeof name)
					error(&item[i].loc, "header name is too long");
				memcpy(p, s, n); p += n;
				++i;
			}
			if (i >= expanded.len / sizeof *item)
				error(&loc, "unterminated angle header name");
			*p = '\0';
			++i;
		} else {
			error(&loc, "expected header name after #include");
		}
		if (i != expanded.len / sizeof *item)
			error(&item[i].loc, "extra tokens after #include header name");
		free(expanded.val);
		free(stream.val);
		{
			bool system;
			include = includeopen(name, &loc, quote, &system);
			if (include) ppadddependency(include, system);
		}
		if (!include)
			error(&loc, "header '%s' not found", name);
		break;
	}
	case TEMBED:
	{
		struct location loc = tok.loc;
		char name[PATH_MAX], *p = name, *path;
		bool quote = false, have_limit = false;
		uint64_t limit = UINT64_MAX, count = 0;
		struct array stream = {0}, expanded = {0};
		struct array prefix = {0}, suffix = {0}, empty = {0}, output = {0};
		struct token *item;
		size_t i = 0, nitem;
		FILE *f;
		int byte;

		scan(&tok);
		while (tok.kind != TNEWLINE && tok.kind != TEOF) {
			arrayaddbuf(&stream, &tok, sizeof tok);
			scan(&tok);
		}
		item = arrayadd(&stream, sizeof *item);
		*item = (struct token){.kind = TEOF};
		ctxpush(stream.val, stream.len / sizeof *item, NULL, false);
		for (;;) {
			item = rawnext();
			if (item->kind == TEOF)
				break;
			if (!expand(item))
				arrayaddbuf(&expanded, item, sizeof *item);
		}
		(void)ctxnext();
		item = expanded.val;
		nitem = expanded.len / sizeof *item;
		if (i < nitem && item[i].kind == TSTRINGLIT) {
			size_t n = strlen(item[i].lit);
			if (n < 2 || item[i].lit[0] != '"' || item[i].lit[n-1] != '"' || n - 1 >= sizeof name)
				error(&item[i].loc, "invalid quoted resource name");
			memcpy(name, item[i].lit + 1, n - 2);
			name[n - 2] = '\0';
			quote = true;
			++i;
		} else if (i < nitem && item[i].kind == TLESS) {
			++i;
			while (i < nitem && item[i].kind != TGREATER) {
				const char *s = item[i].lit ? item[i].lit : tokenstr(item[i].kind);
				size_t n = strlen(s);
				if ((size_t)(p - name) + n + 1 > sizeof name)
					error(&item[i].loc, "resource name is too long");
				memcpy(p, s, n);
				p += n;
				++i;
			}
			if (i >= nitem)
				error(&loc, "unterminated angle resource name");
			*p = '\0';
			++i;
		} else {
			error(&loc, "expected resource name after #embed");
		}
		while (i < nitem) {
			const char *param;
			struct array *dst;
			unsigned depth = 1;
			if (item[i].kind < TPPIDENT)
				error(&item[i].loc, "expected #embed parameter");
			param = tokenstr(item[i++].kind);
			if (strcmp(param, "limit") == 0) dst = NULL;
			else if (strcmp(param, "prefix") == 0) dst = &prefix;
			else if (strcmp(param, "suffix") == 0) dst = &suffix;
			else if (strcmp(param, "if_empty") == 0) dst = &empty;
			else error(&item[i-1].loc, "unknown #embed parameter '%s'", param);
			if (i >= nitem || item[i++].kind != TLPAREN)
				error(&loc, "expected '(' after #embed parameter");
			{
				struct array value = {0};
				while (depth && i < nitem) {
					if (item[i].kind == TLPAREN) ++depth;
					else if (item[i].kind == TRPAREN && --depth == 0) { ++i; break; }
					arrayaddbuf(&value, &item[i++], sizeof *item);
				}
				if (depth)
					error(&loc, "unterminated #embed parameter");
				if (!dst) {
					struct ppexpr e = {value.val, value.len / sizeof(struct token), 0};
					int64_t n;
					if (have_limit) error(&loc, "duplicate #embed limit parameter");
					n = ppexprcond(&e);
					if (e.i != e.n || n < 0) error(&loc, "invalid #embed limit");
					limit = n;
					have_limit = true;
					free(value.val);
				} else {
					if (dst->val) error(&loc, "duplicate #embed parameter '%s'", param);
					*dst = value;
				}
			}
		}
		free(expanded.val);
		free(stream.val);
		{
			bool system;
			path = includeopen(name, &loc, quote, &system);
			if (path) ppadddependency(path, system);
		}
		if (!path)
			error(&loc, "resource '%s' not found", name);
		f = fopen(path, "rb");
		if (!f) fatal("open %s:", path);
		while (count < limit && (byte = fgetc(f)) != EOF) {
			struct token number = {0};
			char spelling[4];
			if (count) {
				struct token comma = {.kind = TCOMMA, .loc = loc};
				arrayaddbuf(&output, &comma, sizeof comma);
			}
			snprintf(spelling, sizeof spelling, "%u", (unsigned)(unsigned char)byte);
			number.kind = TNUMBER;
			number.loc = loc;
			number.lit = xmalloc(strlen(spelling) + 1);
			strcpy(number.lit, spelling);
			arrayaddbuf(&output, &number, sizeof number);
			++count;
		}
		if (ferror(f)) fatal("read %s:", path);
		fclose(f);
		free(path);
		if (count) {
			if (prefix.len) {
				struct array joined = {0};
				arrayaddbuf(&joined, prefix.val, prefix.len);
				arrayaddbuf(&joined, output.val, output.len);
				free(output.val); output = joined;
			}
			arrayaddbuf(&output, suffix.val, suffix.len);
		} else if (empty.len) {
			free(output.val); output = empty; empty.val = NULL;
		}
		if (output.len)
			ctxpush(output.val, output.len / sizeof(struct token), NULL, false);
		break;
	}
	case TDEFINE:
		scan(&tok);
		define();
		break;
	case TUNDEF:
		scan(&tok);
		undef();
		break;
	case TLINE:
		scan(&tok);
		tokencheck(&tok, TNUMBER, "after #line");
	line:
		newloc.line = strtoull(tok.lit, NULL, 0);
		newloc.col = 1;
		scan(&tok);
		newloc.file = tok.loc.file;
		if (tok.kind == TSTRINGLIT) {
			char *quote;

			/* XXX: handle escape sequences (reuse string decoding from expr.c) */
			quote = strchr(tok.lit, '"');
			assert(quote);
			newloc.file = quote + 1;
			quote = strchr(quote + 1, '"');
			assert(quote);
			*quote = '\0';
			scan(&tok);
		}
		while (tok.kind == TNUMBER)
			scan(&tok);
		scansetloc(newloc);
		break;
	case TERROR:
	case TWARNING:
		diagloc(&tok.loc);
		fprintf(stderr, "%s directive:", kind == TERROR ? "#error" : "#warning");
		scan(&tok);
		while (tok.kind != TNEWLINE && tok.kind != TEOF) {
			tokenprint(&tok, stderr);
			scan(&tok);
		}
		fputc('\n', stderr);
		if (kind == TERROR || ppwarningsareerrors)
			exit(1);
		break;
	case TPRAGMA:
		while (tok.kind != TNEWLINE && tok.kind != TEOF)
			next();
		break;
	default:
		error(&tok.loc, "invalid preprocessor directive #%s", tokenstr(tok.kind));
	}
done:
	tokencheck(&tok, TNEWLINE, "after preprocessing directive");
	ppflags = oldflags;
	if (include) {
		size_t depth = conditionals.len;
		size_t *active = arrayadd(&includeconditionals, sizeof *active);
		*active = depth;
		scanfrominclude(include, NULL, depth);
		scanopen();
	}
}

/* get the next token without expanding it */
static void
nextinto(struct token *t)
{
	static bool newline = true;

	for (;;) {
		size_t ended;
		scan(t);
		while (scanincludeend(&ended)) {
			size_t *depth = arraylast(&includeconditionals, sizeof *depth);
			if (!depth || *depth != ended || conditionals.len != ended)
				error(&t->loc, "unterminated conditional directive in included file");
			includeconditionals.len -= sizeof *depth;
		}
		if (t->kind == TEOF && conditionals.len)
			error(&t->loc, "unterminated conditional directive");
		if (newline && t->kind == THASH) {
			directive();
			{
				struct token *out = ctxnext();
				if (out) {
					*t = *out;
				newline = false;
				break;
				}
			}
		} else if (!ppactive()) {
			newline = t->kind == TNEWLINE;
			continue;
		} else {
			newline = tok.kind == TNEWLINE;
			break;
		}
	}
}

static struct token *
rawnext(void)
{
	struct token *t;

	t = ctxnext();
	if (!t) {
		t = &tok;
		nextinto(t);
	}
	return t;
}

static bool
peekparen(void)
{
	static struct array pending;
	struct token *t;
	struct frame *f;

	t = ctxnext();
	if (t) {
		if (t->kind == TLPAREN)
			return true;
		f = arraylast(&ctx, sizeof(*f));
		--f->token;
		++f->ntoken;
		return false;
	}
	pending.len = 0;
	do t = arrayadd(&pending, sizeof(*t)), nextinto(t);
	while (t->kind == TNEWLINE);
	if (t->kind == TLPAREN)
		return true;
	t = pending.val;
	ctxpush(t, pending.len / sizeof(*t), NULL, t[0].space);
	return false;
}

static void
stringize(struct array *buf, struct token *t)
{
	const char *lit;

	if ((t->space || t->kind == TNEWLINE) && buf->len > 1 && ((char *)buf->val)[buf->len - 1] != ' ')
		arrayaddbuf(buf, " ", 1);
	lit = t->lit ? t->lit : tokenstr(t->kind);
	if (t->kind == TSTRINGLIT || t->kind == TCHARCONST) {
		for (; *lit; ++lit) {
			if (*lit == '\\' || *lit == '"')
				arrayaddbuf(buf, "\\", 1);
			arrayaddbuf(buf, lit, 1);
		}
	} else if (lit) {
		arrayaddbuf(buf, lit, strlen(lit));
	}
}

static void expandfunc(struct macro *);

static bool
expand(struct token *t)
{
	struct macro *m;
	struct token *builtin;
	bool space;

	if (t->kind < TPPIDENT)
		return false;
	if (strcmp(tokenstr(t->kind), "__LINE__") == 0) {
		builtin = xmalloc(sizeof *builtin);
		*builtin = *t;
		builtin->kind = TNUMBER;
		builtin->lit = xmalloc(32);
		snprintf(builtin->lit, 32, "%zu", t->loc.line);
		ctxpush(builtin, 1, NULL, t->space);
		return true;
	}
	if (strcmp(tokenstr(t->kind), "__FILE__") == 0) {
		size_t n = strlen(t->loc.file);
		builtin = xmalloc(sizeof *builtin);
		*builtin = *t;
		builtin->kind = TSTRINGLIT;
		builtin->lit = xmalloc(n + 3);
		builtin->lit[0] = '"';
		memcpy(builtin->lit + 1, t->loc.file, n);
		builtin->lit[n+1] = '"'; builtin->lit[n+2] = '\0';
		ctxpush(builtin, 1, NULL, t->space);
		return true;
	}
	if (strcmp(tokenstr(t->kind), "_Pragma") == 0) {
		struct token *arg;
		arg = rawnext();
		if (arg->kind != TLPAREN)
			error(&arg->loc, "_Pragma requires '('");
		arg = rawnext();
		if (arg->kind != TSTRINGLIT)
			error(&arg->loc, "_Pragma requires a string literal");
		arg = rawnext();
		if (arg->kind != TRPAREN)
			error(&arg->loc, "_Pragma requires ')'");
		return true;
	}
	m = macroget(t->kind);
	if (!m)
		return false;
	if (m->hide)
		t->hide = true;
	if (t->hide)
		return false;
	space = t->space;
	if (m->kind == MACROFUNC) {
		if (!peekparen())
			return false;
		expandfunc(m);
	}
	ctxpush(m->token, m->ntoken, m, space);
	m->hide = true;
	++macrodepth;
	return true;
}

static void
expandfunc(struct macro *m)
{
	struct macroparam *p;
	struct macroarg *arg;
	struct array str, tok, raw;
	size_t i, depth, paren;
	struct token *t;

	/* read macro arguments */
	paren = 0;
	depth = macrodepth;
	tok = (struct array){0};
	raw = (struct array){0};
	arg = xreallocarray(NULL, m->nparam, sizeof(*arg));
	t = rawnext();
	for (i = 0; i < m->nparam; ++i) {
		p = &m->param[i];
		if (p->flags & PARAMSTR) {
			str = (struct array){0};
			arrayaddbuf(&str, "\"", 1);
		}
		arg[i].ntoken = 0;
		arg[i].nraw = 0;
		for (;;) {
			if (t->kind == TEOF)
				error(&t->loc, "EOF when reading macro parameters");
			if (macrodepth <= depth) {
				/* adjust current macro depth, in case it got shallower */
				depth = macrodepth;
				if (paren == 0 && (t->kind == TRPAREN || t->kind == TCOMMA && !(p->flags & PARAMVAR)))
					break;
				switch (t->kind) {
				case TLPAREN: ++paren; break;
				case TRPAREN: --paren; break;
				}
				if (p->flags & PARAMSTR)
					stringize(&str, t);
				if (p->flags & PARAMTOK) {
					arrayaddbuf(&raw, t, sizeof *t);
					++arg[i].nraw;
				}
			}
			if (p->flags & PARAMTOK && !expand(t)) {
				arrayaddbuf(&tok, t, sizeof(*t));
				++arg[i].ntoken;
			}
			t = rawnext();
		}
		if (p->flags & PARAMSTR) {
			arrayaddbuf(&str, "\"", 2);
			arg[i].str = (struct token){
				.kind = TSTRINGLIT,
				.lit = str.val,
			};
		}
		if (t->kind == TRPAREN)
			break;
		t = rawnext();
	}
	if (i + 1 < m->nparam) {
		if (i + 2 == m->nparam && m->param[i+1].flags & PARAMVAR) {
			++i;
			arg[i].ntoken = arg[i].nraw = 0;
		} else
			error(&t->loc, "not enough arguments for macro '%s'", m->name);
	}
	if (t->kind != TRPAREN)
		error(&t->loc, "too many arguments for macro '%s'", m->name);
	for (i = 0, t = tok.val; i < m->nparam; ++i) {
		arg[i].token = t;
		t += arg[i].ntoken;
	}
	for (i = 0, t = raw.val; i < m->nparam; ++i) {
		arg[i].rawtoken = t;
		t += arg[i].nraw;
	}
	m->arg = arg;
}

void
next(void)
{
	struct token *t;

	do t = rawnext();
	while (expand(t) || t->kind == TNEWLINE && !(ppflags & PPNEWLINE));
	tok = *t;

	/* keyword aliases */
	switch (tok.kind) {
	case ALIAS_ALIGNAS:       tok.kind = TALIGNAS;       break;
	case ALIAS_ALIGNOF:       tok.kind = TALIGNOF;       break;
	case ALIAS_BOOL:          tok.kind = TBOOL;          break;
	case ALIAS__INLINE:
	case ALIAS__INLINE__:     tok.kind = TINLINE;        break;
	case ALIAS__SIGNED:
	case ALIAS__SIGNED__:     tok.kind = TSIGNED;        break;
	case ALIAS_STATIC_ASSERT: tok.kind = TSTATIC_ASSERT; break;
	case ALIAS_THREAD_LOCAL:
	case ALIAS__THREAD:       tok.kind = TTHREAD_LOCAL;  break;
	case ALIAS__TYPEOF:
	case ALIAS__TYPEOF__:     tok.kind = TTYPEOF;        break;
	case ALIAS__VOLATILE__:   tok.kind = TVOLATILE;      break;
	case ALIAS__ASM:          tok.kind = T__ASM__;       break;
	}
}

bool
peek(enum tokenkind kind)
{
	static struct token pending;
	struct token old;

	old = tok;
	next();
	if (tok.kind == kind) {
		next();
		return true;
	}
	pending = tok;
	tok = old;
	ctxpush(&pending, 1, NULL, pending.space);
	return false;
}

char *
expect(enum tokenkind kind, const char *msg)
{
	char *lit;

	lit = tokencheck(&tok, kind, msg);
	next();

	return lit;
}

bool
consume(enum tokenkind kind)
{
	if (tok.kind != kind)
		return false;
	next();
	return true;
}
