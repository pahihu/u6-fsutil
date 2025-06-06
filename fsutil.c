/*
 * Utility for dealing with unix v6 filesystem images.
 *
 * Copyright (C) 2006 Serge Vakulenko, <vak@cronyx.ru>
 *
 * This file is part of BKUNIX project, which is distributed
 * under the terms of the GNU General Public License (GPL).
 * See the accompanying file "COPYING" for more details.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <argp.h>
#include "u6fs.h"

int verbose;
int extract;
int add;
int newfs;
int check;
int fix;
int flat;
unsigned int bytes;
char *boot_sector;
char *boot_sector2;

const char *argp_program_version =
	"LSX file system information, version 1.0\n"
	"Copyright (C) 2002 Serge Vakulenko\n"
	"This program is free software; it comes with ABSOLUTELY NO WARRANTY;\n"
	"see the GNU General Public License for more details.";

const char *argp_program_bug_address = "<vak@cronyx.ru>";

struct argp_option argp_options[] = {
	{"verbose",	'v', 0,		0,	"Print verbose information" },
	{"add",		'a', 0,		0,	"Add files to filesystem" },
	{"extract",	'x', 0,		0,	"Extract all files" },
	{"check",	'c', 0,		0,	"Check filesystem, use -c -f to fix" },
	{"fix",		'f', 0,		0,	"Fix bugs in filesystem" },
	{"new",		'n', 0,		0,	"Create new filesystem, -s required" },
	{"size",	's', "NUM",	0,	"Size in bytes for created filesystem" },
	{"boot",	'b', "FILE",	0,	"Boot sector, -B required if not -F" },
	{"boot2",	'B', "FILE",	0,	"Secondary boot sector, -b required" },
	{"flat",	'F', 0,		0,	"Flat mode, no sector remapping" },
	{ 0 }
};

/*
 * Parse a single option.
 */
int argp_parse_option (int key, char *arg, struct argp_state *state)
{
	switch (key) {
	case 'v':
		++verbose;
		break;
	case 'a':
		++add;
		break;
	case 'x':
		++extract;
		break;
	case 'n':
		++newfs;
		break;
	case 'c':
		++check;
		break;
	case 'f':
		++fix;
		break;
	case 'F':
		++flat;
		break;
	case 's':
		bytes = strtol (arg, 0, 0);
		break;
	case 'b':
		boot_sector = arg;
		break;
	case 'B':
		boot_sector2 = arg;
		break;
	case ARGP_KEY_END:
		if (state->arg_num < 1)		/* Not enough arguments. */
			argp_usage (state);
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

/*
 * Our argp parser.
 */
const struct argp argp_parser = {
	/* The options we understand. */
	argp_options,

	/* Function to parse a single option. */
	argp_parse_option,

	/* A description of the arguments we accept. */
	"infile.dsk [files-to-add...]",

	/* Program documentation. */
	"\nPrint LSX file system information"
};

void print_inode (u6fs_inode_t *inode,
	char *dirname, char *filename, FILE *out)
{
	fprintf (out, "%s/%s", dirname, filename);
	switch (inode->mode & INODE_MODE_FMT) {
	case INODE_MODE_FDIR:
		fprintf (out, "/\n");
		break;
	case INODE_MODE_FCHR:
		fprintf (out, " - char %d %d\n",
			inode->addr[0] >> 8, inode->addr[0] & 0xff);
		break;
	case INODE_MODE_FBLK:
		fprintf (out, " - block %d %d\n",
			inode->addr[0] >> 8, inode->addr[0] & 0xff);
		break;
	default:
		fprintf (out, " - %lu bytes\n", inode->size);
		break;
	}
}

void print_indirect_block (u6fs_t *fs, unsigned int bno, FILE *out)
{
	unsigned short nb;
	unsigned char data [LSXFS_BSIZE];
	int i;

	fprintf (out, " [%d]", bno);
	if (! u6fs_read_block (fs, bno, data)) {
		fprintf (stderr, "read error at block %d\n", bno);
		return;
	}
	for (i=0; i<LSXFS_BSIZE-2; i+=2) {
		nb = data [i+1] << 8 | data [i];
		if (nb)
			fprintf (out, " %d", nb);
	}
}

void print_double_indirect_block (u6fs_t *fs, unsigned int bno, FILE *out)
{
	unsigned short nb;
	unsigned char data [LSXFS_BSIZE];
	int i;

	fprintf (out, " [%d]", bno);
	if (! u6fs_read_block (fs, bno, data)) {
		fprintf (stderr, "read error at block %d\n", bno);
		return;
	}
	for (i=0; i<LSXFS_BSIZE-2; i+=2) {
		nb = data [i+1] << 8 | data [i];
		if (nb)
			print_indirect_block (fs, nb, out);
	}
}

void print_inode_blocks (u6fs_inode_t *inode, FILE *out)
{
	int i;

	if ((inode->mode & INODE_MODE_FMT) == INODE_MODE_FCHR ||
	    (inode->mode & INODE_MODE_FMT) == INODE_MODE_FBLK)
		return;

	fprintf (out, "    ");
	if (inode->mode & INODE_MODE_LARG) {
		for (i=0; i<7; ++i) {
			if (inode->addr[i] == 0)
				continue;
			print_indirect_block (inode->fs, inode->addr[i], out);
		}
		if (inode->addr[7] != 0)
			print_double_indirect_block (inode->fs,
				inode->addr[7], out);
	} else {
		for (i=0; i<8; ++i) {
			if (inode->addr[i] == 0)
				continue;
			fprintf (out, " %d", inode->addr[i]);
		}
	}
	fprintf (out, "\n");
}

void extract_inode (u6fs_inode_t *inode, char *path)
{
	int fd, n;
	unsigned int offset;
	unsigned char data [512];

	fd = open (path, O_CREAT | O_WRONLY, inode->mode & 0x777);
	if (fd < 0) {
		perror (path);
		return;
	}
	for (offset = 0; offset < inode->size; offset += 512) {
		n = inode->size - offset;
		if (n > 512)
			n = 512;
		if (! u6fs_inode_read (inode, offset, data, n)) {
			fprintf (stderr, "%s: read error at offset %ld\n",
				path, offset);
			break;
		}
		if (write (fd, data, n) != n) {
			fprintf (stderr, "%s: write error\n", path);
			break;
		}
	}
	close (fd);
}

void extractor (u6fs_inode_t *dir, u6fs_inode_t *inode,
	char *dirname, char *filename, void *arg)
{
	FILE *out = arg;
	char *path;

	if (verbose)
		print_inode (inode, dirname, filename, out);

	if ((inode->mode & INODE_MODE_FMT) != INODE_MODE_FDIR &&
	    (inode->mode & INODE_MODE_FMT) != 0)
		return;

	path = alloca (strlen (dirname) + strlen (filename) + 2);
	strcpy (path, dirname);
	strcat (path, "/");
	strcat (path, filename);

	if ((inode->mode & INODE_MODE_FMT) == INODE_MODE_FDIR) {
		if (mkdir (path, 0775) < 0)
			perror (path);
		/* Scan subdirectory. */
		u6fs_directory_scan (inode, path, extractor, arg);
	} else {
		extract_inode (inode, path);
	}
}

void scanner (u6fs_inode_t *dir, u6fs_inode_t *inode,
	char *dirname, char *filename, void *arg)
{
	FILE *out = arg;
	char *path;

	print_inode (inode, dirname, filename, out);

	if (verbose > 1) {
		/* Print a list of blocks. */
		print_inode_blocks (inode, out);
		if (verbose > 2) {
			u6fs_inode_print (inode, out);
			printf ("--------\n");
		}
	}
	if ((inode->mode & INODE_MODE_FMT) == INODE_MODE_FDIR) {
		/* Scan subdirectory. */
		path = alloca (strlen (dirname) + strlen (filename) + 2);
		strcpy (path, dirname);
		strcat (path, "/");
		strcat (path, filename);
		u6fs_directory_scan (inode, path, scanner, arg);
	}
}

/*
 * Create a directory.
 */
void add_directory (u6fs_t *fs, char *name)
{
	u6fs_inode_t dir, parent;
	char buf [512], *p;

	/* Open parent directory. */
	strcpy (buf, name);
	p = strrchr (buf, '/');
	if (p)
		*p = 0;
	else
		*buf = 0;
	if (! u6fs_inode_by_name (fs, &parent, buf, 0, 0)) {
		fprintf (stderr, "%s: cannot open directory\n", buf);
		return;
	}

	/* Create directory. */
	if (! u6fs_inode_by_name (fs, &dir, name, 1,
	    INODE_MODE_FDIR | 0777)) {
		fprintf (stderr, "%s: directory inode create failed\n", name);
		return;
	}
	u6fs_inode_save (&dir, 0);

	/* Make link '.' */
	strcpy (buf, name);
	strcat (buf, "/.");
	if (! u6fs_inode_by_name (fs, &dir, buf, 3, dir.number)) {
		fprintf (stderr, "%s: dot link failed\n", name);
		return;
	}
	++dir.nlink;
	u6fs_inode_save (&dir, 1);
/*printf ("*** inode %d: increment link counter to %d\n", dir.number, dir.nlink);*/

	/* Make parent link '..' */
	strcat (buf, ".");
	if (! u6fs_inode_by_name (fs, &dir, buf, 3, parent.number)) {
		fprintf (stderr, "%s: dotdot link failed\n", name);
		return;
	}
	if (! u6fs_inode_get (fs, &parent, parent.number)) {
		fprintf (stderr, "inode %d: cannot open parent\n", parent.number);
		return;
	}
	++parent.nlink;
	u6fs_inode_save (&parent, 1);
/*printf ("*** inode %d: increment link counter to %d\n", parent.number, parent.nlink);*/
}

/*
 * Create a device node.
 */
void add_device (u6fs_t *fs, char *name, char *spec)
{
	u6fs_inode_t dev;
	int majr, minr;
	char type;

	if (sscanf (spec, "%c%d:%d", &type, &majr, &minr) != 3 ||
	    (type != 'c' && type != 'b') ||
	    majr < 0 || majr > 255 || minr < 0 || minr > 255) {
		fprintf (stderr, "%s: invalid device specification\n", spec);
		fprintf (stderr, "expected c<major>:<minor> or b<major>:<minor>\n");
		return;
	}
	if (! u6fs_inode_by_name (fs, &dev, name, 1, 0666 |
	    ((type == 'b') ? INODE_MODE_FBLK : INODE_MODE_FCHR))) {
		fprintf (stderr, "%s: device inode create failed\n", name);
		return;
	}
	dev.addr[0] = majr << 8 | minr;
	u6fs_inode_save (&dev, 1);
}

/*
 * Copy file to filesystem.
 * When name is ended by slash as "name/", directory is created.
 */
void add_file (u6fs_t *fs, char *name)
{
	u6fs_file_t file;
	FILE *fd;
	char data [512], *p;
	int len;

	if (verbose) {
		printf ("%s\n", name);
	}
	p = strrchr (name, '/');
	if (p && p[1] == 0) {
		*p = 0;
		add_directory (fs, name);
		return;
	}
	p = strrchr (name, '!');
	if (p) {
		*p++ = 0;
		add_device (fs, name, p);
		return;
	}
	fd = fopen (name, "r");
	if (! fd) {
		perror (name);
		return;
	}
	if (! u6fs_file_create (fs, &file, name, 0777)) {
		fprintf (stderr, "%s: cannot create\n", name);
		return;
	}
	for (;;) {
		len = fread (data, 1, sizeof (data), fd);
/*		printf ("read %d bytes from %s\n", len, name);*/
		if (len < 0)
			perror (name);
		if (len <= 0)
			break;
		if (! u6fs_file_write (&file, data, len)) {
			fprintf (stderr, "%s: write error\n", name);
			break;
		}
	}
	u6fs_file_close (&file);
	fclose (fd);
}

void add_boot (u6fs_t *fs)
{
	if (flat) {
		if (boot_sector2) {
			fprintf(stderr, "Secondary boot ignored\n");
		}
		if (boot_sector) {
			if (! u6fs_install_single_boot (fs, boot_sector)) {
				fprintf (stderr, "%s: incorrect boot sector\n",
				boot_sector);
				return;
			}
			printf ("Boot sector %s installed\n", boot_sector);
		}
	} else if (boot_sector && boot_sector2) {
		if (! u6fs_install_boot (fs, boot_sector,
		    boot_sector2)) {
			fprintf (stderr, "%s: incorrect boot sector\n",
				boot_sector);
			return;
		}
		printf ("Boot sectors %s and %s installed\n",
			boot_sector, boot_sector2);
	}
}

int main (int argc, char **argv)
{
	int i;
	u6fs_t fs;
	u6fs_inode_t inode;

	argp_parse (&argp_parser, argc, argv, 0, &i, 0);
	if ((! add && i != argc-1) || (add && i >= argc-1) ||
	    (extract + newfs + check + add > 1) ||
	    (!flat && (! boot_sector ^ ! boot_sector2)) ||
	    (newfs && bytes < 5120)) {
		argp_help (&argp_parser, stderr, ARGP_HELP_USAGE, argv[0]);
		return -1;
	}
	if (newfs) {
		/* Create new filesystem. */
		if (! u6fs_create (&fs, argv[i], bytes)) {
			fprintf (stderr, "%s: cannot create filesystem\n", argv[i]);
			return -1;
		}
		printf ("Created filesystem %s - %ld bytes\n", argv[i], bytes);
		add_boot (&fs);
		u6fs_close (&fs);
		return 0;
	}

	if (check) {
		/* Check filesystem for errors, and optionally fix them. */
		if (! u6fs_open (&fs, argv[i], fix)) {
			fprintf (stderr, "%s: cannot open\n", argv[i]);
			return -1;
		}
		u6fs_check (&fs);
		u6fs_close (&fs);
		return 0;
	}

	/* Add or extract or info or boot update. */
	if (! u6fs_open (&fs, argv[i],
			(add != 0) || (boot_sector && boot_sector2))) {
		fprintf (stderr, "%s: cannot open\n", argv[i]);
		return -1;
	}

	if (extract) {
		/* Extract all files to current directory. */
		if (! u6fs_inode_get (&fs, &inode, 1)) {
			fprintf (stderr, "%s: cannot get inode 1\n", argv[i]);
			return -1;
		}
		u6fs_directory_scan (&inode, ".", extractor, (void*) stdout);
		u6fs_close (&fs);
		return 0;
	}

	add_boot (&fs);

	if (add) {
		/* Add files i+1..argc-1 to filesystem. */
		while (++i < argc)
			add_file (&fs, argv[i]);
		u6fs_sync (&fs, 0);
		u6fs_close (&fs);
		return 0;
	}

	/* Print the structure of flesystem. */
	u6fs_print (&fs, stdout);
	if (verbose) {
		printf ("--------\n");
		if (! u6fs_inode_get (&fs, &inode, 1)) {
			fprintf (stderr, "%s: cannot get inode 1\n", argv[i]);
			return -1;
		}
		if (verbose > 1) {
			u6fs_inode_print (&inode, stdout);
			printf ("--------\n");
			printf ("/\n");
			print_inode_blocks (&inode, stdout);
		}
		u6fs_directory_scan (&inode, "", scanner, (void*) stdout);
	}
	u6fs_close (&fs);
	return 0;
}
