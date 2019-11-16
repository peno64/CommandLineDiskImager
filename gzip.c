// gzip code from http://ftp.gnu.org/gnu/gzip
// with some minor modifications

#include <stdio.h>
#include <stdlib.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

#include "gzip.h"
#include "lzw.h"
#include "tailor.h"

unsigned outcnt;		/* bytes in output buffer */
int level = 9;			/* compression level */
unsigned insize;           /* valid bytes in inbuf */
unsigned inptr;            /* index of next byte to be processed in inbuf */
long bytes_in;             /* number of input bytes */
long bytes_out;            /* number of output bytes */
int  ifd;                  /* input file descriptor */
int  ofd;                  /* output file descriptor */
char ifname[256];		   /* input file name */
char ofname[256];		   /* output file name */
int rsync = 0;             /* make ryncable chunks */
char *program_name = "CommandLineDiskImager";   /* program name */
off_t ifile_size;      /* input file size, -1 for devices (debug only) */

int exit_code = OK;   /* program exit code */
int quiet = 0;        /* be very quiet (-q) */
int test = 0;         /* test .gz file integrity */
int force = 0;        /* don't ask questions, compress links (-f) */
int part_nb;          /* number of parts in .gz file */
int last_member;      /* set for .zip and .Z files */
int no_time = 1;     /* don't save or restore the original file time */
int no_name = 1;     /* don't save or restore the original file name */
int list = 0;         /* list the file contents (-l) */
int verbose = 0;      /* be verbose (-v) */
int maxbits = BITS;   /* max bits per code for LZW */

int method = DEFLATED;/* compression method */

struct timespec time_stamp;       /* original time stamp (modification time) */

int save_orig_name;   /* set if original name must be saved */

int to_stdout = 0;    /* output to stdout (-c) */

int(*work) (int infile, int outfile) = zip; /* function to call */

/* global buffers*/

DECLARE(uch, inbuf,  INBUFSIZ +INBUF_EXTRA);
DECLARE(uch, outbuf, OUTBUFSIZ+OUTBUF_EXTRA);
DECLARE(ush, d_buf,  DIST_BUFSIZE);
DECLARE(uch, window, 2L*WSIZE);
#ifndef MAXSEG_64K
    DECLARE(ush, tab_prefix, 1L<<BITS);
#else
    DECLARE(ush, tab_prefix0, 1L<<(BITS-1));
    DECLARE(ush, tab_prefix1, 1L<<(BITS-1));
#endif

enum { TIMESPEC_HZ = 1000000000 };

enum { TIMESPEC_RESOLUTION = TIMESPEC_HZ };

#define TYPE_SIGNED(t) (! ((t) 0 < (t) -1))
#define TYPE_WIDTH(t) (sizeof (t) * CHAR_BIT)

#define TYPE_MAXIMUM(t)                                                 \
  ((t) (! TYPE_SIGNED (t)                                               \
        ? (t) -1                                                        \
        : ((((t) 1 << (TYPE_WIDTH (t) - 2)) - 1) * 2 + 1)))

#ifndef STDOUT_FILENO
# define STDOUT_FILENO 1
#endif

/* ========================================================================
 * Free all dynamically allocated variables and exit with the given code.
 */
void do_exit(exitcode)
    int exitcode;
{
    FREE(inbuf);
    FREE(outbuf);
    FREE(d_buf);
# ifndef MAXSEG_64K
    FREE(tab_prefix);
# else
    FREE(tab_prefix0);
    FREE(tab_prefix1);
# endif
    exit(exitcode);
}
		
/* ========================================================================
 * Signal and error handler.
 */
RETSIGTYPE abort_gzip()
{
   do_exit(ERROR);
}

void initgzip()
{
	clear_bufs();

	part_nb = 0;
	last_member = 0;

	ifile_size = -1;
	time_stamp.tv_nsec = -1;
}

/* Return the nanosecond component of *ST's data modification time.  */
static long int 
get_stat_mtime_ns(struct stat const *st)
{
# if defined STAT_TIMESPEC
	return STAT_TIMESPEC(st, st_mtim).tv_nsec;
# elif defined STAT_TIMESPEC_NS
	return STAT_TIMESPEC_NS(st, st_mtim);
# else
	return 0;
# endif
}

/* Return *ST's data modification time.  */
static struct timespec
get_stat_mtime(struct stat const *st)
{
#ifdef STAT_TIMESPEC
	return STAT_TIMESPEC(st, st_mtim);
#else
	struct timespec t;
	t.tv_sec = st->st_mtime;
	t.tv_nsec = get_stat_mtime_ns(st);
	return t;
#endif
}

static void
get_input_size_and_time(int fp)
{
	struct stat istat;         /* status for input file */

	ifile_size = -1;
	time_stamp.tv_nsec = -1;

	if (fstat(fp, &istat) != 0)
	{
		/* Record the input file's size and timestamp only if it is a
		regular file.  Doing this for the timestamp helps to keep gzip's
		output more reproducible when it is used as part of a
		pipeline.  */

		ifile_size = istat.st_size;
		time_stamp = get_stat_mtime(&istat);
	}
}

/* Discard NBYTES input bytes from the input, or up through the next
zero byte if NBYTES == (size_t) -1.  If FLAGS say that the header
CRC should be computed, update the CRC accordingly.  */
static void
discard_input_bytes(nbytes, flags)
size_t nbytes;
unsigned int flags;
{
	while (nbytes != 0)
	{
		uch c = get_byte();
		if (flags & HEADER_CRC)
			updcrc(&c, 1);
		if (nbytes != (size_t)-1)
			nbytes--;
		else if (!c)
			break;
	}
}


/* ========================================================================
* Check the magic number of the input file and update ofname if an
* original name was given and to_stdout is not set.
* Return the compression method, -1 for error, -2 for warning.
* Set inptr to the offset of the next byte to be processed.
* Updates time_stamp if there is one and neither -m nor -n is used.
* This function may be called repeatedly for an input file consisting
* of several contiguous gzip'ed members.
* IN assertions: there is at least one remaining compressed member.
*   If the member is a zip file, it must be the only one.
*/
int get_method(in)
int in;        /* input file descriptor */
{
	uch flags;     /* compression flags */
	uch magic[10]; /* magic header */
	int imagic0;   /* first magic byte or EOF */
	int imagic1;   /* like magic[1], but can represent EOF */
	ulg stamp;     /* timestamp */

				   /* If --force and --stdout, zcat == cat, so do not complain about
				   * premature end of file: use try_byte instead of get_byte.
				   */
	if (force && to_stdout) {
		imagic0 = try_byte();
		magic[0] = imagic0;
		imagic1 = try_byte();
		magic[1] = imagic1;
		/* If try_byte returned EOF, magic[1] == (char) EOF.  */
	}
	else {
		magic[0] = get_byte();
		imagic0 = 0;
		if (magic[0]) {
			magic[1] = get_byte();
			imagic1 = 0; /* avoid lint warning */
		}
		else {
			imagic1 = try_byte();
			magic[1] = imagic1;
		}
	}
	method = -1;                 /* unknown yet */
	part_nb++;                   /* number of parts in gzip file */
	header_bytes = 0;
	last_member = 0;
	/* assume multiple members in gzip file except for record oriented I/O */

	if (memcmp(magic, GZIP_MAGIC, 2) == 0
		|| memcmp(magic, OLD_GZIP_MAGIC, 2) == 0) {

		method = (int)get_byte();
		if (method != DEFLATED) {
			fprintf(stderr,
				"%s: %s: unknown method %d -- not supported\n",
				program_name, ifname, method);
			exit_code = ERROR;
			return -1;
		}
		work = unzip;
		flags = (uch)get_byte();

		if ((flags & ENCRYPTED) != 0) {
			fprintf(stderr,
				"%s: %s is encrypted -- not supported\n",
				program_name, ifname);
			exit_code = ERROR;
			return -1;
		}
		if ((flags & RESERVED) != 0) {
			fprintf(stderr,
				"%s: %s has flags 0x%x -- not supported\n",
				program_name, ifname, flags);
			exit_code = ERROR;
			if (force <= 1) return -1;
		}
		stamp = (ulg)get_byte();
		stamp |= ((ulg)get_byte()) << 8;
		stamp |= ((ulg)get_byte()) << 16;
		stamp |= ((ulg)get_byte()) << 24;
		if (stamp != 0 && !no_time)
		{
			if (stamp <= TYPE_MAXIMUM(time_t))
			{
				time_stamp.tv_sec = stamp;
				time_stamp.tv_nsec = 0;
			}
			else
			{
				WARN((stderr,
					"%s: %s: MTIME %lu out of range for this platform\n",
					program_name, ifname, stamp));
				time_stamp.tv_sec = TYPE_MAXIMUM(time_t);
				time_stamp.tv_nsec = TIMESPEC_RESOLUTION - 1;
			}
		}

		magic[8] = get_byte();  /* Ignore extra flags.  */
		magic[9] = get_byte();  /* Ignore OS type.  */

		if (flags & HEADER_CRC)
		{
			magic[2] = DEFLATED;
			magic[3] = flags;
			magic[4] = stamp & 0xff;
			magic[5] = (stamp >> 8) & 0xff;
			magic[6] = (stamp >> 16) & 0xff;
			magic[7] = stamp >> 24;
			updcrc(NULL, 0);
			updcrc(magic, 10);
		}

		if ((flags & EXTRA_FIELD) != 0) {
			uch lenbuf[2];
			unsigned int len = lenbuf[0] = get_byte();
			len |= (lenbuf[1] = get_byte()) << 8;
			if (verbose) {
				fprintf(stderr, "%s: %s: extra field of %u bytes ignored\n",
					program_name, ifname, len);
			}
			if (flags & HEADER_CRC)
				updcrc(lenbuf, 2);
			discard_input_bytes(len, flags);
		}

		/* Get original file name if it was truncated */
		if ((flags & ORIG_NAME) != 0) {
			if (no_name || (to_stdout && !list) || part_nb > 1) {
				/* Discard the old name */
				discard_input_bytes(-1, flags);
			}
			else {
				/* Copy the base name. Keep a directory prefix intact. */
				char *p = gzip_base_name(ofname);
				char *base = p;
				for (;;) {
					*p = (char)get_byte();
					if (*p++ == '\0') break;
					if (p >= ofname + sizeof(ofname)) {
						gzip_error("corrupted input -- file name too large");
					}
				}
				if (flags & HEADER_CRC)
					updcrc((uch *)base, p - base);
				p = gzip_base_name(base);
				memmove(base, p, strlen(p) + 1);
				/* If necessary, adapt the name to local OS conventions: */
				if (!list) {
					MAKE_LEGAL_NAME(base);
					if (base) list = 0; /* avoid warning about unused variable */
				}
			} /* no_name || to_stdout */
		} /* ORIG_NAME */

		  /* Discard file comment if any */
		if ((flags & COMMENT) != 0) {
			discard_input_bytes(-1, flags);
		}

		if (flags & HEADER_CRC)
		{
			unsigned int crc16 = updcrc(magic, 0) & 0xffff;
			unsigned int header16 = get_byte();
			header16 |= ((unsigned int)get_byte()) << 8;
			if (header16 != crc16)
			{
				fprintf(stderr,
					"%s: %s: header checksum 0x%04x != computed checksum 0x%04x\n",
					program_name, ifname, header16, crc16);
				exit_code = ERROR;
				if (force <= 1)
					return -1;
			}
		}

		if (part_nb == 1) {
			header_bytes = inptr + 2 * 4; /* include crc and size */
		}

	}
	else if (memcmp(magic, PKZIP_MAGIC, 2) == 0 && inptr == 2
		&& memcmp((char*)inbuf, PKZIP_MAGIC, 4) == 0) {
		/* To simplify the code, we support a zip file when alone only.
		* We are thus guaranteed that the entire local header fits in inbuf.
		*/
		inptr = 0;
		work = unzip;
		if (check_zipfile(in) != OK) return -1;
		/* check_zipfile may get ofname from the local header */
		last_member = 1;

	}
	else if (memcmp(magic, PACK_MAGIC, 2) == 0) {
		work = unpack;
		method = PACKED;

	}
	else if (memcmp(magic, LZW_MAGIC, 2) == 0) {
		work = unlzw;
		method = COMPRESSED;
		last_member = 1;

	}
	else if (memcmp(magic, LZH_MAGIC, 2) == 0) {
		work = unlzh;
		method = LZHED;
		last_member = 1;

	}
	else if (force && to_stdout && !list) { /* pass input unchanged */
		method = STORED;
		work = copy;
		if (imagic1 != EOF)
			inptr--;
		last_member = 1;
		if (imagic0 != EOF) {
			write_buf(STDOUT_FILENO, magic, 1);
			bytes_out++;
		}
	}
	if (method >= 0) return method;

	if (part_nb == 1) {
		fprintf(stderr, "\n%s: %s: not in gzip format\n",
			program_name, ifname);
		exit_code = ERROR;
		return -1;
	}
	else {
		if (magic[0] == 0)
		{
			int inbyte;
			for (inbyte = imagic1; inbyte == 0; inbyte = try_byte())
				continue;
			if (inbyte == EOF)
			{
				if (verbose)
					WARN((stderr, "\n%s: %s: decompression OK, trailing zero bytes ignored\n",
						program_name, ifname));
				return -3;
			}
		}

		WARN((stderr, "\n%s: %s: decompression OK, trailing garbage ignored\n",
			program_name, ifname));
		return -2;
	}
}

int dounzip()
{
	method = get_method(ifd);

	if (method == -1)
	{
		printf("Unknown compression method\n");
		return 0;
	}
	else
		work(0, 0);

	return 1;
}

int dozip()
{
	zip(0, 0);
}

#if TESTING
int(*write_buffer_func)(int fd, voidp buf, unsigned cnt);
int(*read_buffer_func)(int ifd, char *buf, unsigned size);

int read_buffer(ifd, buf, size)
int ifd;
char *buf;
unsigned size;
{
	return read_buffer_func(ifd, buf, size);
}

int read_buffer_file(ifd, buf, size)
int ifd;
char *buf;
unsigned size;
{
	return read(ifd, buf, size);
}

int write_buffer(fd, buf, cnt)
int       fd;
voidp     buf;
unsigned  cnt;
{
	return write_buffer_func(fd, buf, cnt);
}

int write_buffer_file(fd, buf, cnt)
int       fd;
voidp     buf;
unsigned  cnt;
{
	int written = cnt;
	unsigned  n;

	while ((n = write(fd, buf, cnt)) != cnt) {
		if (n == (unsigned)(-1)) {
			write_error();
		}
		cnt -= n;
		buf = (voidp)((char*)buf + n);
	}

	return written;
}

int gunzip()
{
	initgzip();

	read_buffer_func = read_buffer_file;
	write_buffer_func = write_buffer_file;

	ifd = open("c:\\brol\\a.gz", O_RDONLY | O_BINARY);

	method = get_method(ifd);

	//lseek(ifd, (long) (2 + 1 + 1 + 4 + 1 + 1), SEEK_SET);

	ofd = open("c:\\brol\\a'", O_WRONLY | O_BINARY | O_TRUNC | O_CREAT, S_IREAD | S_IWRITE);

	//unzip(ifd, ofd);
	work(ifd, ofd);

	close(ofd);
	close(ifd);
}

int gzip()
{
	initgzip();

	read_buffer_func = read_buffer_file;
	write_buffer_func = write_buffer_file;

	ifd = open("c:\\brol\\a", O_RDONLY | O_BINARY);

	get_input_size_and_time(ifd);

	ofd = open("c:\\brol\\a.gz", O_WRONLY | O_BINARY | O_TRUNC | O_CREAT, S_IREAD | S_IWRITE);

	zip(ifd, ofd);

	close(ofd);
	close(ifd);
}

#endif
