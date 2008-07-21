/* $MirOS: contrib/hosted/p5/BSD/arc4random/arc4random.c,v 1.3 2008/07/20 14:57:37 tg Exp $ */
/* $miros: contrib/code/Snippets/arc4random.c,v 1.3 2008/03/04 22:53:14 tg Exp $ */

/*-
 * Arc4 random number generator for OpenBSD.
 * Copyright 1996 David Mazieres <dm@lcs.mit.edu>.
 *
 * Modification and redistribution in source and binary forms is
 * permitted provided that due credit is given to the author and the
 * OpenBSD project by leaving this copyright notice intact.
 */

/*-
 * This code is derived from section 17.1 of Applied Cryptography,
 * second edition, which describes a stream cipher allegedly
 * compatible with RSA Labs "RC4" cipher (the actual description of
 * which is a trade secret).  The same algorithm is used as a stream
 * cipher called "arcfour" in Tatu Ylonen's ssh package.
 *
 * Here the stream cipher has been modified always to include the time
 * when initializing the state.  That makes it impossible to
 * regenerate the same random sequence twice, so this can't be used
 * for encryption, but will generate good random numbers.
 *
 * RC4 is a registered trademark of RSA Laboratories.
 */

/*-
 * Modified by Robert Connolly from OpenBSD lib/libc/crypt/arc4random.c v1.11.
 * This is arc4random(3) using urandom.
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/time.h>
#if HAVE_SYS_SYSCTL_H
#include <sys/sysctl.h>
#endif
#include <fcntl.h>
#if HAVE_STDINT_H
#include <stdint.h>
#elif defined(USE_INTTYPES)
#include <inttypes.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef REDEF_USCORETYPES
#define u_int32_t	uint32_t
#endif

struct arc4_stream {
	uint8_t i;
	uint8_t j;
	uint8_t s[256];
};

static int rs_initialized;
static struct arc4_stream rs;
static pid_t arc4_stir_pid;
static int arc4_count;

static uint8_t arc4_getbyte(struct arc4_stream *);

u_int32_t arc4random(void);
void arc4random_addrandom(u_char *, int);
void arc4random_stir(void);

static void
arc4_init(struct arc4_stream *as)
{
	int     n;

	for (n = 0; n < 256; n++)
		as->s[n] = n;
	as->i = 0;
	as->j = 0;
}

static void
arc4_addrandom(struct arc4_stream *as, u_char *dat, int datlen)
{
	int     n;
	uint8_t si;

	as->i--;
	for (n = 0; n < 256; n++) {
		as->i = (as->i + 1);
		si = as->s[as->i];
		as->j = (as->j + si + dat[n % datlen]);
		as->s[as->i] = as->s[as->j];
		as->s[as->j] = si;
	}
	as->j = as->i;
}

static void
arc4_stir(struct arc4_stream *as)
{
	int     n, fd;
	struct {
		struct timeval tv;
		pid_t pid;
		u_int rnd[(128 - (sizeof (struct timeval) + sizeof (pid_t))) / sizeof(u_int)];
	} rdat;
	size_t sz = 0;

	gettimeofday(&rdat.tv, NULL);

	/* /dev/urandom is a multithread interface, sysctl is not. */
	/* Try to use /dev/urandom before sysctl. */
	fd = open("/dev/urandom", O_RDONLY);
	if (fd != -1) {
		sz = (size_t)read(fd, rdat.rnd, sizeof (rdat.rnd));
		close(fd);
	}
	if (sz > sizeof (rdat.rnd))
		sz = 0;
	if (fd == -1 || sz != sizeof (rdat.rnd)) {
		/* /dev/urandom failed? Maybe we're in a chroot. */
//#if defined(CTL_KERN) && defined(KERN_RANDOM) && defined(RANDOM_UUID)
#ifdef _LINUX_SYSCTL_H
		/* XXX this is for Linux, which uses enums */

		int mib[3];
		size_t i = sz / sizeof (u_int), len;

		mib[0] = CTL_KERN;
		mib[1] = KERN_RANDOM;
		mib[2] = RANDOM_UUID;

		while (i < sizeof (rdat.rnd) / sizeof (u_int)) {
			len = sizeof(u_int);
			if (sysctl(mib, 3, &rdat.rnd[i++], &len, NULL, 0) == -1) {
				fprintf(stderr, "warning: no entropy source\n");
				break;
			}
		}
#else
		/* XXX kFreeBSD doesn't seem to have KERN_ARND or so */
		;
#endif
	}

	rdat.pid = arc4_stir_pid = getpid();
	/*
	 * Time to give up. If no entropy could be found then we will just
	 * use gettimeofday.
	 */
	arc4_addrandom(as, (void *)&rdat, sizeof(rdat));

	/*
	 * Discard early keystream, as per recommendations in:
	 * http://www.wisdom.weizmann.ac.il/~itsik/RC4/Papers/Rc4_ksa.ps
	 * We discard 256 words. A long word is 4 bytes.
	 */
	for (n = 0; n < 256 * 4; n ++)
		arc4_getbyte(as);

	/* discard by a randomly fuzzed factor as well */
	n = (arc4_getbyte(as) & 0x0F) + 1;
	while (n--)
		arc4_getbyte(as);

	/* take a chance to write back to the kernel (works e.g. on Cygwin) */
	if ((fd = open("/dev/urandom", O_WRONLY)) != -1) {
		for (n = 0; n < 16; ++n)
			rdat.rnd[n] = arc4_getbyte(as);
		write(fd, rdat.rnd, 16);
		close(fd);
		arc4_getbyte(as); /* discard one */
	}

	arc4_count = 400000;
}

static uint8_t
arc4_getbyte(struct arc4_stream *as)
{
	uint8_t si, sj;

	as->i = (as->i + 1);
	si = as->s[as->i];
	as->j = (as->j + si);
	sj = as->s[as->j];
	as->s[as->i] = sj;
	as->s[as->j] = si;
	return (as->s[(si + sj) & 0xff]);
}

static uint32_t
arc4_getword(struct arc4_stream *as)
{
	uint32_t val;
	val = arc4_getbyte(as) << 24;
	val |= arc4_getbyte(as) << 16;
	val |= arc4_getbyte(as) << 8;
	val |= arc4_getbyte(as);
	return val;
}

void
arc4random_stir(void)
{
	if (!rs_initialized) {
		arc4_init(&rs);
		rs_initialized = 1;
	}
	arc4_stir(&rs);
}

void
arc4random_addrandom(u_char *dat, int datlen)
{
	if (!rs_initialized)
		arc4random_stir();
	arc4_addrandom(&rs, dat, datlen);
}

u_int32_t
arc4random(void)
{
	if (--arc4_count == 0 || !rs_initialized || arc4_stir_pid != getpid())
		arc4random_stir();
	return arc4_getword(&rs);
}
