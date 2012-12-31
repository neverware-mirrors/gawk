/*
 * mpfr.c - routines for arbitrary-precision number support in gawk.
 */

/* 
 * Copyright (C) 2012 the Free Software Foundation, Inc.
 * 
 * This file is part of GAWK, the GNU implementation of the
 * AWK Programming Language.
 * 
 * GAWK is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 * 
 * GAWK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#include "awk.h"

#ifdef HAVE_MPFR
#include <gmp.h>
#include <mpfr.h>

#ifndef MPFR_RNDN
/* for compatibility with MPFR 2.X */
#define MPFR_RNDN GMP_RNDN
#define MPFR_RNDZ GMP_RNDZ
#define MPFR_RNDU GMP_RNDU
#define MPFR_RNDD GMP_RNDD
#endif

#if !defined(MPFR_VERSION_MAJOR) || MPFR_VERSION_MAJOR < 3
typedef mp_exp_t mpfr_exp_t;
#endif

#define DEFAULT_PREC		53
#define DEFAULT_ROUNDMODE	"N"		/* round to nearest */

extern NODE **fmt_list;          /* declared in eval.c */

/* exported functions */
static NODE *mpfp_make_number(AWKNUM);
static int mpfp_compare(const NODE *, const NODE *);
static void mpfp_negate_num(NODE *);
static NODE *mpfp_str2node(char *, char **, int, bool);
static NODE *mpfp_force_number(NODE *);
static void mpfp_free_num(NODE *);
static NODE *mpfp_format_val(const char *, int, NODE *);
static unsigned long mpfp_toulong(const NODE *);
static long mpfp_tolong(const NODE *);
static AWKNUM mpfp_todouble(const NODE *);
static uintmax_t mpfp_touintmax_t(const NODE *);
static int mpfp_sgn(const NODE *);
static bool mpfp_is_integer(const NODE *n);
static NODE *mpfp_copy_number(const NODE *);
static NODE *mpfp_format_nodes(const char *, size_t, NODE **, long);
static bool mpfp_init(bltin_t **);
static NODE *mpfp_add(const NODE *, const NODE *);
static NODE *mpfp_sub(const NODE *, const NODE *);
static NODE *mpfp_mul(const NODE *, const NODE *);
static NODE *mpfp_div(const NODE *, const NODE *);
static NODE *mpfp_mod(const NODE *, const NODE *);
static NODE *mpfp_pow(const NODE *, const NODE *);
static NODE *mpfp_add_long(const NODE *, long);
static NODE *mpfp_update_var(NODE *);
static void mpfp_set_var(const NODE *);
static long mpfp_increment_var(const NODE *, long);
static void mpfp_init_vars(void);
static void mpfp_load_procinfo(void);
static const char *mpfp_version_string(void);

/* builtins */
static NODE *do_mpfp_and(int);
static NODE *do_mpfp_atan2(int);
static NODE *do_mpfp_compl(int);
static NODE *do_mpfp_cos(int);
static NODE *do_mpfp_exp(int);
static NODE *do_mpfp_int(int);
static NODE *do_mpfp_log(int);
static NODE *do_mpfp_lshift(int);
static NODE *do_mpfp_or(int);
static NODE *do_mpfp_rand(int);
static NODE *do_mpfp_rshift(int);
static NODE *do_mpfp_sin(int);
static NODE *do_mpfp_sqrt(int);
static NODE *do_mpfp_srand(int);
static NODE *do_mpfp_strtonum(int);
static NODE *do_mpfp_xor(int);


/* internal functions */
static NODE *mpfp_make_node(unsigned int type);
static int mpfp_format_ieee(mpfr_ptr, int);
static const char *mpfp_sprintf(const char *, ...);
static int mpfp_strtoui(mpz_ptr, char *, size_t, char **, int);
static mpfr_rnd_t mpfp_get_rounding_mode(const char rmode);
static mpfr_ptr mpz2mpfr(mpz_ptr mpz_val, mpfr_ptr mpfr_val);


static mpfr_rnd_t ROUND_MODE;
static mpz_t MNR;
static mpz_t MFNR;
static bool do_ieee_fmt;	/* emulate IEEE 754 floating-point format */

static mpfr_exp_t min_exp = MPFR_EMIN_DEFAULT;
static mpfr_exp_t max_exp = MPFR_EMAX_DEFAULT;

/* needed for MPFR and GMP macros */
#define MPFR_T(x)	((mpfr_ptr) x)
#define MPZ_T(x)	((mpz_ptr) x)


/* temporary MPFR floats used to hold converted GMP integer operands */
static mpfr_t _mp1;
static mpfr_t _mp2;

/*
 * PRECISION_MIN is the precision used to initialize _mp1 and _mp2.
 * 64 bits should be enough for exact conversion of most integers to floats.
 */

#define PRECISION_MIN	64

static mpz_t _mpzval;	/* GMP type for float to int conversion in format_tree() */
static mpfr_t _mpfrval;	/* MPFR type for int to float conversion in format_tree() */

#define IEEE_FMT(r, t)		(void) (do_ieee_fmt && mpfp_format_ieee(r, t))

#define mpfp_float()		mpfp_make_node(MPFN)
#define mpfp_integer()		mpfp_make_node(MPZN)
#define is_mpfp_float(n)	(((n)->flags & MPFN) != 0)
#define is_mpfp_integer(n)	(((n)->flags & MPZN) != 0)
#define is_mpfp_number(n)	(((n)->flags & (MPZN|MPFN)) != 0)


/* mpfp_tofloat --- convert GMP integer to MPFR float without loosing any precision */

static inline mpfr_ptr
mpfp_tofloat(const NODE *t, mpfr_ptr pf)
{
	return is_mpfp_float(t) ? t->qnumbr : mpz2mpfr(t->qnumbr, pf);
}


numbr_handler_t mpfp_hndlr = {
	mpfp_init,
	mpfp_version_string,
	mpfp_load_procinfo,
	mpfp_make_number,
	mpfp_str2node,
	mpfp_copy_number,
	mpfp_free_num,
	mpfp_force_number,
	mpfp_negate_num,
	mpfp_compare,
	mpfp_sgn,
	mpfp_is_integer,
	mpfp_format_val,
	mpfp_format_nodes,
	mpfp_todouble,
	mpfp_tolong,
	mpfp_toulong,
	mpfp_touintmax_t,
	mpfp_add,
	mpfp_sub,
	mpfp_mul,
	mpfp_div,
	mpfp_mod,
	mpfp_pow,
	mpfp_add_long,
	mpfp_update_var,
	mpfp_set_var,
	mpfp_increment_var,
	mpfp_init_vars,
};


/* mpfp_init --- set up MPFR related variables */

static bool
mpfp_init(bltin_t **numbr_bltins)
{
	static bltin_t mpfp_bltins[] = {
		{ "and",	do_mpfp_and },
		{ "atan2",	do_mpfp_atan2 },
		{ "compl",	do_mpfp_compl },
		{ "cos",	do_mpfp_cos },
		{ "exp",	do_mpfp_exp },
		{ "int",	do_mpfp_int },
		{ "log",	do_mpfp_log },
		{ "lshift",	do_mpfp_lshift },
		{ "or",		do_mpfp_or },
		{ "rand",	do_mpfp_rand },
		{ "rshift",	do_mpfp_rshift },
		{ "sin",	do_mpfp_sin },
		{ "sqrt",	do_mpfp_sqrt },
		{ "srand",	do_mpfp_srand },
		{ "strtonum",	do_mpfp_strtonum },
		{ "xor",	do_mpfp_xor },
		{ NULL, NULL },
	};
	const char *rndmode = DEFAULT_ROUNDMODE;

	mpfr_set_default_prec(DEFAULT_PREC);
	ROUND_MODE = mpfp_get_rounding_mode(rndmode[0]); 
	mpfr_set_default_rounding_mode(ROUND_MODE);

	mpz_init(MNR);
	mpz_init(MFNR);
	do_ieee_fmt = false;

	mpfr_init2(_mp1, PRECISION_MIN);
	mpfr_init2(_mp2, PRECISION_MIN);
	mpz_init(_mpzval);
	mpfr_init2(_mpfrval, PRECISION_MIN);

	/* set the numeric value of null string */
	emalloc(Nnull_string->qnumbr, void *, sizeof (mpz_t), "mpfp_init");
	mpz_init(Nnull_string->qnumbr);	/* initialized to 0 */
	Nnull_string->flags |= (MPZN|NUMCUR|NUMBER);

	/* initialize TRUE and FALSE nodes */
	false_node = mpfp_integer();
	true_node = mpfp_integer();
	mpz_set_si(true_node->qnumbr, 1);

	*numbr_bltins = mpfp_bltins;
	return true;
}

static void
mpfp_load_procinfo()
{
	char name[64];

	snprintf(name, 64, "GNU MPFR %s", mpfr_get_version());
	update_PROCINFO_str("mpfr_version", name);
	snprintf(name, 64, "GNU MP %s", gmp_version);
	update_PROCINFO_str("gmp_version", name);
	update_PROCINFO_num("prec_max", MPFR_PREC_MAX);
	update_PROCINFO_num("prec_min", MPFR_PREC_MIN);
}

static const char *
mpfp_version_string()
{
	static char version_string[64];
	snprintf(version_string, 64, "GNU MPFR %s, GNU MP %s", mpfr_get_version(), gmp_version);
	return version_string;
}

/* mpfp_toulong --- conversion to unsigned long */

static unsigned long
mpfp_toulong(const NODE *n)
{
	return (n->flags & MPFN) != 0 ? mpfr_get_ui(n->qnumbr, ROUND_MODE) : mpz_get_ui(n->qnumbr);
}

/* mpfp_tolong --- conversion to long */

static long
mpfp_tolong(const NODE *n)
{
	return (n->flags & MPFN) != 0 ? mpfr_get_si(n->qnumbr, ROUND_MODE) : mpz_get_si(n->qnumbr);
}

/* mpfp_todouble --- conversion to AWKNUM */

static AWKNUM
mpfp_todouble(const NODE *n)
{
	return (n->flags & MPFN) != 0 ? mpfr_get_d(n->qnumbr, ROUND_MODE) : mpz_get_d(n->qnumbr);
}

/* mpfp_touintmax_t --- conversion to uintmax_t */

static uintmax_t
mpfp_touintmax_t(const NODE *n)
{
	return (n->flags & MPFN) != 0 ? mpfr_get_uj(n->qnumbr, ROUND_MODE) \
			: (uintmax_t) mpz_get_d(n->qnumbr);
}

/* mpfp_sgn --- return 1 if number > 0, zero if number == 0, and -1 if number < 0 */

static int
mpfp_sgn(const NODE *n)
{
	return (n->flags & MPFN) != 0 ? mpfr_sgn(MPFR_T(n->qnumbr)) \
		: mpz_sgn(MPZ_T(n->qnumbr));
}

/* mpfp_is_integer --- check if a number is an integer */

static bool
mpfp_is_integer(const NODE *n)
{
	return is_mpfp_integer(n) ? true : mpfr_integer_p(n->qnumbr);
}

/* mpfp_make_node --- allocate a node to store MPFR float or GMP integer */

static NODE *
mpfp_make_node(unsigned int type)
{
	NODE *r;

	getnode(r);
	r->type = Node_val;
	if (type == MPFN) {
		/* Initialize, set precision to the default precision, and value to NaN */
		emalloc(r->qnumbr, void *, sizeof (mpfr_t), "mpfp_make_node");
		mpfr_init(r->qnumbr);
		r->flags = MPFN;
	} else {
		/* Initialize and set value to 0 */
		emalloc(r->qnumbr, void *, sizeof (mpz_t), "mpfp_make_node");
		mpz_init(r->qnumbr);
		r->flags = MPZN;
	}
	
	r->valref = 1;
	r->flags |= (MALLOC|NUMBER|NUMCUR);
	r->stptr = NULL;
	r->stlen = 0;
#if MBS_SUPPORT
	r->wstptr = NULL;
	r->wstlen = 0;
#endif /* defined MBS_SUPPORT */
	return r;
}

/*
 * mpfp_make_number --- make a arbitrary-precision number node
 *	and initialize with AWKNUM.
 */

static NODE *
mpfp_make_number(AWKNUM x)
{
	NODE *r;
	int tval;

	r = mpfp_float();
	tval = mpfr_set_d(r->qnumbr, x, ROUND_MODE);
	IEEE_FMT(r->qnumbr, tval);
	return r;
}

/* mpfp_strtoui --- assign arbitrary-precision integral value from a string */ 

static int
mpfp_strtoui(mpz_ptr zi, char *str, size_t len, char **end, int base)
{
	char *s = str;
	char *start;
	int ret = -1;

	/*
	 * mpz_set_str does not like leading 0x or 0X for hex (or 0 for octal)
	 * with a non-zero base argument.
	*/
	if (base == 16 && len >= 2 && *s == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2; len -= 2;
	} else if (base == 8 && len >= 1 && *s == '0') {
		s++; len--;
	}
	start = s;

	while (len > 0) {
		switch (*s) {
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
			break;
		case '8':
		case '9':
			if (base == 8)
				goto done;
			break;
		case 'a':
		case 'b':
		case 'c':
		case 'd':
		case 'e':
		case 'f':
		case 'A':
		case 'B':
		case 'C':
		case 'D':
		case 'E':
		case 'F':
			if (base == 16)
				break;
		default:
			goto done;
		}
		s++; len--;
	}
done:
	if (s > start) {
		char save = *s;
		*s = '\0';
		ret = mpz_set_str(zi, start, base);
		*s = save;
	}
	if (end != NULL)
		*end = s;
	return ret;
}


/* mpfp_maybe_float --- test if a string may contain arbitrary-precision float */

static int
mpfp_maybe_float(const char *str, int use_locale)
{
	int dec_point = '.';
	const char *s = str;

#if defined(HAVE_LOCALE_H)
	/*
	 * loc.decimal_point may not have been initialized yet,
	 * so double check it before using it.
	 */
	if (use_locale && loc.decimal_point != NULL && loc.decimal_point[0] != '\0')
		dec_point = loc.decimal_point[0];	/* XXX --- assumes one char */
#endif

	if (strlen(s) >= 3
		&& (   (   (s[0] == 'i' || s[0] == 'I')
			&& (s[1] == 'n' || s[1] == 'N')
			&& (s[2] == 'f' || s[2] == 'F'))
		    || (   (s[0] == 'n' || s[0] == 'N')
			&& (s[1] == 'a' || s[1] == 'A')
			&& (s[2] == 'n' || s[2] == 'N'))))
		return true;

	for (; *s != '\0'; s++) {
		if (*s == dec_point || *s == 'e' || *s == 'E')
			return true;
	}

	return false;
}


/*
 * mpfp_init_zero --- initialize with arbitrary-precision integer and set value to zero.
 *	N.B. : this function also converts MPFR number to GMP number.
 */

static void
mpfp_init_zero(NODE *n)
{
	if (is_mpfp_float(n)) {
		mpfr_clear(n->qnumbr);
		efree(n->qnumbr);
		n->qnumbr = NULL;
		n->flags &= ~MPFN;
	}
	if (! is_mpfp_integer(n)) {
		emalloc(n->qnumbr, void *, sizeof (mpz_t), "mpfp_init_zero");
		mpz_init(n->qnumbr);	/* this also sets its value to 0 */ 
		n->flags |= MPZN;
	} else
		mpz_set_si(n->qnumbr, 0);
}


/* mpfp_str2num --- force a value to be a GMP integer or MPFR float */

static bool
mpfp_str2num(NODE *n, int do_nondec, int use_locale)
{
	char *cp, *cpend, *ptr, *cp1;
	char save;
	int tval, base = 10;

	if (n->stlen == 0) {
		mpfp_init_zero(n);	/* GMP integer */
		return false;
	}

	cp = n->stptr;
	cpend = n->stptr + n->stlen;
	while (cp < cpend && isspace((unsigned char) *cp))
		cp++;
	if (cp == cpend) {	/* only spaces */
		mpfp_init_zero(n);
		return false;
	}

	save = *cpend;
	*cpend = '\0';

	if (*cp == '+' || *cp == '-')
		cp1 = cp + 1;
	else
		cp1 = cp;

	if (do_nondec)
		base = get_numbase(cp1, use_locale);

	if (! mpfp_maybe_float(cp1, use_locale)) {
		mpfp_init_zero(n);	/* GMP integer */
		errno = 0;
		mpfp_strtoui(n->qnumbr, cp1, cpend - cp1, & ptr, base);
		if (*cp == '-')
			mpz_neg(n->qnumbr, n->qnumbr);
		goto done;
	}

	if (is_mpfp_integer(n)) {
		mpz_clear(n->qnumbr);
		efree(n->qnumbr);
		n->flags &= ~MPZN;
	}

	if (! is_mpfp_float(n)) {
		emalloc(n->qnumbr, void *, sizeof (mpfr_t), "mpfp_str2num");
		mpfr_init(n->qnumbr);
		n->flags |= MPFN;
	}

	errno = 0;
	tval = mpfr_strtofr(n->qnumbr, cp, & ptr, base, ROUND_MODE);
	IEEE_FMT(n->qnumbr, tval);
done:
	/* trailing space is OK for NUMBER */
	while (isspace((unsigned char) *ptr))
		ptr++;
	*cpend = save;
	if (errno == 0 && ptr == cpend)
		return true;
	errno = 0;
	return false;
}

/* mpfp_force_number --- force a value to be a multiple-precision number */

static NODE *
mpfp_force_number(NODE *n)
{
	unsigned int newflags = 0;

	if (is_mpfp_number(n) && (n->flags & NUMCUR) != 0)
		return n;

	if ((n->flags & MAYBE_NUM) != 0) {
		n->flags &= ~MAYBE_NUM;
		newflags = NUMBER;
	}

	if (mpfp_str2num(n, (do_non_decimal_data && ! do_traditional), true)) {
		n->flags |= newflags;
		n->flags |= NUMCUR;
	}
	return n;
}

/* mpfp_format_val --- format a numeric value based on format */

static NODE *
mpfp_format_val(const char *format, int index, NODE *s)
{
	NODE *dummy[2], *r;
	unsigned int oflags;

	/* create dummy node for a sole use of format_tree */
	dummy[1] = s;
	oflags = s->flags;

	if (is_mpfp_integer(s) || mpfr_integer_p(s->qnumbr)) {
		/* integral value, use %d */
		r = mpfp_format_nodes("%d", 2, dummy, 2);
		s->stfmt = -1;
	} else {
		r = mpfp_format_nodes(format, fmt_list[index]->stlen, dummy, 2);
		assert(r != NULL);
		s->stfmt = (char) index;
	}
	s->flags = oflags;
	s->stlen = r->stlen;
	if ((s->flags & STRCUR) != 0)
		efree(s->stptr);
	s->stptr = r->stptr;
	freenode(r);	/* Do not unref(r)! We want to keep s->stptr == r->stpr.  */
 
	s->flags |= STRCUR;
	free_wstr(s);
	return s;
}

/* mpfp_str2node --- create an arbitrary-pecision number from string */

static NODE *
mpfp_str2node(char *str, char **endptr, int base, bool is_integer)
{
	NODE *r;

	if (is_integer) {
		r = mpfp_integer();
		mpfp_strtoui(r->qnumbr, str, strlen(str), endptr, base);
	} else {
		int tval;
		r = mpfp_float();
		tval = mpfr_strtofr(r->qnumbr, str, endptr, base, ROUND_MODE);
		IEEE_FMT(r->qnumbr, tval);
	}
	return r;
}

/* mpfp_free_num --- free all storage allocated for a multiple-precision number */

static void
mpfp_free_num(NODE *tmp)
{
	assert((tmp->flags & (MPFN|MPZN)) != 0);
        if (is_mpfp_float(tmp))
                mpfr_clear(tmp->qnumbr);
        else /* if (is_mpfp_integer(tmp)) */
                mpz_clear(tmp->qnumbr);
	efree(tmp->qnumbr);
	tmp->qnumbr = NULL;
}

/* mpfp_compare --- compare two numbers */

static int
mpfp_compare(const NODE *t1, const NODE *t2)
{
	/*
	 * For the purposes of sorting, NaN is considered greater than
	 * any other value, and all NaN values are considered equivalent and equal.
	 */

	if (is_mpfp_float(t1)) {
		if (is_mpfp_float(t2)) {
			if (mpfr_nan_p(MPFR_T(t1->qnumbr)))
				return ! mpfr_nan_p(MPFR_T(t2->qnumbr));
			if (mpfr_nan_p(MPFR_T(t2->qnumbr)))
				return -1;
			return mpfr_cmp(t1->qnumbr, t2->qnumbr);
		}
		if (mpfr_nan_p(MPFR_T(t1->qnumbr)))
			return 1;
		return mpfr_cmp_z(t1->qnumbr, t2->qnumbr);
	} else if (is_mpfp_float(t2)) {
		int ret;

		if (mpfr_nan_p(MPFR_T(t2->qnumbr)))
			return -1;
		ret = mpfr_cmp_z(t2->qnumbr, t1->qnumbr);
		return ret > 0 ? -1 : (ret < 0);
	}
	assert(is_mpfp_integer(t1) == true);
	return mpz_cmp(t1->qnumbr, t2->qnumbr);
}

/* mpfp_init_vars --- set PREC and ROUNDMODE defaults */

static void
mpfp_init_vars()
{
	unref(PREC_node->var_value);
	PREC_node->var_value = mpfp_make_number(DEFAULT_PREC);
	unref(ROUNDMODE_node->var_value);
	ROUNDMODE_node->var_value = make_string(DEFAULT_ROUNDMODE, strlen(DEFAULT_ROUNDMODE));
}


/* mpfp_set_PREC --- update MPFR PRECISION related variables when PREC assigned to */

static void
mpfp_set_PREC(const NODE *var)
{
	long prec = 0;
	NODE *val;
	static const struct ieee_fmt {
		const char *name;
		mpfr_prec_t precision;
		mpfr_exp_t emax;
		mpfr_exp_t emin;
	} ieee_fmts[] = {
{ "half",	11,	16,	-23	},	/* binary16 */
{ "single",	24,	128,	-148	},	/* binary32 */
{ "double",	53,	1024,	-1073	},	/* binary64 */
{ "quad",	113,	16384,	-16493	},	/* binary128 */
{ "oct",	237,	262144,	-262377	},	/* binary256, not in the IEEE 754-2008 standard */

		/*
 		 * For any bitwidth = 32 * k ( k >= 4),
 		 * precision = 13 + bitwidth - int(4 * log2(bitwidth))
		 * emax = 1 << bitwidth - precision - 1
		 * emin = 4 - emax - precision
 		 */
	};

	val = var->var_value;
	if ((val->flags & MAYBE_NUM) != 0)
		(void) force_number(val);

	if ((val->flags & (STRING|NUMBER)) == STRING) {
		int i, j;

		/* emulate IEEE-754 binary format */

		for (i = 0, j = sizeof(ieee_fmts)/sizeof(ieee_fmts[0]); i < j; i++) {
			if (strcasecmp(ieee_fmts[i].name, val->stptr) == 0)
				break;
		}

		if (i < j) {
			prec = ieee_fmts[i].precision;

			/*
			 * We *DO NOT* change the MPFR exponent range using
			 * mpfr_set_{emin, emax} here. See format_ieee() for details.
			 */
			max_exp = ieee_fmts[i].emax;
			min_exp = ieee_fmts[i].emin;
			do_ieee_fmt = true;
		}
	}

	if (prec <= 0) {
		force_number(val);
		prec = get_number_si(val);		
		if (prec < MPFR_PREC_MIN || prec > MPFR_PREC_MAX) {
			(void) force_string(val);
			warning(_("PREC value `%.*s' is invalid"), (int) val->stlen, val->stptr);
			prec = 0;
		} else
			do_ieee_fmt = false;
	}

	if (prec > 0)
		mpfr_set_default_prec(prec);
}


/* mpfp_get_rounding_mode --- convert string to MPFR rounding mode */

static mpfr_rnd_t
mpfp_get_rounding_mode(const char rmode)
{
	switch (rmode) {
	case 'N':
	case 'n':
		return MPFR_RNDN;	/* round to nearest (IEEE-754 roundTiesToEven) */
	case 'Z':
	case 'z':
		return MPFR_RNDZ;	/* round toward zero (IEEE-754 roundTowardZero) */
	case 'U':
	case 'u':
		return MPFR_RNDU;	/* round toward plus infinity (IEEE-754 roundTowardPositive) */
	case 'D':
	case 'd':
		return MPFR_RNDD;	/* round toward minus infinity (IEEE-754 roundTowardNegative) */
#if defined(MPFR_VERSION_MAJOR) && MPFR_VERSION_MAJOR > 2
	case 'A':
	case 'a':
		return MPFR_RNDA;	/* round away from zero (IEEE-754 roundTiesToAway) */
#endif
	default:
		break;
	}
	return -1;
}

/*
 * mpfp_set_ROUNDMODE --- update MPFR rounding mode related variables
 *	when ROUNDMODE assigned to
 */

static void
mpfp_set_ROUNDMODE(const NODE *var)
{
	mpfr_rnd_t rndmode = -1;
	NODE *val;

	val = force_string(var->var_value);
	if (val->stlen == 1)
		rndmode = mpfp_get_rounding_mode(val->stptr[0]);
	if (rndmode != -1) {
		mpfr_set_default_rounding_mode(rndmode);
		ROUND_MODE = rndmode;
	} else
		warning(_("ROUNDMODE value `%.*s' is invalid"), (int) val->stlen, val->stptr);
}

/*
 * mpfp_update_var --- update NR or FNR. 
 *	NR_node->var_value(mpz_t) = MNR(mpz_t) * LONG_MAX + NR(long) 
 */

static NODE *
mpfp_update_var(NODE *n)
{
	NODE *val = n->var_value;
	long nr = 0;
	mpz_ptr nq = 0;

	if (n == NR_node) {
		nr = NR;
		nq = MNR;
	} else {
		assert(n == FNR_node);
		nr = FNR;
		nq = MFNR;
	}

	if (mpz_sgn(nq) == 0) {
		/* Efficiency hack similar to that for AWKNUM */
		if (is_mpfp_float(val) || mpz_get_si(val->qnumbr) != nr) {
			unref(val);
			val = n->var_value = mpfp_integer();
			mpz_set_si(val->qnumbr, nr);
		}
	} else {
		unref(val);
		val = n->var_value = mpfp_integer();
		mpz_set_si(val->qnumbr, nr);
		mpz_addmul_ui(val->qnumbr, nq, LONG_MAX);	/* val->mpg_i += nq * LONG_MAX */
	}
	return val;
}

/* mpfp_set_var --- set internal variables */

static void
mpfp_set_var(const NODE *var)
{
	if (var == PREC_node)
		mpfp_set_PREC(var);
	else if (var == ROUNDMODE_node)
		mpfp_set_ROUNDMODE(var);
	else {
		NODE *val = var->var_value;
		mpz_ptr r;
		mpz_t mpz_val;

		if (is_mpfp_integer(val))
			r = val->qnumbr;
		else {
			/* convert float to integer */
			mpz_init(mpz_val);
			mpfr_get_z(mpz_val, val->qnumbr, MPFR_RNDZ);
			r = mpz_val;
		}

		if (var == NR_node)
			NR = mpz_fdiv_q_ui(MNR, r, LONG_MAX);	/* MNR is quotient */
		else
			FNR = mpz_fdiv_q_ui(MFNR, r, LONG_MAX);

		if (r != val->qnumbr)
			mpz_clear(mpz_val);
	}
}

/* mpfp_increment_var --- increment NR or FNR */

static long
mpfp_increment_var(const NODE *var, long nr)
{
	if (nr == LONG_MAX - 1) {
		/* increment quotient, set remainder(NR or FNR) to 0 */
		if (var == NR_node)
 			mpz_add_ui(MNR, MNR, 1);
		else /* if (var == FNR_node) */
			mpz_add_ui(MFNR, MFNR, 1);
		return 0;
	}
	return ++nr;
}

/* mpfp_format_ieee --- make sure a number follows IEEE-754 floating-point standard */

static int
mpfp_format_ieee(mpfr_ptr x, int tval)
{
	/*
	 * The MPFR doc says that it's our responsibility to make sure all numbers
	 * including those previously created are in range after we've changed the
	 * exponent range. Most MPFR operations and functions require
	 * the input arguments to have exponents within the current exponent range.
	 * Any argument outside the range results in a MPFR assertion failure
	 * like this:
	 *
	 *   $ gawk -M 'BEGIN { x=1.0e-10000; print x+0; PREC="double"; print x+0}'
	 *   1e-10000
	 *   init2.c:52: MPFR assertion failed ....
	 *
	 * A "naive" approach would be to keep track of the ternary state and
	 * the rounding mode for each number, and make sure it is in the current
	 * exponent range (using mpfr_check_range) before using it in an
	 * operation or function. Instead, we adopt the following strategy.
	 *
	 * When gawk starts, the exponent range is the MPFR default
	 * [MPFR_EMIN_DEFAULT, MPFR_EMAX_DEFAULT]. Any number that gawk
	 * creates must have exponent in this range (excluding infinities, NaNs and zeros).
	 * Each MPFR operation or function is performed with this default exponent
	 * range.
	 *
	 * When emulating IEEE-754 format, the exponents are *temporarily* changed,
	 * mpfr_check_range is called to make sure the number is in the new range,
	 * and mpfr_subnormalize is used to round following the rules of subnormal
	 * arithmetic. The exponent range is then *restored* to the original value
	 * [MPFR_EMIN_DEFAULT, MPFR_EMAX_DEFAULT].
	 */

	(void) mpfr_set_emin(min_exp);
	(void) mpfr_set_emax(max_exp);
	tval = mpfr_check_range(x, tval, ROUND_MODE);
	tval = mpfr_subnormalize(x, tval, ROUND_MODE);
	(void) mpfr_set_emin(MPFR_EMIN_DEFAULT);
	(void) mpfr_set_emax(MPFR_EMAX_DEFAULT);
	return tval;
}

/* mpfp_negate_num --- negate a number in NODE */

static void
mpfp_negate_num(NODE *n)
{
	if (is_mpfp_float(n)) {
		int tval;
		tval = mpfr_neg(n->qnumbr, n->qnumbr, ROUND_MODE);
		IEEE_FMT(n->qnumbr, tval);
	} else /* if (is_mpfp_integer(n)) */
		mpz_neg(n->qnumbr, n->qnumbr);
}

/* do_mpfp_atan2 --- do the atan2 function */

static NODE *
do_mpfp_atan2(int nargs)
{
	NODE *t1, *t2, *res;
	mpfr_ptr p1, p2;
	int tval;

	t2 = POP_SCALAR();
	t1 = POP_SCALAR();

	if (do_lint) {
		if ((t1->flags & (NUMCUR|NUMBER)) == 0)
			lintwarn(_("atan2: received non-numeric first argument"));
		if ((t2->flags & (NUMCUR|NUMBER)) == 0)
			lintwarn(_("atan2: received non-numeric second argument"));
	}

	(void) force_number(t1);
	(void) force_number(t2);

	p1 = mpfp_tofloat(t1, _mp1);
	p2 = mpfp_tofloat(t2, _mp2);

	res = mpfp_float();
	/* See MPFR documentation for handling of special values like +inf as an argument */ 
	tval = mpfr_atan2(res->qnumbr, p1, p2, ROUND_MODE);
	IEEE_FMT(res->qnumbr, tval);

	DEREF(t1);
	DEREF(t2);
	return res;
}


#define MPFPFUNC(X)						\
NODE *t1, *res;							\
mpfr_ptr p1;							\
int tval;							\
t1 = POP_SCALAR();						\
if (do_lint && (t1->flags & (NUMCUR|NUMBER)) == 0)		\
	lintwarn(_("%s: received non-numeric argument"), #X);	\
t1 = force_number(t1);						\
p1 = mpfp_tofloat(t1, _mp1);					\
res = mpfp_float();						\
tval = mpfr_##X(res->qnumbr, p1, ROUND_MODE);			\
IEEE_FMT(res->qnumbr, tval);					\
DEREF(t1);							\
return res


/* do_mpfp_sin --- do the sin function */

static NODE *
do_mpfp_sin(int nargs)
{
	MPFPFUNC(sin);
}

/* do_mpfp_cos --- do the cos function */

static NODE *
do_mpfp_cos(int nargs)
{
	MPFPFUNC(cos);
}

/* do_mpfp_exp --- exponential function */

static NODE *
do_mpfp_exp(int nargs)
{
	MPFPFUNC(exp);
}

/* do_mpfp_log --- the log function */

static NODE *
do_mpfp_log(int nargs)
{
	MPFPFUNC(log);
}

/* do_mpfp_sqrt --- do the sqrt function */

static NODE *
do_mpfp_sqrt(int nargs)
{
	MPFPFUNC(sqrt);
}

/* do_mpfp_int --- convert floating point number to integer for awk */

static NODE *
do_mpfp_int(int nargs)
{
	NODE *tmp, *r;

	tmp = POP_SCALAR();
	if (do_lint && (tmp->flags & (NUMCUR|NUMBER)) == 0)
		lintwarn(_("int: received non-numeric argument"));
	tmp = force_number(tmp);

	if (is_mpfp_integer(tmp)) {
		r = mpfp_integer();
		mpz_set(r->qnumbr, tmp->qnumbr);
	} else {
		if (! mpfr_number_p(tmp->qnumbr)) {
			/* [+-]inf or NaN */
			return tmp;
		}
		r = mpfp_integer();
		mpfr_get_z(r->qnumbr, tmp->qnumbr, MPFR_RNDZ);
	}

	DEREF(tmp);
	return r;
}

/* do_mpfp_compl --- perform a ~ operation */

static NODE *
do_mpfp_compl(int nargs)
{
	NODE *tmp, *r;
	mpz_ptr zptr;

	tmp = POP_SCALAR();
	if (do_lint && (tmp->flags & (NUMCUR|NUMBER)) == 0)
		lintwarn(_("compl: received non-numeric argument"));

	(void) force_number(tmp);
	if (is_mpfp_float(tmp)) {
		mpfr_ptr p = tmp->qnumbr;

		if (! mpfr_number_p(p)) {
			/* [+-]inf or NaN */
			return tmp;
		}
		if (do_lint) {
			if (mpfr_sgn(p) < 0)
				lintwarn("%s",
			mpfp_sprintf(_("compl(%Rg): negative value will give strange results"), p)
					);
			if (! mpfr_integer_p(p))
				lintwarn("%s",
			mpfp_sprintf(_("comp(%Rg): fractional value will be truncated"), p)
					);
		}

		emalloc(zptr, mpz_ptr, sizeof (mpz_t), "do_mpfr_compl");
		mpz_init(zptr);
		mpfr_get_z(zptr, p, MPFR_RNDZ);	/* float to integer conversion */

	} else {
		/* (tmp->flags & MPZN) != 0 */ 
		zptr = tmp->qnumbr;
		if (do_lint) {
			if (mpz_sgn(zptr) < 0)
				lintwarn("%s",
			mpfp_sprintf(_("cmpl(%Zd): negative values will give strange results"), zptr)
					);
		}
	}

	r = mpfp_integer();
	mpz_com(r->qnumbr, zptr);

	if (zptr != tmp->qnumbr) {
		mpz_clear(zptr);
		efree(zptr);
	}
	DEREF(tmp);
	return r;
}


/* get_intval --- get the (converted) integral operand of a binary function. */

static mpz_ptr
get_intval(NODE *t1, int argnum, const char *op)
{
	mpz_ptr pz;

	if (do_lint && (t1->flags & (NUMCUR|NUMBER)) == 0)
		lintwarn(_("%s: received non-numeric argument #%d"), op, argnum);

	(void) force_number(t1);

	if (is_mpfp_float(t1)) {
		mpfr_ptr left = t1->qnumbr;
		if (! mpfr_number_p(left)) {
			/* inf or NaN */
			NODE *res;
			if (do_lint)
                       		lintwarn("%s",
		mpfp_sprintf(_("%s: argument #%d has invalid value %Rg, using 0"),
                                	op, argnum, left)
				);
			emalloc(pz, mpz_ptr, sizeof (mpz_t), "get_intval");
			mpz_init(pz);
			return pz;	/* should be freed */
		}

		if (do_lint) {
			if (mpfr_sgn(left) < 0)
				lintwarn("%s",
		mpfp_sprintf(_("%s: argument #%d negative value %Rg will give strange results"),
					op, argnum, left)
				);

			if (! mpfr_integer_p(left))
				lintwarn("%s",
		mpfp_sprintf(_("%s: argument #%d fractional value %Rg will be truncated"),
					op, argnum, left)
				);
		}

		emalloc(pz, mpz_ptr, sizeof (mpz_t), "get_intval");
		mpz_init(pz);
		mpfr_get_z(pz, left, MPFR_RNDZ);	/* float to integer conversion */
		return pz;	/* should be freed */
	}

	/* (t1->flags & MPZN) != 0 */ 
	pz = t1->qnumbr;
	if (do_lint) {
		if (mpz_sgn(pz) < 0)
			lintwarn("%s",
	mpfp_sprintf(_("%s: argument #%d negative value %Zd will give strange results"),
					op, argnum, pz)
				);
	}
	return pz;	/* must not be freed */
}

/* free_intval --- free the converted integer value returned by get_intval() */

static inline void
free_intval(NODE *t, mpz_ptr pz)
{
	if (t->qnumbr != pz) {
		mpz_clear(pz);
		efree(pz);
	}
}

/* do_mpfp_lshift --- perform a << operation */

static NODE *
do_mpfp_lshift(int nargs)
{
	NODE *t1, *t2, *res;
	unsigned long shift;
	mpz_ptr pz1, pz2;
 
	t2 = POP_SCALAR();
	t1 = POP_SCALAR();

	pz1 = get_intval(t1, 1, "lshift");
	pz2 = get_intval(t2, 2, "lshift");

	/*
	 * mpz_get_ui: If op is too big to fit an unsigned long then just
	 * the least significant bits that do fit are returned.
	 * The sign of op is ignored, only the absolute value is used.
	 */

	shift = mpz_get_ui(pz2);	/* GMP integer => unsigned long conversion */
	res = mpfp_integer();
	mpz_mul_2exp(res->qnumbr, pz1, shift);		/* res = pz1 * 2^shift */

	free_intval(t1, pz1);
	free_intval(t2, pz2);
	DEREF(t2);
	DEREF(t1);
	return res;
}

/* do_mpfp_rshift --- perform a >> operation */

static NODE *
do_mpfp_rshift(int nargs)
{
	NODE *t1, *t2, *res;
	unsigned long shift;
	mpz_ptr pz1, pz2;
 
	t2 = POP_SCALAR();
	t1 = POP_SCALAR();

	pz1 = get_intval(t1, 1, "rshift");
	pz2 = get_intval(t2, 2, "rshift");

	/* N.B: See do_mpfp_lshift. */
	shift = mpz_get_ui(pz2);	/* GMP integer => unsigned long conversion */
	res = mpfp_integer();
	mpz_fdiv_q_2exp(res->qnumbr, pz1, shift);	/* res = pz1 / 2^shift, round towards −inf */

	free_intval(t1, pz1);
	free_intval(t2, pz2);
	DEREF(t2);
	DEREF(t1);
	return res;
}

/* do_mpfp_and --- perform an & operation */

static NODE *
do_mpfp_and(int nargs)
{
	NODE *t1, *t2, *res;
	mpz_ptr pz1, pz2;
	int i;

	if (nargs < 2)
		fatal(_("and: called with less than two arguments"));

	t2 = POP_SCALAR();
	pz2 = get_intval(t2, nargs, "and");

	res = mpfp_integer();
	for (i = 1; i < nargs; i++) {
		t1 = POP_SCALAR();
		pz1 = get_intval(t1, nargs - i, "and");
		mpz_and(res->qnumbr, pz1, pz2);
		free_intval(t1, pz1);
		DEREF(t1);
		if (i == 1) {
			free_intval(t2, pz2);
			DEREF(t2);
		}
		pz2 = res->qnumbr;
	}
	return res;
}

/* do_mpfp_or --- perform an | operation */

static NODE *
do_mpfp_or(int nargs)
{
	NODE *t1, *t2, *res;
	mpz_ptr pz1, pz2;
	int i;

	if (nargs < 2)
		fatal(_("or: called with less than two arguments"));

	t2 = POP_SCALAR();
	pz2 = get_intval(t2, nargs, "or");

	res = mpfp_integer();
	for (i = 1; i < nargs; i++) {
		t1 = POP_SCALAR();
		pz1 = get_intval(t1, nargs - i, "or");
		mpz_ior(res->qnumbr, pz1, pz2);
		free_intval(t1, pz1);
		DEREF(t1);
		if (i == 1) {
			free_intval(t2, pz2);
			DEREF(t2);
		}
		pz2 = res->qnumbr;
	}
	return res;
}

/* do_mpfp_xor --- perform an ^ operation */

static NODE *
do_mpfp_xor(int nargs)
{
	NODE *t1, *t2, *res;
	mpz_ptr pz1, pz2;
	int i;

	if (nargs < 2)
		fatal(_("xor: called with less than two arguments"));

	t2 = POP_SCALAR();
	pz2 = get_intval(t2, nargs, "xor");

	res = mpfp_integer();
	for (i = 1; i < nargs; i++) {
		t1 = POP_SCALAR();
		pz1 = get_intval(t1, nargs - i, "xor");
		mpz_xor(res->qnumbr, pz1, pz2);
		free_intval(t1, pz1);
		DEREF(t1);
		if (i == 1) {
			free_intval(t2, pz2);
			DEREF(t2);
		}
		pz2 = res->qnumbr;
	}
	return res;
}

/* do_mpfp_strtonum --- the strtonum function */

static NODE *
do_mpfp_strtonum(int nargs)
{
	NODE *tmp, *r;

	tmp = POP_SCALAR();
	if ((tmp->flags & (NUMBER|NUMCUR)) == 0) {
		r = mpfp_integer();	/* will be changed to MPFR float if necessary in force_mpnum() */
		r->stptr = tmp->stptr;
		r->stlen = tmp->stlen;
		mpfp_str2num(r, true, use_lc_numeric);
		r->stptr = NULL;
		r->stlen = 0;
	} else {
		(void) force_number(tmp);
		if (is_mpfp_float(tmp)) {
			int tval;
			r = mpfp_float();
			tval = mpfr_set(r->qnumbr, (mpfr_ptr) tmp->qnumbr, ROUND_MODE);
			IEEE_FMT(r->qnumbr, tval);
		} else {
			r = mpfp_integer();
			mpz_set(r->qnumbr, tmp->qnumbr);
		}
	}

	DEREF(tmp);
	return r;
}


static bool firstrand = true;
static gmp_randstate_t state;
static mpz_t seed;	/* current seed */

/* do_mpfp_rand --- do the rand function */

static NODE *
do_mpfp_rand(int nargs ATTRIBUTE_UNUSED)
{
	NODE *res;
	int tval;

	if (firstrand) {
#if 0
		/* Choose the default algorithm */
		gmp_randinit_default(state);
#endif
		/*
		 * Choose a specific (Mersenne Twister) algorithm in case the default
		 * changes in the future.
		 */

		gmp_randinit_mt(state);

		mpz_init(seed);
		mpz_set_ui(seed, 1);
		/* seed state */
		gmp_randseed(state, seed);
		firstrand = false;
	}
	res = mpfp_float();
	tval = mpfr_urandomb(res->qnumbr, state);
	IEEE_FMT(res->qnumbr, tval);
	return res;
}


/* do_mpfp_srand --- seed the random number generator */

static NODE *
do_mpfp_srand(int nargs)
{
	NODE *res;

	if (firstrand) {
#if 0
		/* Choose the default algorithm */
		gmp_randinit_default(state);
#endif
		/*
		 * Choose a specific algorithm (Mersenne Twister) in case default
		 * changes in the future.
		 */

		gmp_randinit_mt(state);

		mpz_init(seed);
		mpz_set_ui(seed, 1);
		/* No need to seed state, will change it below */
		firstrand = false;
	}

	res = mpfp_integer();
	mpz_set(res->qnumbr, seed);	/* previous seed */

	if (nargs == 0)
		mpz_set_ui(seed, (unsigned long) time((time_t *) 0));
	else {
		NODE *tmp;
		tmp = POP_SCALAR();
		if (do_lint && (tmp->flags & (NUMCUR|NUMBER)) == 0)
			lintwarn(_("srand: received non-numeric argument"));
		(void) force_number(tmp);
		if (is_mpfp_float(tmp))
			mpfr_get_z(seed, tmp->qnumbr, MPFR_RNDZ);
		else /* MP integer */
			mpz_set(seed, tmp->qnumbr);
		DEREF(tmp);
	}

	gmp_randseed(state, seed);
	return res;
}


/* mpfp_add --- add arbitrary-precision numbers */ 

static NODE *
mpfp_add(const NODE *t1, const NODE *t2)
{
	NODE *r;
	int tval;

	if (is_mpfp_integer(t1) && is_mpfp_integer(t2)) {
		r = mpfp_integer();
		mpz_add(r->qnumbr, t1->qnumbr, t2->qnumbr);
	} else {
		r = mpfp_float();
		if (is_mpfp_integer(t2))
			tval = mpfr_add_z(r->qnumbr, t1->qnumbr, t2->qnumbr, ROUND_MODE);
		else if (is_mpfp_integer(t1))
			tval = mpfr_add_z(r->qnumbr, t2->qnumbr, t1->qnumbr, ROUND_MODE);
		else
			tval = mpfr_add(r->qnumbr, t1->qnumbr, t2->qnumbr, ROUND_MODE);
		IEEE_FMT(r->qnumbr, tval);
	}
	return r;
}

/* mpfp_sub --- subtract arbitrary-precision numbers */

static NODE *
mpfp_sub(const NODE *t1, const NODE *t2)
{
	NODE *r;
	int tval;

	if (is_mpfp_integer(t1) && is_mpfp_integer(t2)) {
		r = mpfp_integer();
		mpz_sub(r->qnumbr, t1->qnumbr, t2->qnumbr);
	} else {
		r = mpfp_float();
		if (is_mpfp_integer(t2))
			tval = mpfr_sub_z(r->qnumbr, t1->qnumbr, t2->qnumbr, ROUND_MODE);
		else if (is_mpfp_integer(t1)) {
#if (!defined(MPFR_VERSION) || (MPFR_VERSION < MPFR_VERSION_NUM(3,1,0)))
			const NODE *tmp = t1;

			t1 = t2;
			t2 = tmp;
			tval = mpfr_sub_z(r->qnumbr, t1->qnumbr, t2->qnumbr, ROUND_MODE);
			tval = mpfr_neg(r->qnumbr, r->qnumbr, ROUND_MODE);
			t2 = t1;
			t1 = tmp;
#else
			tval = mpfr_z_sub(r->qnumbr, t1->qnumbr, t2->qnumbr, ROUND_MODE);
#endif
		} else
			tval = mpfr_sub(r->qnumbr, t1->qnumbr, t2->qnumbr, ROUND_MODE);
		IEEE_FMT(r->qnumbr, tval);
	}
	return r;
}

/* mpfp_mul --- multiply arbitrary-precision numbers */

static NODE *
mpfp_mul(const NODE *t1, const NODE *t2)
{
	NODE *r;
	int tval;

	if (is_mpfp_integer(t1) && is_mpfp_integer(t2)) {
		r = mpfp_integer();
		mpz_mul(r->qnumbr, t1->qnumbr, t2->qnumbr);
	} else {
		r = mpfp_float();
		if (is_mpfp_integer(t2))
			tval = mpfr_mul_z(r->qnumbr, t1->qnumbr, t2->qnumbr, ROUND_MODE);
		else if (is_mpfp_integer(t1))
			tval = mpfr_mul_z(r->qnumbr, t2->qnumbr, t1->qnumbr, ROUND_MODE);
		else
			tval = mpfr_mul(r->qnumbr, t1->qnumbr, t2->qnumbr, ROUND_MODE);
		IEEE_FMT(r->qnumbr, tval);
	}
	return r;
}

/* mpfp_pow --- exponentiation involving arbitrary-precision numbers */ 

static NODE *
mpfp_pow(const NODE *t1, const NODE *t2)
{
	NODE *r;
	int tval;

	if (is_mpfp_integer(t1) && is_mpfp_integer(t2)) {
		if (mpz_sgn(MPZ_T(t2->qnumbr)) >= 0 && mpz_fits_ulong_p(t2->qnumbr)) {
			r = mpfp_integer();
			mpz_pow_ui(r->qnumbr, t1->qnumbr, mpz_get_ui(t2->qnumbr));
		} else {
			mpfr_ptr p1, p2;

			p1 = mpfp_tofloat(t1, _mp1);
			p2 = mpfp_tofloat(t2, _mp2);
			r = mpfp_float();
			tval = mpfr_pow(r->qnumbr, p1, p2, ROUND_MODE);
			IEEE_FMT(r->qnumbr, tval);
		}
	} else {
		r = mpfp_float();
		if (is_mpfp_integer(t2))
			tval = mpfr_pow_z(r->qnumbr, t1->qnumbr, t2->qnumbr, ROUND_MODE);
		else {
			mpfr_ptr p1;

			p1 = mpfp_tofloat(t1, _mp1);
			tval = mpfr_pow(r->qnumbr, p1, t2->qnumbr, ROUND_MODE);
		}
		IEEE_FMT(r->qnumbr, tval);
	}
	return r;
}

/* mpfp_div --- arbitrary-precision division */

static NODE *
mpfp_div(const NODE *t1, const NODE *t2)
{
	NODE *r;
	int tval;

	if (is_mpfp_integer(t1) && is_mpfp_integer(t2)
			&& (mpz_sgn(MPZ_T(t2->qnumbr)) != 0)	/* not dividing by 0 */
			&& mpz_divisible_p(t1->qnumbr, t2->qnumbr)
	) {
		r = mpfp_integer();
		mpz_divexact(r->qnumbr, t1->qnumbr, t2->qnumbr);
	} else {
		mpfr_ptr p1, p2;

		p1 = mpfp_tofloat(t1, _mp1);
		p2 = mpfp_tofloat(t2, _mp2);
		r = mpfp_float();
		tval = mpfr_div(r->qnumbr, p1, p2, ROUND_MODE);
		IEEE_FMT(r->qnumbr, tval);
	}
	return r;
}

/* mpfp_mod --- modulus operation with arbitrary-precision numbers */

static NODE *
mpfp_mod(const NODE *t1, const NODE *t2)
{
	NODE *r;
	int tval;

	if (is_mpfp_integer(t1) && is_mpfp_integer(t2)) {
		r = mpfp_integer();
		mpz_mod(r->qnumbr, t1->qnumbr, t2->qnumbr);
	} else {
		mpfr_ptr p1, p2;

		p1 = mpfp_tofloat(t1, _mp1);
		p2 = mpfp_tofloat(t2, _mp2);
		r = mpfp_float();
		tval = mpfr_fmod(r->qnumbr, p1, p2, ROUND_MODE);
		IEEE_FMT(r->qnumbr, tval);
	}
	return r;
}

/* mpfp_add_long --- add aribitary-precision number to long */

static NODE *
mpfp_add_long(const NODE *t1, long l)
{
	NODE *r;

	if (is_mpfp_integer(t1)) {
		r = mpfp_integer();
		if (l >= 0)
			mpz_add_ui(r->qnumbr, t1->qnumbr, l);
		else
			mpz_sub_ui(r->qnumbr, t1->qnumbr, l);
	} else {
		int tval;

		r = mpfp_float();
		tval = mpfr_add_si(r->qnumbr, t1->qnumbr, l, ROUND_MODE);
		IEEE_FMT(r->qnumbr, tval);
	}
	return r;
}

/* mpfp_copy_number --- copy an arbitrary-precision number */

static NODE *
mpfp_copy_number(const NODE *n)
{
	NODE *r;

	if (is_mpfp_integer(n)) {
		r = mpfp_integer();
		mpz_set(r->qnumbr, n->qnumbr);
	} else {
		int tval;

		r = mpfp_float();
		tval = mpfr_set(r->qnumbr, MPFR_T(n->qnumbr), ROUND_MODE);
		IEEE_FMT(r->qnumbr, tval);
	}
	return r;
}


/* mpfp_sprintf --- output formatted string with special MPFR/GMP conversion specifiers */

static const char *
mpfp_sprintf(const char *mesg, ...)
{
	static char *tmp = NULL;
	int ret;
	va_list args;

	if (tmp != NULL) {
		mpfr_free_str(tmp);
		tmp = NULL;
	}
	va_start(args, mesg);
	ret = mpfr_vasprintf(& tmp, mesg, args);
	va_end(args);
	if (ret >= 0 && tmp != NULL)
		return tmp;
	return mesg;
}


/*
 * mpz2mpfr --- convert an arbitrary-precision integer to a float
 *	without any loss of precision. If the 2nd arg is NULL, the returned MPFR
 *	value should be freed when done:
 *		mpfr_clear(mpfrval); efree(mpfrval);
 * 	If the 2nd arg is not NULL, it is assumed that the MPFR variable has
 *	already been initialized.
 */


static mpfr_ptr
mpz2mpfr(mpz_ptr mpz_val, mpfr_ptr mpfr_val)
{
	size_t prec;
	int tval;

	/*
	 * When implicitely converting a GMP integer operand to a MPFR float, use
	 * a precision sufficiently large to hold the converted value exactly.
	 * 	
 	 *	$ ./gawk -M 'BEGIN { print 13 % 2 }'
 	 *	1
 	 * If the user-specified precision is used to convert the integer 13 to a
	 * float, one will get:
 	 *	$ ./gawk -M 'BEGIN { PREC=2; print 13 % 2.0 }'
 	 *	0	
 	 */

	/* estimate minimum precision for exact conversion */
	prec = mpz_sizeinbase(mpz_val, 2);	/* most significant 1 bit position starting at 1 */

	if (mpfr_val != NULL && mpfr_get_prec(mpfr_val) >= prec)
		goto finish;

	prec -= (size_t) mpz_scan1(mpz_val, 0);	/* least significant 1 bit index starting at 0 */
	if (prec < MPFR_PREC_MIN)
		prec = MPFR_PREC_MIN;
	else if (prec > MPFR_PREC_MAX)
		prec = MPFR_PREC_MAX;

	if (mpfr_val == NULL) {
		emalloc(mpfr_val, mpfr_ptr, sizeof (mpfr_t), "mpz2mpfr");
		mpfr_init2(mpfr_val, prec);
	} else if (prec < mpfr_get_prec(mpfr_val))
		mpfr_set_prec(mpfr_val, prec);

finish:
	tval = mpfr_set_z(mpfr_val, mpz_val, ROUND_MODE);
	IEEE_FMT(mpfr_val, tval);
	return mpfr_val;
}



extern size_t mbc_byte_count(const char *ptr, size_t numchars);
extern size_t mbc_char_count(const char *ptr, size_t numbytes);

/*
 * mpfp_format_nodes() formats arguments of sprintf,
 * and accordingly to a fmt_string providing a format like in
 * printf family from C library.  Returns a string node which value
 * is a formatted string.  Called by  sprintf function.
 *
 * It is one of the uglier parts of gawk.  Thanks to Michal Jaegermann
 * for taming this beast and making it compatible with ANSI C.
 */

static NODE *
mpfp_format_nodes(
	const char *fmt_string,
	size_t n0,
	NODE **the_args,
	long num_args)
{
/* copy 'l' bytes from 's' to 'obufout' checking for space in the process */
/* difference of pointers should be of ptrdiff_t type, but let us be kind */
#define bchunk(s, l) if (l) { \
	while ((l) > ofre) { \
		size_t olen = obufout - obuf; \
		erealloc(obuf, char *, osiz * 2, "format_tree"); \
		ofre += osiz; \
		osiz *= 2; \
		obufout = obuf + olen; \
	} \
	memcpy(obufout, s, (size_t) (l)); \
	obufout += (l); \
	ofre -= (l); \
}

/* copy one byte from 's' to 'obufout' checking for space in the process */
#define bchunk_one(s) { \
	if (ofre < 1) { \
		size_t olen = obufout - obuf; \
		erealloc(obuf, char *, osiz * 2, "format_tree"); \
		ofre += osiz; \
		osiz *= 2; \
		obufout = obuf + olen; \
	} \
	*obufout++ = *s; \
	--ofre; \
}

/* Is there space for something L big in the buffer? */
#define chksize(l)  if ((l) >= ofre) { \
	size_t olen = obufout - obuf; \
	size_t delta = osiz+l-ofre; \
	erealloc(obuf, char *, osiz + delta, "format_tree"); \
	obufout = obuf + olen; \
	ofre += delta; \
	osiz += delta; \
}

	size_t cur_arg = 0;
	NODE *r = NULL;
	int i, nc;
	bool toofew = false;
	char *obuf, *obufout;
	size_t osiz, ofre;
	const char *chbuf;
	const char *s0, *s1;
	int cs1;
	NODE *arg;
	long fw, prec, argnum;
	bool used_dollar;
	bool lj, alt, big_flag, bigbig_flag, small_flag, have_prec, need_format;
	long *cur = NULL;
	uintmax_t uval;
	bool sgn;
	int base;
	/*
	 * Although this is an array, the elements serve two different
	 * purposes. The first element is the general buffer meant
	 * to hold the entire result string.  The second one is a
	 * temporary buffer for large floating point values. They
	 * could just as easily be separate variables, and the
	 * code might arguably be clearer.
	 */
	struct {
		char *buf;
		size_t bufsize;
		char stackbuf[30];
	} cpbufs[2];
#define cpbuf	cpbufs[0].buf
	char *cend = &cpbufs[0].stackbuf[sizeof(cpbufs[0].stackbuf)];
	char *cp;
	const char *fill;
	AWKNUM tmpval = 0.0;
	char signchar = '\0';
	size_t len;
	bool zero_flag = false;
	bool quote_flag = false;
	int ii, jj;
	char *chp;
	size_t copy_count, char_count;
#ifdef HAVE_MPFR
	mpz_ptr zi;
	mpfr_ptr mf;
#endif
	enum { MP_INT_WITH_PREC = 1, MP_INT_WITHOUT_PREC, MP_FLOAT } fmt_type;

	static const char sp[] = " ";
	static const char zero_string[] = "0";
	static const char lchbuf[] = "0123456789abcdef";
	static const char Uchbuf[] = "0123456789ABCDEF";

#define INITIAL_OUT_SIZE	512
	emalloc(obuf, char *, INITIAL_OUT_SIZE, "format_tree");
	obufout = obuf;
	osiz = INITIAL_OUT_SIZE;
	ofre = osiz - 2;

	cur_arg = 1;

	{
		size_t k;
		for (k = 0; k < sizeof(cpbufs)/sizeof(cpbufs[0]); k++) {
			cpbufs[k].bufsize = sizeof(cpbufs[k].stackbuf);
			cpbufs[k].buf = cpbufs[k].stackbuf;
		}
	}

	/*
	 * The point of this goop is to grow the buffer
	 * holding the converted number, so that large
	 * values don't overflow a fixed length buffer.
	 */
#define PREPEND(CH) do {	\
	if (cp == cpbufs[0].buf) {	\
		char *prev = cpbufs[0].buf;	\
		emalloc(cpbufs[0].buf, char *, 2*cpbufs[0].bufsize, \
		 	"format_tree");	\
		memcpy((cp = cpbufs[0].buf+cpbufs[0].bufsize), prev,	\
		       cpbufs[0].bufsize);	\
		cpbufs[0].bufsize *= 2;	\
		if (prev != cpbufs[0].stackbuf)	\
			efree(prev);	\
		cend = cpbufs[0].buf+cpbufs[0].bufsize;	\
	}	\
	*--cp = (CH);	\
} while(0)

	/*
	 * Check first for use of `count$'.
	 * If plain argument retrieval was used earlier, choke.
	 *	Otherwise, return the requested argument.
	 * If not `count$' now, but it was used earlier, choke.
	 * If this format is more than total number of args, choke.
	 * Otherwise, return the current argument.
	 */
#define parse_next_arg() { \
	if (argnum > 0) { \
		if (cur_arg > 1) { \
			msg(_("fatal: must use `count$' on all formats or none")); \
			goto out; \
		} \
		arg = the_args[argnum]; \
	} else if (used_dollar) { \
		msg(_("fatal: must use `count$' on all formats or none")); \
		arg = 0; /* shutup the compiler */ \
		goto out; \
	} else if (cur_arg >= num_args) { \
		arg = 0; /* shutup the compiler */ \
		toofew = true; \
		break; \
	} else { \
		arg = the_args[cur_arg]; \
		cur_arg++; \
	} \
}

	need_format = false;
	used_dollar = false;

	s0 = s1 = fmt_string;
	while (n0-- > 0) {
		if (*s1 != '%') {
			s1++;
			continue;
		}
		need_format = true;
		bchunk(s0, s1 - s0);
		s0 = s1;
		cur = &fw;
		fw = 0;
		prec = 0;
		base = 0;
		argnum = 0;
		base = 0;
		have_prec = false;
		signchar = '\0';
		zero_flag = false;
		quote_flag = false;
#ifdef HAVE_MPFR
		mf = NULL;
		zi = NULL;
#endif
		fmt_type = 0;

		lj = alt = big_flag = bigbig_flag = small_flag = false;
		fill = sp;
		cp = cend;
		chbuf = lchbuf;
		s1++;

retry:
		if (n0-- == 0)	/* ran out early! */
			break;

		switch (cs1 = *s1++) {
		case (-1):	/* dummy case to allow for checking */
check_pos:
			if (cur != &fw)
				break;		/* reject as a valid format */
			goto retry;
		case '%':
			need_format = false;
			/*
			 * 29 Oct. 2002:
			 * The C99 standard pages 274 and 279 seem to imply that
			 * since there's no arg converted, the field width doesn't
			 * apply.  The code already was that way, but this
			 * comment documents it, at least in the code.
			 */
			if (do_lint) {
				const char *msg = NULL;

				if (fw && ! have_prec)
					msg = _("field width is ignored for `%%' specifier");
				else if (fw == 0 && have_prec)
					msg = _("precision is ignored for `%%' specifier");
				else if (fw && have_prec)
					msg = _("field width and precision are ignored for `%%' specifier");

				if (msg != NULL)
					lintwarn("%s", msg);
			}
			bchunk_one("%");
			s0 = s1;
			break;

		case '0':
			/*
			 * Only turn on zero_flag if we haven't seen
			 * the field width or precision yet.  Otherwise,
			 * screws up floating point formatting.
			 */
			if (cur == & fw)
				zero_flag = true;
			if (lj)
				goto retry;
			/* FALL through */
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			if (cur == NULL)
				break;
			if (prec >= 0)
				*cur = cs1 - '0';
			/*
			 * with a negative precision *cur is already set
			 * to -1, so it will remain negative, but we have
			 * to "eat" precision digits in any case
			 */
			while (n0 > 0 && *s1 >= '0' && *s1 <= '9') {
				--n0;
				*cur = *cur * 10 + *s1++ - '0';
			}
			if (prec < 0) 	/* negative precision is discarded */
				have_prec = false;
			if (cur == &prec)
				cur = NULL;
			if (n0 == 0)	/* badly formatted control string */
				continue;
			goto retry;
		case '$':
			if (do_traditional) {
				msg(_("fatal: `$' is not permitted in awk formats"));
				goto out;
			}

			if (cur == &fw) {
				argnum = fw;
				fw = 0;
				used_dollar = true;
				if (argnum <= 0) {
					msg(_("fatal: arg count with `$' must be > 0"));
					goto out;
				}
				if (argnum >= num_args) {
					msg(_("fatal: arg count %ld greater than total number of supplied arguments"), argnum);
					goto out;
				}
			} else {
				msg(_("fatal: `$' not permitted after period in format"));
				goto out;
			}

			goto retry;
		case '*':
			if (cur == NULL)
				break;
			if (! do_traditional && isdigit((unsigned char) *s1)) {
				int val = 0;

				for (; n0 > 0 && *s1 && isdigit((unsigned char) *s1); s1++, n0--) {
					val *= 10;
					val += *s1 - '0';
				}
				if (*s1 != '$') {
					msg(_("fatal: no `$' supplied for positional field width or precision"));
					goto out;
				} else {
					s1++;
					n0--;
				}
				if (val >= num_args) {
					toofew = true;
					break;
				}
				arg = the_args[val];
			} else {
				parse_next_arg();
			}
			(void) force_number(arg);
			*cur = get_number_si(arg);
			if (*cur < 0 && cur == &fw) {
				*cur = -*cur;
				lj++;
			}
			if (cur == &prec) {
				if (*cur >= 0)
					have_prec = true;
				else
					have_prec = false;
				cur = NULL;
			}
			goto retry;
		case ' ':		/* print ' ' or '-' */
					/* 'space' flag is ignored */
					/* if '+' already present  */
			if (signchar != false) 
				goto check_pos;
			/* FALL THROUGH */
		case '+':		/* print '+' or '-' */
			signchar = cs1;
			goto check_pos;
		case '-':
			if (prec < 0)
				break;
			if (cur == &prec) {
				prec = -1;
				goto retry;
			}
			fill = sp;      /* if left justified then other */
			lj++; 		/* filling is ignored */
			goto check_pos;
		case '.':
			if (cur != &fw)
				break;
			cur = &prec;
			have_prec = true;
			goto retry;
		case '#':
			alt = true;
			goto check_pos;
		case '\'':
#if defined(HAVE_LOCALE_H)       
			/* allow quote_flag if there is a thousands separator. */
			if (loc.thousands_sep[0] != '\0')
				quote_flag = true;
			goto check_pos;
#else
			goto retry;  
#endif
		case 'l':
			if (big_flag)
				break;
			else {
				static bool warned = false;
				
				if (do_lint && ! warned) {
					lintwarn(_("`l' is meaningless in awk formats; ignored"));
					warned = true;
				}
				if (do_posix) {
					msg(_("fatal: `l' is not permitted in POSIX awk formats"));
					goto out;
				}
			}
			big_flag = true;
			goto retry;
		case 'L':
			if (bigbig_flag)
				break;
			else {
				static bool warned = false;
				
				if (do_lint && ! warned) {
					lintwarn(_("`L' is meaningless in awk formats; ignored"));
					warned = true;
				}
				if (do_posix) {
					msg(_("fatal: `L' is not permitted in POSIX awk formats"));
					goto out;
				}
			}
			bigbig_flag = true;
			goto retry;
		case 'h':
			if (small_flag)
				break;
			else {
				static bool warned = false;
				
				if (do_lint && ! warned) {
					lintwarn(_("`h' is meaningless in awk formats; ignored"));
					warned = true;
				}
				if (do_posix) {
					msg(_("fatal: `h' is not permitted in POSIX awk formats"));
					goto out;
				}
			}
			small_flag = true;
			goto retry;
		case 'c':
			need_format = false;
			parse_next_arg();
			/* user input that looks numeric is numeric */
			if ((arg->flags & (MAYBE_NUM|NUMBER)) == MAYBE_NUM)
				(void) force_number(arg);
			if ((arg->flags & NUMBER) != 0) {
				uval = get_number_uj(arg);
#if MBS_SUPPORT
				if (gawk_mb_cur_max > 1) {
					char buf[100];
					wchar_t wc;
					mbstate_t mbs;
					size_t count;

					memset(& mbs, 0, sizeof(mbs));
					wc = uval;

					count = wcrtomb(buf, wc, & mbs);
					if (count == 0
					    || count == (size_t)-1
					    || count == (size_t)-2)
						goto out0;

					memcpy(cpbuf, buf, count);
					prec = count;
					cp = cpbuf;
					goto pr_tail;
				}
out0:
				;
				/* else,
					fall through */
#endif
				if (do_lint && uval > 255) {
					lintwarn("[s]printf: value %g is too big for %%c format",
							arg->numbr);
				}
				cpbuf[0] = uval;
				prec = 1;
				cp = cpbuf;
				goto pr_tail;
			}
			/*
			 * As per POSIX, only output first character of a
			 * string value.  Thus, we ignore any provided
			 * precision, forcing it to 1.  (Didn't this
			 * used to work? 6/2003.)
			 */
			cp = arg->stptr;
#if MBS_SUPPORT
			/*
			 * First character can be multiple bytes if
			 * it's a multibyte character. Grr.
			 */
			if (gawk_mb_cur_max > 1) {
				mbstate_t state;
				size_t count;

				memset(& state, 0, sizeof(state));
				count = mbrlen(cp, arg->stlen, & state);
				if (count == 0
				    || count == (size_t)-1
				    || count == (size_t)-2)
					goto out2;
				prec = count;
				goto pr_tail;
			}
out2:
			;
#endif
			prec = 1;
			goto pr_tail;
		case 's':
			need_format = false;
			parse_next_arg();
			arg = force_string(arg);
			if (fw == 0 && ! have_prec)
				prec = arg->stlen;
			else {
				char_count = mbc_char_count(arg->stptr, arg->stlen);
				if (! have_prec || prec > char_count)
					prec = char_count;
			}
			cp = arg->stptr;
			goto pr_tail;
		case 'd':
		case 'i':
			need_format = false;
			parse_next_arg();
			(void) force_number(arg);
#ifdef HAVE_MPFR
			if (is_mpfp_float(arg))
				goto mpf0;
			else {
				assert(is_mpfp_integer(arg) == true);
				goto mpz0;
			}
#endif
		case 'X':
			chbuf = Uchbuf;	/* FALL THROUGH */
		case 'x':
			base += 6;	/* FALL THROUGH */
		case 'u':
			base += 2;	/* FALL THROUGH */
		case 'o':
			base += 8;
			need_format = false;
			parse_next_arg();
			(void) force_number(arg);
#ifdef HAVE_MPFR
			if (is_mpfp_integer(arg)) {
mpz0:
				zi = arg->qnumbr;

				if (cs1 != 'd' && cs1 != 'i') {
					if (mpz_sgn(zi) <= 0) {
						/*
						 * Negative value or 0 requires special handling.
						 * Unlike MPFR, GMP does not allow conversion
						 * to (u)intmax_t. So we first convert GMP type to
						 * a MPFR type.
						 */
						mf = mpz2mpfr(zi, _mpfrval);
						goto mpf1;
					}
					signchar = '\0';	/* Don't print '+' */
				}

				/* See comments above about when to fill with zeros */
				zero_flag = (! lj
						    && ((zero_flag && ! have_prec)
							 || (fw == 0 && have_prec)));

 				fmt_type = have_prec ? MP_INT_WITH_PREC : MP_INT_WITHOUT_PREC;
				goto fmt0;

			} else {
				assert(is_mpfp_float(arg) == true);
mpf0:
				mf = arg->qnumbr;
				if (! mpfr_number_p(mf)) {
					/* inf or NaN */
					cs1 = 'g';
					fmt_type = MP_FLOAT;
					goto fmt1;
				}

				if (cs1 != 'd' && cs1 != 'i') {
mpf1:
					/*
					 * The output of printf("%#.0x", 0) is 0 instead of 0x, hence <= in
					 * the comparison below.
					 */
					if (mpfr_sgn(mf) <= 0) {
						if (! mpfr_fits_intmax_p(mf, ROUND_MODE)) {
							/* -ve number is too large */
							cs1 = 'g';
							fmt_type = MP_FLOAT;
							goto fmt1;
						}

						tmpval = uval = (uintmax_t) mpfr_get_sj(mf, ROUND_MODE);
						if (! alt && have_prec && prec == 0 && tmpval == 0)
							goto pr_tail;	/* printf("%.0x", 0) is no characters */
						goto int0;
					}
					signchar = '\0';	/* Don't print '+' */
				}

				/* See comments above about when to fill with zeros */
				zero_flag = (! lj
						    && ((zero_flag && ! have_prec)
							 || (fw == 0 && have_prec)));
				
				(void) mpfr_get_z(_mpzval, mf, MPFR_RNDZ);	/* convert to GMP integer */
 				fmt_type = have_prec ? MP_INT_WITH_PREC : MP_INT_WITHOUT_PREC;
				zi = _mpzval;
				goto fmt0;
			}
#endif

#ifdef HAVE_MPFR
	int0:
#endif
			/*
			 * When to fill with zeroes is of course not simple.
			 * First: No zero fill if left-justifying.
			 * Next: There seem to be two cases:
			 * 	A '0' without a precision, e.g. %06d
			 * 	A precision with no field width, e.g. %.10d
			 * Any other case, we don't want to fill with zeroes.
			 */
			if (! lj
			    && ((zero_flag && ! have_prec)
				 || (fw == 0 && have_prec)))
				fill = zero_string;
			ii = jj = 0;
			do {
				PREPEND(chbuf[uval % base]);
				uval /= base;
#if defined(HAVE_LOCALE_H)
				if (base == 10 && quote_flag && loc.grouping[ii] && ++jj == loc.grouping[ii]) {
					if (uval)	/* only add if more digits coming */
						PREPEND(loc.thousands_sep[0]);	/* XXX --- assumption it's one char */
					if (loc.grouping[ii+1] == 0)                                          
						jj = 0;     /* keep using current val in loc.grouping[ii] */
					else if (loc.grouping[ii+1] == CHAR_MAX)                        
						quote_flag = false;
					else {                 
						ii++;
						jj = 0;
					}
				}
#endif
			} while (uval > 0);

			/* add more output digits to match the precision */
			if (have_prec) {
				while (cend - cp < prec)
					PREPEND('0');
			}

			if (alt && tmpval != 0) {
				if (base == 16) {
					PREPEND(cs1);
					PREPEND('0');
					if (fill != sp) {
						bchunk(cp, 2);
						cp += 2;
						fw -= 2;
					}
				} else if (base == 8)
					PREPEND('0');
			}
			base = 0;
			if (prec > fw)
				fw = prec;
			prec = cend - cp;
	pr_tail:
			if (! lj) {
				while (fw > prec) {
			    		bchunk_one(fill);
					fw--;
				}
			}
			copy_count = prec;
			if (fw == 0 && ! have_prec)
				;
			else if (gawk_mb_cur_max > 1 && (cs1 == 's' || cs1 == 'c')) {
				assert(cp == arg->stptr || cp == cpbuf);
				copy_count = mbc_byte_count(arg->stptr, prec);
			}
			bchunk(cp, copy_count);
			while (fw > prec) {
				bchunk_one(fill);
				fw--;
			}
			s0 = s1;
			break;

     out_of_range:
			/* out of range - emergency use of %g format */
			if (do_lint)
				lintwarn(_("[s]printf: value %g is out of range for `%%%c' format"),
							(double) tmpval, cs1);
			cs1 = 'g';
			goto fmt1;

		case 'F':
#if ! defined(PRINTF_HAS_F_FORMAT) || PRINTF_HAS_F_FORMAT != 1
			cs1 = 'f';
			/* FALL THROUGH */
#endif
		case 'g':
		case 'G':
		case 'e':
		case 'f':
		case 'E':
			need_format = false;
			parse_next_arg();
			(void) force_number(arg);

			if (! is_mpfp_number(arg))
				tmpval = arg->numbr;
#ifdef HAVE_MPFR
			else if (is_mpfp_float(arg)) {
				mf = arg->qnumbr;
				fmt_type = MP_FLOAT;
			} else {
				/* arbitrary-precision integer, convert to MPFR float */
				assert(mf == NULL);
				mf = mpz2mpfr(arg->qnumbr, _mpfrval);
				fmt_type = MP_FLOAT;
			}
#endif
     fmt1:
			if (! have_prec)
				prec = DEFAULT_G_PRECISION;
#ifdef HAVE_MPFR
     fmt0:
#endif
			chksize(fw + prec + 11);	/* 11 == slop */
			cp = cpbuf;
			*cp++ = '%';
			if (lj)
				*cp++ = '-';
			if (signchar)
				*cp++ = signchar;
			if (alt)
				*cp++ = '#';
			if (zero_flag)
				*cp++ = '0';
			if (quote_flag)
				*cp++ = '\'';

#if defined(LC_NUMERIC)
			if (quote_flag && ! use_lc_numeric)
				setlocale(LC_NUMERIC, "");
#endif

			switch (fmt_type) {
#ifdef HAVE_MPFR
			case MP_INT_WITH_PREC:
				sprintf(cp, "*.*Z%c", cs1);
				while ((nc = mpfr_snprintf(obufout, ofre, cpbuf,
					     (int) fw, (int) prec, zi)) >= ofre)
					chksize(nc)
				break;
			case MP_INT_WITHOUT_PREC:
				sprintf(cp, "*Z%c", cs1);
				while ((nc = mpfr_snprintf(obufout, ofre, cpbuf,
					     (int) fw, zi)) >= ofre)
					chksize(nc)
				break;
			case MP_FLOAT:
				sprintf(cp, "*.*R*%c", cs1);
				while ((nc = mpfr_snprintf(obufout, ofre, cpbuf,
					     (int) fw, (int) prec, ROUND_MODE, mf)) >= ofre)
					chksize(nc)
				break;
#endif
			default:
				sprintf(cp, "*.*%c", cs1);
				while ((nc = snprintf(obufout, ofre, cpbuf,
					     (int) fw, (int) prec,
					     (double) tmpval)) >= ofre)
					chksize(nc)
			}

#if defined(LC_NUMERIC)
			if (quote_flag && ! use_lc_numeric)
				setlocale(LC_NUMERIC, "C");
#endif

			len = strlen(obufout);
			ofre -= len;
			obufout += len;
			s0 = s1;
			break;
		default:
			if (do_lint && isalpha(cs1))
				lintwarn(_("ignoring unknown format specifier character `%c': no argument converted"), cs1);
			break;
		}
		if (toofew) {
			msg("%s\n\t`%s'\n\t%*s%s",
			      _("fatal: not enough arguments to satisfy format string"),
			      fmt_string, (int) (s1 - fmt_string - 1), "",
			      _("^ ran out for this one"));
			goto out;
		}
	}
	if (do_lint) {
		if (need_format)
			lintwarn(
			_("[s]printf: format specifier does not have control letter"));
		if (cur_arg < num_args)
			lintwarn(
			_("too many arguments supplied for format string"));
	}
	bchunk(s0, s1 - s0);
	r = make_str_node(obuf, obufout - obuf, ALREADY_MALLOCED);
	obuf = NULL;
out:
	{
		size_t k;
		size_t count = sizeof(cpbufs)/sizeof(cpbufs[0]);
		for (k = 0; k < count; k++) {
			if (cpbufs[k].buf != cpbufs[k].stackbuf)
				efree(cpbufs[k].buf);
		}
		if (obuf != NULL)
			efree(obuf);
	}

	if (r == NULL)
		gawk_exit(EXIT_FATAL);
	return r;
}

#else

static bool mpfp_init(bltin_t **bltins);

numbr_handler_t mpfp_hndlr = {
	mpfp_init,
	NULL,
	NULL,
};

/* mpfp_init --- set up MPFR related variables */

static bool
mpfp_init(bltin_t **bltins)
{
	warning(_("this version of gawk does not support arbitrary-precision numbers"));
	*bltins = NULL;
	return false;
}

#endif
