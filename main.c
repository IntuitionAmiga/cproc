#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "util.h"
#include "arg.h"
#include "cc.h"

static void
ppoption(struct array *buf, const char *directive, const char *arg)
{
	const char *eq;
	arrayaddbuf(buf, directive, strlen(directive));
	arrayaddbuf(buf, " ", 1);
	if ((eq = strchr(arg, '='))) {
		arrayaddbuf(buf, arg, eq - arg);
		arrayaddbuf(buf, " ", 1);
		arrayaddbuf(buf, eq + 1, strlen(eq + 1));
	} else {
		arrayaddbuf(buf, arg, strlen(arg));
		if (strcmp(directive, "#define") == 0)
			arrayaddbuf(buf, " 1", 2);
	}
	arrayaddbuf(buf, "\n", 1);
}

static void
ppdatetime(struct array *buf)
{
	char date[64], clock[64], def[96], *end, *epoch = getenv("SOURCE_DATE_EPOCH");
	time_t when;
	struct tm *tm;

	if (epoch) {
		if (!*epoch || *epoch == '-' || (when = (time_t)strtoll(epoch, &end, 10), *end))
			fatal("invalid SOURCE_DATE_EPOCH");
	} else
		when = time(NULL);
	tm = epoch ? gmtime(&when) : localtime(&when);
	if (!tm || !strftime(date, sizeof date, "%b %e %Y", tm)
	|| !strftime(clock, sizeof clock, "%H:%M:%S", tm))
		fatal("cannot format preprocessor invocation time");
	snprintf(def, sizeof def, "__DATE__=\"%s\"", date);
	ppoption(buf, "#define", def);
	snprintf(def, sizeof def, "__TIME__=\"%s\"", clock);
	ppoption(buf, "#define", def);
}

static void
usage(void)
{
	fprintf(stderr, "usage: %s [input]\n", argv0);
	exit(2);
}

int
main(int argc, char *argv[])
{
	bool pponly = false, deponly = false, depomit = false, depphony = false;
	char *output = NULL, *target = NULL, *depfile = NULL, *primary = NULL;
	struct array predefines = {0};
	struct array preincludes = {0};
	struct array deptargets = {0};
	FILE *predefile;

	ppoption(&predefines, "#define", "__STDC__=1");
	ppoption(&predefines, "#define", "__STDC_VERSION__=202311L");
	ppoption(&predefines, "#define", "__STDC_HOSTED__=0");
	ppoption(&predefines, "#define", "__LP64__=1");
	ppoption(&predefines, "#define", "__STDC_EMBED_NOT_FOUND__=0");
	ppoption(&predefines, "#define", "__STDC_EMBED_FOUND__=1");
	ppoption(&predefines, "#define", "__STDC_EMBED_EMPTY__=2");
	/* Keep fallback headers from replacing the built-in #if query operator. */
	ppoption(&predefines, "#define", "__has_attribute=__has_attribute");

	argv0 = progname(argv[0], "cproc-qbe");
	ppdatetime(&predefines);
	ARGBEGIN {
	case 'E':
		pponly = true;
		break;
	case 'Z':
		pponly = deponly = true;
		break;
	case 'd':
		depfile = EARGF(usage());
		break;
	case 'k':
		depomit = true;
		break;
	case 'P':
		depphony = true;
		break;
	case 'T':
		arrayaddptr(&deptargets, EARGF(usage()));
		break;
	case 'I':
		ppaddinclude(PPINCLUDE_USER, EARGF(usage()));
		break;
	case 'Q':
		ppaddinclude(PPINCLUDE_QUOTE, EARGF(usage()));
		break;
	case 'Y':
		ppaddinclude(PPINCLUDE_SYSTEM, EARGF(usage()));
		break;
	case 'A':
		ppaddinclude(PPINCLUDE_AFTER, EARGF(usage()));
		break;
	case 'F': {
		char *path = EARGF(usage());
		char *copy = xmalloc(strlen(path) + 1);
		arrayaddptr(&preincludes, strcpy(copy, path));
		break;
	}
	case 'D':
		ppoption(&predefines, "#define", EARGF(usage()));
		break;
	case 'U':
		ppoption(&predefines, "#undef", EARGF(usage()));
		break;
	case 't':
		target = EARGF(usage());
		break;
	case 'W':
		if (strcmp(EARGF(usage()), "error") != 0)
			usage();
		ppwarningsareerrors = true;
		break;
	case 'o':
		output = EARGF(usage());
		break;
	default:
		usage();
	} ARGEND

	targinit(target);
	if (target && strcmp(target, "ie64") == 0) {
		ppoption(&predefines, "#define", "__ie64__=1");
		ppoption(&predefines, "#define", "__IE64__=1");
		ppoption(&predefines, "#define", "__ie64_baremetal__=1");
		ppoption(&predefines, "#define", "__IE64_ABI_VERSION__=3");
		ppoption(&predefines, "#define", "__ORDER_LITTLE_ENDIAN__=1234");
		ppoption(&predefines, "#define", "__ORDER_BIG_ENDIAN__=4321");
		ppoption(&predefines, "#define", "__ORDER_PDP_ENDIAN__=3412");
		ppoption(&predefines, "#define", "__BYTE_ORDER__=__ORDER_LITTLE_ENDIAN__");
		ppoption(&predefines, "#define", "__FLOAT_WORD_ORDER__=__ORDER_LITTLE_ENDIAN__");
		ppoption(&predefines, "#define", "__SIZEOF_POINTER__=8");
		ppoption(&predefines, "#define", "__SIZEOF_SHORT__=2");
		ppoption(&predefines, "#define", "__SIZEOF_INT__=4");
		ppoption(&predefines, "#define", "__SIZEOF_LONG__=8");
		ppoption(&predefines, "#define", "__SIZEOF_LONG_LONG__=8");
		ppoption(&predefines, "#define", "__SIZEOF_FLOAT__=4");
		ppoption(&predefines, "#define", "__SIZEOF_DOUBLE__=8");
		ppoption(&predefines, "#define", "__SIZEOF_LONG_DOUBLE__=8");
		ppoption(&predefines, "#define", "__SIZEOF_WCHAR_T__=4");
		ppoption(&predefines, "#define", "__SIZEOF_WINT_T__=4");
		ppoption(&predefines, "#define", "__CHAR_BIT__=8");
		ppoption(&predefines, "#define", "__SCHAR_WIDTH__=8");
		ppoption(&predefines, "#define", "__SHRT_WIDTH__=16");
		ppoption(&predefines, "#define", "__INT_WIDTH__=32");
		ppoption(&predefines, "#define", "__LONG_WIDTH__=64");
		ppoption(&predefines, "#define", "__LONG_LONG_WIDTH__=64");
		ppoption(&predefines, "#define", "__SCHAR_MAX__=127");
		ppoption(&predefines, "#define", "__SHRT_MAX__=32767");
		ppoption(&predefines, "#define", "__INT_MAX__=2147483647");
		ppoption(&predefines, "#define", "__LONG_MAX__=9223372036854775807L");
		ppoption(&predefines, "#define", "__LONG_LONG_MAX__=9223372036854775807LL");
		ppoption(&predefines, "#define", "__INT8_TYPE__=signed char");
		ppoption(&predefines, "#define", "__UINT8_TYPE__=unsigned char");
		ppoption(&predefines, "#define", "__INT16_TYPE__=short");
		ppoption(&predefines, "#define", "__UINT16_TYPE__=unsigned short");
		ppoption(&predefines, "#define", "__INT32_TYPE__=int");
		ppoption(&predefines, "#define", "__UINT32_TYPE__=unsigned int");
		ppoption(&predefines, "#define", "__INT64_TYPE__=long");
		ppoption(&predefines, "#define", "__UINT64_TYPE__=unsigned long");
		ppoption(&predefines, "#define", "__INTPTR_TYPE__=long");
		ppoption(&predefines, "#define", "__UINTPTR_TYPE__=unsigned long");
		ppoption(&predefines, "#define", "__INTPTR_MAX__=9223372036854775807L");
		ppoption(&predefines, "#define", "__UINTPTR_MAX__=18446744073709551615UL");
		ppoption(&predefines, "#define", "__INTMAX_TYPE__=long");
		ppoption(&predefines, "#define", "__UINTMAX_TYPE__=unsigned long");
		ppoption(&predefines, "#define", "__INTMAX_MAX__=9223372036854775807L");
		ppoption(&predefines, "#define", "__UINTMAX_MAX__=18446744073709551615UL");
		ppoption(&predefines, "#define", "__SIZE_TYPE__=unsigned long");
		ppoption(&predefines, "#define", "__SIZE_MAX__=18446744073709551615UL");
		ppoption(&predefines, "#define", "__PTRDIFF_TYPE__=long");
		ppoption(&predefines, "#define", "__PTRDIFF_MAX__=9223372036854775807L");
		ppoption(&predefines, "#define", "__WCHAR_TYPE__=unsigned int");
		ppoption(&predefines, "#define", "__WCHAR_MAX__=4294967295U");
		ppoption(&predefines, "#define", "__WINT_TYPE__=unsigned int");
		ppoption(&predefines, "#define", "__WINT_MAX__=4294967295U");
		ppoption(&predefines, "#define", "__FLT_MANT_DIG__=24");
		ppoption(&predefines, "#define", "__DBL_MANT_DIG__=53");
		ppoption(&predefines, "#define", "__LDBL_MANT_DIG__=53");
		ppoption(&predefines, "#define", "__FLT_MIN_EXP__=(-125)");
		ppoption(&predefines, "#define", "__DBL_MIN_EXP__=(-1021)");
		ppoption(&predefines, "#define", "__LDBL_MIN_EXP__=(-1021)");
		ppoption(&predefines, "#define", "__FLT_MAX_EXP__=128");
		ppoption(&predefines, "#define", "__DBL_MAX_EXP__=1024");
		ppoption(&predefines, "#define", "__LDBL_MAX_EXP__=1024");
		ppoption(&predefines, "#define", "__FLT_MAX_10_EXP__=38");
		ppoption(&predefines, "#define", "__DBL_MAX_10_EXP__=308");
		ppoption(&predefines, "#define", "__LDBL_MAX_10_EXP__=308");
		ppoption(&predefines, "#define", "__FLT_MIN__=1.17549435082228750797e-38F");
		ppoption(&predefines, "#define", "__DBL_MIN__=2.22507385850720138309e-308");
		ppoption(&predefines, "#define", "__LDBL_MIN__=2.22507385850720138309e-308L");
		ppoption(&predefines, "#define", "__FLT_MAX__=3.40282346638528859812e+38F");
		ppoption(&predefines, "#define", "__DBL_MAX__=1.79769313486231570815e+308");
		ppoption(&predefines, "#define", "__LDBL_MAX__=1.79769313486231570815e+308L");
		ppoption(&predefines, "#define", "__FLT_EPSILON__=1.19209289550781250000e-7F");
		ppoption(&predefines, "#define", "__DBL_EPSILON__=2.22044604925031308085e-16");
		ppoption(&predefines, "#define", "__LDBL_EPSILON__=2.22044604925031308085e-16L");
		ppoption(&predefines, "#define", "__FLT_DENORM_MIN__=1.40129846432481707092e-45F");
		ppoption(&predefines, "#define", "__DBL_DENORM_MIN__=4.94065645841246544177e-324");
		ppoption(&predefines, "#define", "__LDBL_DENORM_MIN__=4.94065645841246544177e-324L");
		ppoption(&predefines, "#define", "__FLT_DIG__=6");
		ppoption(&predefines, "#define", "__DBL_DIG__=15");
		ppoption(&predefines, "#define", "__LDBL_DIG__=15");
		ppoption(&predefines, "#define", "__DECIMAL_DIG__=17");
		ppoption(&predefines, "#define", "__FLT_DECIMAL_DIG__=9");
		ppoption(&predefines, "#define", "__DBL_DECIMAL_DIG__=17");
		ppoption(&predefines, "#define", "__LDBL_DECIMAL_DIG__=17");
		ppoption(&predefines, "#define", "__FLT_RADIX__=2");
		ppoption(&predefines, "#define", "__FLT_EVAL_METHOD__=0");
	}

	if (output && !freopen(output, "w", stdout))
		fatal("open %s:", output);

	if (argc) {
		primary = argv[0];
		while (argc--)
			scanfrom(argv[argc], NULL);
	} else {
		primary = "<stdin>";
		scanfrom("<stdin>", stdin);
	}
	if (preincludes.len) {
		char **paths = preincludes.val;
		size_t n = preincludes.len / sizeof(*paths);
		while (n--) {
			ppadddependency(paths[n], false);
			scanfrom(paths[n], NULL);
		}
	}
	predefile = tmpfile();
	if (!predefile)
		fatal("tmpfile:");
	if (fwrite(predefines.val, 1, predefines.len, predefile) != predefines.len
	|| fseek(predefile, 0, SEEK_SET) != 0)
		fatal("write command-line predefinitions:");
	scanfrom("<command-line>", predefile);
	scanopen();

	tokeninit();
	ppinit();
	if (pponly) {
		ppflags |= PPNEWLINE;
		while (tok.kind != TEOF) {
			if (!deponly) tokenprint(&tok, stdout);
			next();
		}
	} else {
		scopeinit();
		while (tok.kind != TEOF) {
			/* C23 permits an empty declaration at file scope. */
			if (consume(TSEMICOLON))
				continue;
			if (!decl(&filescope, NULL)) {
				error(&tok.loc, "expected declaration or function definition");
			}
		}
		emittentativedefns();
		emitfinish();
	}
	if (depfile) {
		FILE *depout = strcmp(depfile, "-") == 0 ? stdout : fopen(depfile, "w");
		if (!depout) fatal("open %s:", depfile);
		ppwritedependencies(depout, primary, deptargets.val,
			deptargets.len / sizeof(char *), depomit, depphony);
		if (depout != stdout && fclose(depout) != 0) fatal("close %s:", depfile);
	}

	fflush(stdout);
	if (ferror(stdout))
		fatal("write failed");
	return 0;
}
