/*
   Copyright (C) 2006-2016, 2021 Con Kolivas
   Copyright (C) 2011 Serge Belyshev
   Copyright (C) 2008, 2011, 2019, 2020, 2021 Peter Hyman
   Copyright (C) 1998 Andrew Tridgell

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

/*
  Utilities used in rzip

  tridge, June 1996
  */

/*
 * Realloc removed
 * Functions added
 *    read_config()
 * Peter Hyman, December 2008
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdarg.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#include <termios.h>

#ifdef _SC_PAGE_SIZE
# define PAGE_SIZE (sysconf(_SC_PAGE_SIZE))
#else
# define PAGE_SIZE (4096)
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include "lrzip_private.h"
#include "util.h"
#include <gcrypt.h>
#ifdef HAVE_CTYPE_H
# include <ctype.h>
#endif

/* Macros for testing parameters */
#define isparameter( parmstring, value )	(!strcasecmp( parmstring, value ))
#define iscaseparameter( parmvalue, value )	(!strcmp( parmvalue, value ))

void register_infile(rzip_control *control, const char *name, char delete)
{
	control->util_infile = name;
	control->delete_infile = delete;
}

void register_outfile(rzip_control *control, const char *name, char delete)
{
	control->util_outfile = name;
	control->delete_outfile = delete;
}

void register_outputfile(rzip_control *control, FILE *f)
{
	control->outputfile = f;
}

void unlink_files(rzip_control *control)
{
	/* Delete temporary files generated for testing or faking stdio */
	if (control->util_outfile && control->delete_outfile)
		unlink(control->util_outfile);

	if (control->util_infile && control->delete_infile)
		unlink(control->util_infile);
}

void fatal_exit(rzip_control *control)
{
	struct termios termios_p;

	/* Make sure we haven't died after disabling stdin echo */
	tcgetattr(fileno(stdin), &termios_p);
	termios_p.c_lflag |= ECHO;
	tcsetattr(fileno(stdin), 0, &termios_p);

	unlink_files(control);
	if (!STDOUT && !TEST_ONLY && control->outfile) {
		if (!KEEP_BROKEN) {
			print_verbose("Deleting broken file %s\n", control->outfile);
			unlink(control->outfile);
		} else
			print_verbose("Keeping broken file %s as requested\n", control->outfile);
	}
	fprintf(control->outputfile, "Fatal error - exiting\n");
	fflush(control->outputfile);
	exit(1);
}

void setup_overhead(rzip_control *control)
{
	/* Work out the compression overhead per compression thread for the
	 * compression back-ends that need a lot of ram
	 * and set Dictionary size */
	if (LZMA_COMPRESS) {
		if (control->dictSize == 0) {
			switch (control->compression_level) {
			case 1:
			case 2:
			case 3:
			case 4:
			case 5: control->dictSize = (1 << (control->compression_level * 2 + 14));
				break; // 65KB to 16MB
			case 6:
			case 7: control->dictSize = (1 << 25);
				break; // 32MB
			case 8: control->dictSize = (1 << 26);
				break; // 64MB
			case 9: control->dictSize = (1 << 27);
				break; // 128MB -- this is maximum for 32 bits
			default: control->dictSize = (1 << 24);
				break; // 16MB -- should never reach here
			}
		}
		/* LZMA spec shows memory requirements as 6MB, not 4MB and state size
		 * where default is 16KB */
		control->overhead = ((i64)control->dictSize * 23 / 2) + (6 * 1024 * 1024) + 16384;
	} else if (ZPAQ_COMPRESS) {
		if (control->zpaq_bs == 0) {
			control->zpaq_level = control->compression_level /4 + 3;	/* only use levels 3,4 and 5 */
			switch (control->compression_level) {
			case 1:
			case 2:
			case 3:
			case 4:	control->zpaq_bs = 6;
				break;	//64MB
			case 6:	control->zpaq_bs = 7;
				break;	//128MB
			case 7:	control->zpaq_bs = 9;
				break;	//512MB
			case 8:	control->zpaq_bs = 10;
				break;	//1GB
			case 9:	control->zpaq_bs = 11;
				break;	//2GB
			default: control->zpaq_bs = 6;
				break;	// should never reach here
			}
		}
		control->overhead = (i64) (1 << control->zpaq_bs) * 1024 * 1024;	// times 8 or 16. Out for now
	}

	/* no need for zpaq computation here. do in open_stream_out() */
}

void setup_ram(rzip_control *control)
{
	/* Use less ram when using STDOUT to store the temporary output file. */
	if (STDOUT && ((STDIN && DECOMPRESS) || !(DECOMPRESS || TEST_ONLY)))
		control->maxram = control->ramsize / 6;
	else
		control->maxram = control->ramsize / 3;
	if (BITS32) {
		/* Decrease usable ram size on 32 bits due to kernel /
		 * userspace split. Cannot allocate larger than a 1
		 * gigabyte chunk due to 32 bit signed long being
		 * used in alloc, and at most 3GB can be malloced, and
		 * 2/3 of that makes for a total of 2GB to be split
		 * into thirds.
		 */
		control->usable_ram = MAX(control->ramsize - 900000000ll, 900000000ll);
		control->maxram = MIN(control->maxram, control->usable_ram);
		control->maxram = MIN(control->maxram, one_g * 2 / 3);
	} else
		control->usable_ram = control->maxram;
	round_to_page(&control->maxram);
}

void round_to_page(i64 *size)
{
	*size -= *size % PAGE_SIZE;
	if (unlikely(!*size))
		*size = PAGE_SIZE;
}

size_t round_up_page(rzip_control *control, size_t len)
{
	int rem = len % control->page_size;

	if (rem)
		len += control->page_size - rem;
	return len;
}

bool get_rand(rzip_control *control, uchar *buf, int len)
{
	int fd, i;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd == -1) {
		for (i = 0; i < len; i++)
			buf[i] = (uchar)random();
	} else {
		if (unlikely(read(fd, buf, len) != len))
			fatal_return(("Failed to read fd in get_rand\n"), false);
		if (unlikely(close(fd)))
			fatal_return(("Failed to close fd in get_rand\n"), false);
	}
	return true;
}

bool read_config(rzip_control *control)
{
	/* check for lrzip.conf in ., $HOME/.lrzip and /etc/lrzip */
	char *HOME, homeconf[255];
	char *parametervalue;
	char *parameter;
	char line[255];
	FILE *fp;

	fp = fopen("lrzip.conf", "r");
	if (fp)
		fprintf(control->msgout, "Using configuration file ./lrzip.conf\n");
	if (fp == NULL) {
		HOME=getenv("HOME");
		if (HOME) {
			snprintf(homeconf, sizeof(homeconf), "%s/.lrzip/lrzip.conf", HOME);
			fp = fopen(homeconf, "r");
			if (fp)
				fprintf(control->msgout, "Using configuration file %s\n", homeconf);
		}
	}
	if (fp == NULL) {
		fp = fopen("/etc/lrzip/lrzip.conf", "r");
		if (fp)
			fprintf(control->msgout, "Using configuration file /etc/lrzip/lrzip.conf\n");
	}
	if (fp == NULL)
		return false;

	/* if we get here, we have a file. read until no more. */

	while ((fgets(line, 255, fp)) != NULL) {
		if (strlen(line))
			line[strlen(line) - 1] = '\0';
		parameter = strtok(line, " =");
		if (parameter == NULL)
			continue;
		/* skip if whitespace or # */
		if (isspace(*parameter))
			continue;
		if (*parameter == '#')
			continue;

		parametervalue = strtok(NULL, " =");
		if (parametervalue == NULL)
			continue;

		/* have valid parameter line, now assign to control */

		if (isparameter(parameter, "window")) {
			control->window = atoi(parametervalue);
		}
		else if (isparameter(parameter, "unlimited")) {
			if (isparameter(parametervalue, "yes"))
				control->flags |= FLAG_UNLIMITED;
		}
		else if (isparameter(parameter, "compressionlevel")) {
			control->compression_level = atoi(parametervalue);
			if ( control->compression_level < 1 || control->compression_level > 9 )
				failure_return(("CONF.FILE error. Compression Level must between 1 and 9"), false);
		}
		else if (isparameter(parameter, "rziplevel")) {
			control->rzip_compression_level = atoi(parametervalue);
			if ( control->rzip_compression_level < 1 || control->rzip_compression_level > 9 )
				failure_return(("CONF.FILE error. RZIP Compression Level must between 1 and 9"), false);
		}
		else if (isparameter(parameter, "compressionmethod")) {
			/* valid are rzip, gzip, bzip2, lzo, lzma (default), and zpaq */
			if (control->flags & FLAG_NOT_LZMA)
				failure_return(("CONF.FILE error. Can only specify one compression method"), false);
			if (isparameter(parametervalue, "bzip2"))
				control->flags |= FLAG_BZIP2_COMPRESS;
			else if (isparameter(parametervalue, "gzip"))
				control->flags |= FLAG_ZLIB_COMPRESS;
			else if (isparameter(parametervalue, "lzo"))
				control->flags |= FLAG_LZO_COMPRESS;
			else if (isparameter(parametervalue, "rzip"))
				control->flags |= FLAG_NO_COMPRESS;
			else if (isparameter(parametervalue, "zpaq"))
				control->flags |= FLAG_ZPAQ_COMPRESS;
			else if (!isparameter(parametervalue, "lzma")) /* oops, not lzma! */
				failure_return(("CONF.FILE error. Invalid compression method %s specified\n",parametervalue), false);
		}
		else if (isparameter(parameter, "lzotest")) {
			/* default is yes */
			if (isparameter(parametervalue, "no"))
				control->flags &= ~FLAG_THRESHOLD;
		}
		else if (isparameter(parameter, "threshold")) {
			/* default is 100 */
			control->threshold = atoi(parametervalue);
			if (control->threshold < 1 || control->threshold > 99)
				failure_return(("CONF.FILE error. LZO Threshold must be between 1 and 99"), false);
		}
		else if (isparameter(parameter, "hashcheck")) {
			if (isparameter(parametervalue, "yes")) {
				control->flags |= FLAG_CHECK;
				control->flags |= FLAG_HASH;
			}
		}
		else if (isparameter(parameter, "showhash")) {
			if (isparameter(parametervalue, "yes"))
				control->flags |= FLAG_HASH;
		}
		else if (isparameter(parameter, "outputdirectory")) {
			control->outdir = malloc(strlen(parametervalue) + 2);
			if (!control->outdir)
				fatal_return(("Fatal Memory Error in read_config"), false);
			strcpy(control->outdir, parametervalue);
			if (strcmp(parametervalue + strlen(parametervalue) - 1, "/"))
				strcat(control->outdir, "/");
		}
		else if (isparameter(parameter,"verbosity")) {
			if (control->flags & FLAG_VERBOSE)
				failure_return(("CONF.FILE error. Verbosity already defined."), false);
			if (isparameter(parametervalue, "yes"))
				control->flags |= FLAG_VERBOSITY;
			else if (isparameter(parametervalue,"max"))
				control->flags |= FLAG_VERBOSITY_MAX;
			else /* oops, unrecognized value */
				print_err("lrzip.conf: Unrecognized verbosity value %s. Ignored.\n", parametervalue);
		}
		else if (isparameter(parameter, "showprogress")) {
			/* Yes by default */
			if (isparameter(parametervalue, "NO"))
				control->flags &= ~FLAG_SHOW_PROGRESS;
		}
		else if (isparameter(parameter,"nice")) {
			control->nice_val = atoi(parametervalue);
			if (control->nice_val < -20 || control->nice_val > 19)
				failure_return(("CONF.FILE error. Nice must be between -20 and 19"), false);
		}
		else if (isparameter(parameter, "keepbroken")) {
			if (isparameter(parametervalue, "yes" ))
				control->flags |= FLAG_KEEP_BROKEN;
		}
		else if (iscaseparameter(parameter, "DELETEFILES")) {
			/* delete files must be case sensitive */
			if (iscaseparameter(parametervalue, "YES"))
				control->flags &= ~FLAG_KEEP_FILES;
		}
		else if (iscaseparameter(parameter, "REPLACEFILE")) {
			/* replace lrzip file must be case sensitive */
			if (iscaseparameter(parametervalue, "YES"))
				control->flags |= FLAG_FORCE_REPLACE;
		}
		else if (isparameter(parameter, "tmpdir")) {
			control->tmpdir = realloc(NULL, strlen(parametervalue) + 2);
			if (!control->tmpdir)
				fatal_return(("Fatal Memory Error in read_config"), false);
			strcpy(control->tmpdir, parametervalue);
			if (strcmp(parametervalue + strlen(parametervalue) - 1, "/"))
				strcat(control->tmpdir, "/");
		}
		else if (isparameter(parameter, "encrypt")) {
			if (isparameter(parameter, "YES"))
				control->flags |= FLAG_ENCRYPT;
		}
		else if (isparameter(parameter, "dictionarysize")) {
			int p;
			p = atoi(parametervalue);
			if (p < 0 || p > 40)
				failure_return(("CONF FILE error. Dictionary Size must be between 0 and 40."), false);
			control->dictSize = ((p == 40) ? 0xFFFFFFFF : ((2 | ((p) & 1)) << ((p) / 2 + 11)));	// Slight modification to lzma2 spec 2^31 OK
		}
		else {
			/* oops, we have an invalid parameter, display */
			print_err("lrzip.conf: Unrecognized parameter value, %s = %s. Continuing.\n",\
			       parameter, parametervalue);
		}
	}

	if (unlikely(fclose(fp)))
		fatal_return(("Failed to fclose fp in read_config\n"), false);

/*	fprintf(stderr, "\nWindow = %d \
		\nCompression Level = %d \
		\nThreshold = %1.2f \
		\nOutput Directory = %s \
		\nFlags = %d\n", control->window,control->compression_level, control->threshold, control->outdir, control->flags);
*/
	return true;
}

static void xor128 (void *pa, const void *pb)
{
	i64 *a = pa;
	const i64 *b = pb;

	a [0] ^= b [0];
	a [1] ^= b [1];
}

static void lrz_keygen(const rzip_control *control, const uchar *salt, uchar *key, uchar *iv)
{
	uchar buf [HASH_LEN + SALT_LEN + PASS_LEN];
	gcry_md_hd_t gcry_sha512_handle;

	mlock(buf, HASH_LEN + SALT_LEN + PASS_LEN);

	memcpy(buf, control->hash, HASH_LEN);
	memcpy(buf + HASH_LEN, salt, SALT_LEN);
	memcpy(buf + HASH_LEN + SALT_LEN, control->salt_pass, control->salt_pass_len);

	/* No error checking for gcrypt key/iv hash functions */
	gcry_md_open(&gcry_sha512_handle, GCRY_MD_SHA512, GCRY_MD_FLAG_SECURE);
	gcry_md_write(gcry_sha512_handle, buf, HASH_LEN + SALT_LEN + control->salt_pass_len);
	memcpy(key, gcry_md_read(gcry_sha512_handle, GCRY_MD_SHA512), HASH_LEN);

	gcry_md_reset(gcry_sha512_handle);

	memcpy(buf, key, HASH_LEN);
	memcpy(buf + HASH_LEN, salt, SALT_LEN);
	memcpy(buf + HASH_LEN + SALT_LEN, control->salt_pass, control->salt_pass_len);

	gcry_md_write(gcry_sha512_handle, buf, HASH_LEN + SALT_LEN + control->salt_pass_len);
	memcpy(iv, gcry_md_read(gcry_sha512_handle, GCRY_MD_SHA512), HASH_LEN);
	gcry_md_close(gcry_sha512_handle);

	memset(buf, 0, sizeof(buf));
	munlock(buf, sizeof(buf));
}

bool lrz_crypt(const rzip_control *control, uchar *buf, i64 len, const uchar *salt, int encrypt)
{
	/* libgcrypt using cipher text stealing simplifies matters */
	uchar key[HASH_LEN], iv[HASH_LEN];
	uchar tmp0[CBC_LEN], tmp1[CBC_LEN];
	gcry_cipher_hd_t gcry_aes_cbc_handle;
	bool ret = false;
	size_t gcry_error;
	int M, N;

	/* Generate unique key and IV for each block of data based on salt */
	mlock(key, HASH_LEN);
	mlock(iv, HASH_LEN);

	M=len % CBC_LEN;
	N=len - M;

	lrz_keygen(control, salt, key, iv);

	/* Using libgcrypt and CBC/ECB methods. While we could use CTS mode to encrypt/decrypt
	 * entrire buffer in one pass, this will preserve compatibility with older versions of
	 * lrzip/lrzip-next.
	 * Error checking may be superfluous, but inserted for clarity and proper coding standard.
	 */
	gcry_error=gcry_cipher_open(&gcry_aes_cbc_handle, GCRY_CIPHER_AES, GCRY_CIPHER_MODE_CBC, GCRY_CIPHER_SECURE);
	if (unlikely(gcry_error))
		failure_goto(("Unable to set AES CBC handle in lrz_crypt: %d\n", gcry_error), error);
	gcry_error=gcry_cipher_setkey(gcry_aes_cbc_handle, key, CBC_LEN);
	if (unlikely(gcry_error))
		failure_goto(("Failed to set AES CBC key in lrz_crypt: %d\n", gcry_error), error);
	gcry_error=gcry_cipher_setiv(gcry_aes_cbc_handle, iv, CBC_LEN);
	if (unlikely(gcry_error))
		failure_goto(("Failed to set AES CBC iv in lrz_crypt: %d\n", gcry_error), error);

	if (encrypt == LRZ_ENCRYPT) {
		print_maxverbose("Encrypting data        \n");
		/* Encrypt whole buffer */
		gcry_error=gcry_cipher_encrypt(gcry_aes_cbc_handle, buf, N, NULL, 0);
		if (unlikely(gcry_error))
			failure_goto(("Failed to encrypt AES CBC data in lrz_crypt: %d\n", gcry_error), error);
		if (M) {
			memset(tmp0, 0, CBC_LEN);
			memcpy(tmp0, buf + N, M);
			gcry_error=gcry_cipher_encrypt(gcry_aes_cbc_handle, tmp1, CBC_LEN, tmp0, CBC_LEN);
			if (unlikely(gcry_error))
				failure_goto(("Failed to encrypt AES CBC data in lrz_crypt: %d\n", gcry_error), error);
			memcpy(buf + N, buf + N - CBC_LEN, M);
			memcpy(buf + N - CBC_LEN, tmp1, CBC_LEN);
		}
	} else { //LRZ_DECRYPT or LRZ_VALIDATE
		if (encrypt == LRZ_DECRYPT)	// don't print if validating or in info
			print_maxverbose("Decrypting data        \n");
		if (M) {
			gcry_cipher_hd_t gcry_aes_ecb_handle;
			gcry_error=gcry_cipher_open(&gcry_aes_ecb_handle, GCRY_CIPHER_AES, GCRY_CIPHER_MODE_ECB, GCRY_CIPHER_SECURE);
			if (unlikely(gcry_error))
				failure_goto(("Unable to set AES ECB handle in lrz_crypt: %d\n", gcry_error), error);
			gcry_error=gcry_cipher_setkey(gcry_aes_ecb_handle,key,CBC_LEN);
			if (unlikely(gcry_error))
				failure_goto(("Failed to set AES ECB key in lrz_crypt: %d\n", gcry_error), error);
			gcry_error=gcry_cipher_decrypt(gcry_aes_cbc_handle, buf, N-CBC_LEN, NULL, 0);
			if (unlikely(gcry_error))
				failure_goto(("Failed to decrypt AES CBC data in lrz_crypt: %d\n", gcry_error), error);
			gcry_error=gcry_cipher_decrypt(gcry_aes_ecb_handle, tmp0, CBC_LEN, buf+N-CBC_LEN, CBC_LEN);
			if (unlikely(gcry_error))
				failure_goto(("Failed to decrypt AES ECB data in lrz_crypt: %d\n", gcry_error), error);
			memset(tmp1, 0, CBC_LEN);
			memcpy(tmp1, buf + N, M);
			xor128(tmp0, tmp1);
			memcpy(buf + N, tmp0, M);
			memcpy(tmp1 + M, tmp0 + M, CBC_LEN - M);
			gcry_error=gcry_cipher_decrypt(gcry_aes_cbc_handle, buf+N-CBC_LEN, CBC_LEN, tmp1, CBC_LEN);
			if (unlikely(gcry_error))
				failure_goto(("Failed to decrypt final AES CBC block in lrz_crypt: %d\n", gcry_error), error);
			gcry_cipher_close(gcry_aes_ecb_handle);
		} else {
			/* Decrypt whole buffer */
			gcry_error=gcry_cipher_decrypt(gcry_aes_cbc_handle, buf, len, NULL, 0);
			if (unlikely(gcry_error))
				failure_goto(("Failed to decrypt AES CBC data in lrz_crypt: %d\n", gcry_error), error);
		}
	}
	gcry_cipher_close(gcry_aes_cbc_handle);

	ret = true;
error:
	memset(iv, 0, HASH_LEN);
	memset(key, 0, HASH_LEN);
	munlock(iv, HASH_LEN);
	munlock(key, HASH_LEN);
	return ret;
}

void lrz_stretch(rzip_control *control)
{
	gcry_md_hd_t gcry_sha512_handle;
	i64 j, n, counter;

	gcry_md_open(&gcry_sha512_handle, GCRY_MD_SHA512, GCRY_MD_FLAG_SECURE);

	n = control->encloops * HASH_LEN / (control->salt_pass_len + sizeof(i64));
	print_maxverbose("Hashing passphrase %lld (%lld) times \n", control->encloops, n);
	for (j = 0; j < n; j ++) {
		counter = htole64(j);
		gcry_md_write(gcry_sha512_handle, (uchar *)&counter, sizeof(counter));
		gcry_md_write(gcry_sha512_handle, control->salt_pass, control->salt_pass_len);
	}
}

/* The block headers are all encrypted so we read the data and salt associated
 * with them, decrypt the data, then return the decrypted version of the
 * values */
bool decrypt_header(rzip_control *control, uchar *head, uchar *c_type,
			   i64 *c_len, i64 *u_len, i64 *last_head, int dec_or_validate)
{
	uchar *buf = head + SALT_LEN;

	memcpy(buf, c_type, 1);
	memcpy(buf + 1, c_len, 8);
	memcpy(buf + 9, u_len, 8);
	memcpy(buf + 17, last_head, 8);

	if (unlikely(!lrz_decrypt(control, buf, 25, head, dec_or_validate)))
		return false;

	memcpy(c_type, buf, 1);
	memcpy(c_len, buf + 1, 8);
	memcpy(u_len, buf + 9, 8);
	memcpy(last_head, buf + 17, 8);
	return true;
}
