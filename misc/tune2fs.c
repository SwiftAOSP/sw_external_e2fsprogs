/*
 * tune2fs.c		- Change the file system parameters on
 *			  an unmounted second extended file system
 *
 * Copyright (C) 1992, 1993, 1994  Remy Card <card@masi.ibp.fr>
 *                                 Laboratoire MASI, Institut Blaise Pascal
 *                                 Universite Pierre et Marie Curie (Paris VI)
 *
 * Copyright 1995, 1996, 1997 by Theodore Ts'o.
 *
 * %Begin-Header%
 * This file may be redistributed under the terms of the GNU Public
 * License.
 * %End-Header%
 */

/*
 * History:
 * 93/06/01	- Creation
 * 93/10/31	- Added the -c option to change the maximal mount counts
 * 93/12/14	- Added -l flag to list contents of superblock
 *                M.J.E. Mol (marcel@duteca.et.tudelft.nl)
 *                F.W. ten Wolde (franky@duteca.et.tudelft.nl)
 * 93/12/29	- Added the -e option to change errors behavior
 * 94/02/27	- Ported to use the ext2fs library
 * 94/03/06	- Added the checks interval from Uwe Ohse (uwe@tirka.gun.de)
 */

#include <fcntl.h>
#include <grp.h>
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#else
extern char *optarg;
extern int optind;
#endif
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>

#include <linux/ext2_fs.h>

#include "ext2fs/ext2fs.h"
#include "et/com_err.h"
#include "uuid/uuid.h"
#include "e2p/e2p.h"
#include "util.h"

#include "../version.h"
#include "nls-enable.h"

const char * program_name = "tune2fs";
char * device_name;
char * new_label, *new_last_mounted, *new_UUID, *journal_opts;
static int c_flag, C_flag, e_flag, g_flag, i_flag, l_flag, L_flag;
static int m_flag, M_flag, r_flag, s_flag = -1, u_flag, U_flag;
static int max_mount_count, mount_count, mount_flags;
static unsigned long interval, reserved_ratio, reserved_blocks;
static unsigned long resgid, resuid;
static unsigned short errors;

int journal_size, journal_flags;
char *journal_device;

static const char *please_fsck = N_("Please run e2fsck on the filesystem.\n");

static void usage(void)
{
	fprintf(stderr,
		_("Usage: %s [-c max-mounts-count] [-e errors-behavior] "
		  "[-g group]\n"
		 "\t[-i interval[d|m|w]] [-j journal-options]\n"
		 "\t[-l] [-s sparse-flag] [-m reserved-blocks-percent]\n"
		  "\t[-r reserved-blocks-count] [-u user] [-C mount-count]\n"
		  "\t[-L volume-label] [-M last-mounted-dir] [-U UUID]\n"
		  "\t[-O [^]feature[,...]] device\n"), program_name);
	exit (1);
}

static __u32 ok_features[3] = {
	EXT3_FEATURE_COMPAT_HAS_JOURNAL,	/* Compat */
	EXT2_FEATURE_INCOMPAT_FILETYPE,		/* Incompat */
	EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER	/* R/O compat */
};

/*
 * Update the feature set as provided by the user.
 */
static void update_feature_set(ext2_filsys fs, char *features_cmd)
{
	int sparse, old_sparse, filetype, old_filetype;
	int journal, old_journal;
	struct ext2_inode	inode;
	struct ext2_super_block *sb= fs->super;
	errcode_t		retval;

	old_sparse = sb->s_feature_ro_compat &
		EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER;
	old_filetype = sb->s_feature_incompat &
		EXT2_FEATURE_INCOMPAT_FILETYPE;
	old_journal = sb->s_feature_compat &
		EXT3_FEATURE_COMPAT_HAS_JOURNAL;
	if (e2p_edit_feature(features_cmd, &sb->s_feature_compat,
			     ok_features)) {
		fprintf(stderr, _("Invalid filesystem option set: %s\n"),
			features_cmd);
		exit(1);
	}
	sparse = sb->s_feature_ro_compat &
		EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER;
	filetype = sb->s_feature_incompat &
		EXT2_FEATURE_INCOMPAT_FILETYPE;
	journal = sb->s_feature_compat &
		EXT3_FEATURE_COMPAT_HAS_JOURNAL;
	if (old_journal && !journal) {
		if ((mount_flags & EXT2_MF_MOUNTED) &&
		    !(mount_flags & EXT2_MF_READONLY)) {
			fprintf(stderr,
				_("The HAS_JOURNAL flag may only be "
				  "cleared when the filesystem is\n"
				  "unmounted or mounted "
				  "read-only.\n"));
			exit(1);
		}
		if (sb->s_feature_incompat &
		    EXT3_FEATURE_INCOMPAT_RECOVER) {
			fprintf(stderr,
				_("The NEEDS_RECOVERY flag is set.  "
				  "Please run e2fsck before clearing\n"
				  "the HAS_JOURNAL flag.\n"));
			exit(1);
		}
		/*
		 * Remove the immutable flag on the journal inode
		 */
		if (sb->s_journal_inum) {
			retval = ext2fs_read_inode(fs, sb->s_journal_inum, 
						   &inode);
			if (retval) {
				com_err(program_name, retval,
					"while reading journal inode");
				exit(1);
			}
			inode.i_flags &= ~EXT2_IMMUTABLE_FL;
			retval = ext2fs_write_inode(fs, sb->s_journal_inum, 
						    &inode);
			if (retval) {
				com_err(program_name, retval,
					"while write journal inode");
				exit(1);
			}
		}
	}
	if (journal && !old_journal) {
		/*
		 * If adding a journal flag, let the create journal
		 * code below handle creating setting the flag and
		 * creating the journal.  We supply a default size if
		 * necessary.
		 */
		if (!journal_opts)
			journal_opts = "size=16";
		sb->s_feature_compat &=~EXT3_FEATURE_COMPAT_HAS_JOURNAL;
		journal = old_journal;
	}
	
	if (sb->s_rev_level == EXT2_GOOD_OLD_REV &&
	    (sb->s_feature_compat || sb->s_feature_ro_compat ||
	     sb->s_feature_incompat))
		ext2fs_update_dynamic_rev(fs);
	if ((sparse != old_sparse) ||
	    (filetype != old_filetype) ||
	    (journal != old_journal)) {
		sb->s_state &= ~EXT2_VALID_FS;
		printf("\n%s\n", _(please_fsck));
	}
	ext2fs_mark_super_dirty(fs);
}

/*
 * Add a journal to the filesystem.
 */
static void add_journal(ext2_filsys fs)
{
	unsigned long journal_blocks;
	errcode_t	retval;

	if (fs->super->s_feature_compat &
	    EXT3_FEATURE_COMPAT_HAS_JOURNAL) {
		fprintf(stderr, _("The filesystem already has a journal.\n"));
		exit(1);
	}
	parse_journal_opts(journal_opts);
	journal_blocks = journal_size * 1024 / (fs->blocksize / 1024);
	if (journal_device) {
		check_plausibility(journal_device);
		check_mount(journal_device, 0, _("journal"));
		printf(_("Creating journal on device %s: "),
		       journal_device);
		retval = ext2fs_add_journal_device(fs, journal_device,
						   journal_blocks,
						   journal_flags);
		if (retval) {
			com_err (program_name, retval,
				 _("while trying to create journal on device %s"),
				 journal_device);
			exit(1);
		}
		printf(_("done\n"));
	} else if (journal_size) {
		errcode_t	retval;
		int		mount_flags;

		printf(_("Creating journal inode: "));
		fflush(stdout);
		retval = ext2fs_add_journal_inode(fs, journal_blocks,
						  journal_flags);
		if (retval) {
			printf("\n");
			com_err(program_name, retval,
				_("while trying to create journal"));
			exit(1);
		}
		printf(_("done\n"));
		/*
		 * If the filesystem wasn't mounted, we need to force
		 * the block group descriptors out.
		 */
		if ((mount_flags & EXT2_MF_MOUNTED) == 0)
			fs->flags &= ~EXT2_FLAG_SUPER_ONLY;
	}
}



int main (int argc, char ** argv)
{
	int c;
	char * tmp;
	errcode_t retval;
	ext2_filsys fs;
	struct ext2_super_block *sb;
	struct group * gr;
	struct passwd * pw;
	int open_flag = 0;
	char *features_cmd = 0;

#ifdef ENABLE_NLS
	setlocale(LC_MESSAGES, "");
	bindtextdomain(NLS_CAT_NAME, LOCALEDIR);
	textdomain(NLS_CAT_NAME);
#endif
	fprintf (stderr, _("tune2fs %s, %s for EXT2 FS %s, %s\n"),
		 E2FSPROGS_VERSION, E2FSPROGS_DATE,
		 EXT2FS_VERSION, EXT2FS_DATE);
	if (argc && *argv)
		program_name = *argv;
	initialize_ext2_error_table();
	while ((c = getopt (argc, argv, "c:e:g:i:j:lm:r:s:u:C:L:M:O:U:")) != EOF)
		switch (c)
		{
			case 'c':
				max_mount_count = strtol (optarg, &tmp, 0);
				if (*tmp || max_mount_count > 16000) {
					com_err (program_name, 0,
						 _("bad mounts count - %s"),
						 optarg);
					usage();
				}
				c_flag = 1;
				open_flag = EXT2_FLAG_RW;
				break;
			case 'C':
				mount_count = strtoul (optarg, &tmp, 0);
				if (*tmp || mount_count > 16000) {
					com_err (program_name, 0,
						 _("bad mounts count - %s"),
						 optarg);
					usage();
				}
				C_flag = 1;
				open_flag = EXT2_FLAG_RW;
				break;
			case 'e':
				if (strcmp (optarg, "continue") == 0)
					errors = EXT2_ERRORS_CONTINUE;
				else if (strcmp (optarg, "remount-ro") == 0)
					errors = EXT2_ERRORS_RO;
				else if (strcmp (optarg, "panic") == 0)
					errors = EXT2_ERRORS_PANIC;
				else {
					com_err (program_name, 0,
						 _("bad error behavior - %s"),
						 optarg);
					usage();
				}
				e_flag = 1;
				open_flag = EXT2_FLAG_RW;
				break;
			case 'g':
				resgid = strtoul (optarg, &tmp, 0);
				if (*tmp) {
					gr = getgrnam (optarg);
					if (gr == NULL)
						tmp = optarg;
					else {
						resgid = gr->gr_gid;
						*tmp =0;
					}
				}
				if (*tmp) {
					com_err (program_name, 0,
						 _("bad gid/group name - %s"),
						 optarg);
					usage();
				}
				g_flag = 1;
				open_flag = EXT2_FLAG_RW;
				break;
			case 'i':
				interval = strtoul (optarg, &tmp, 0);
				switch (*tmp) {
				case 's':
					tmp++;
					break;
				case '\0':
				case 'd':
				case 'D': /* days */
					interval *= 86400;
					if (*tmp != '\0')
						tmp++;
					break;
				case 'm':
				case 'M': /* months! */
					interval *= 86400 * 30;
					tmp++;
					break;
				case 'w':
				case 'W': /* weeks */
					interval *= 86400 * 7;
					tmp++;
					break;
				}
				if (*tmp || interval > (365 * 86400)) {
					com_err (program_name, 0,
						_("bad interval - %s"), optarg);
					usage();
				}
				i_flag = 1;
				open_flag = EXT2_FLAG_RW;
				break;
			case 'l':
				l_flag = 1;
				break;
			case 'j':
				journal_opts = optarg;
				open_flag = EXT2_FLAG_RW;
				break;
			case 'L':
				new_label = optarg;
				L_flag = 1;
				open_flag = EXT2_FLAG_RW;
				break;
			case 'm':
				reserved_ratio = strtoul (optarg, &tmp, 0);
				if (*tmp || reserved_ratio > 50) {
					com_err (program_name, 0,
						 _("bad reserved block ratio - %s"),
						 optarg);
					usage();
				}
				m_flag = 1;
				open_flag = EXT2_FLAG_RW;
				break;
			case 'M':
				new_last_mounted = optarg;
				M_flag = 1;
				open_flag = EXT2_FLAG_RW;
				break;
			case 'O':
				features_cmd = optarg;
				open_flag = EXT2_FLAG_RW;
				break;
			case 'r':
				reserved_blocks = strtoul (optarg, &tmp, 0);
				if (*tmp) {
					com_err (program_name, 0,
						 _("bad reserved blocks count - %s"),
						 optarg);
					usage();
				}
				r_flag = 1;
				open_flag = EXT2_FLAG_RW;
				break;
			case 's':
				s_flag = atoi(optarg);
				open_flag = EXT2_FLAG_RW;
				break;
			case 'u':
				resuid = strtoul (optarg, &tmp, 0);
				if (*tmp) {
					pw = getpwnam (optarg);
					if (pw == NULL)
						tmp = optarg;
					else {
						resuid = pw->pw_uid;
						*tmp = 0;
					}
				}
				if (*tmp) {
					com_err (program_name, 0,
						 _("bad uid/user name - %s"),
						 optarg);
					usage();
				}
				u_flag = 1;
				open_flag = EXT2_FLAG_RW;
				break;
			case 'U':
				new_UUID = optarg;
				U_flag = 1;
				open_flag = EXT2_FLAG_RW;
				break;
			default:
				usage();
		}
	if (optind < argc - 1 || optind == argc)
		usage();
	if (!open_flag && !l_flag)
		usage();
	device_name = argv[optind];
	retval = ext2fs_open (device_name, open_flag, 0, 0,
			      unix_io_manager, &fs);
        if (retval) {
		com_err (program_name, retval, _("while trying to open %s"),
			 device_name);
		printf(_("Couldn't find valid filesystem superblock.\n"));
		exit(1);
	}
	retval = ext2fs_check_if_mounted(device_name, &mount_flags);
	if (retval) {
		com_err("ext2fs_check_if_mount", retval,
			_("while determining whether %s is mounted."),
			device_name);
		return;
	}
	sb = fs->super;
	/* Normally we only need to write out the superblock */
	fs->flags |= EXT2_FLAG_SUPER_ONLY;

	if (c_flag) {
		sb->s_max_mnt_count = max_mount_count;
		ext2fs_mark_super_dirty(fs);
		printf (_("Setting maximal mount count to %d\n"),
			max_mount_count);
	}
	if (C_flag) {
		sb->s_mnt_count = mount_count;
		ext2fs_mark_super_dirty(fs);
		printf (_("Setting current mount count to %d\n"), mount_count);
	}
	if (e_flag) {
		sb->s_errors = errors;
		ext2fs_mark_super_dirty(fs);
		printf (_("Setting error behavior to %d\n"), errors);
	}
	if (g_flag) {
		sb->s_def_resgid = resgid;
		ext2fs_mark_super_dirty(fs);
		printf (_("Setting reserved blocks gid to %lu\n"), resgid);
	}
	if (i_flag) {
		sb->s_checkinterval = interval;
		ext2fs_mark_super_dirty(fs);
		printf (_("Setting interval between check %lu seconds\n"), interval);
	}
	if (m_flag) {
		sb->s_r_blocks_count = (sb->s_blocks_count / 100)
			* reserved_ratio;
		ext2fs_mark_super_dirty(fs);
		printf (_("Setting reserved blocks percentage to %lu (%u blocks)\n"),
			reserved_ratio, sb->s_r_blocks_count);
	}
	if (r_flag) {
		if (reserved_blocks >= sb->s_blocks_count) {
			com_err (program_name, 0,
				 _("reserved blocks count is too big (%ul)"),
				 reserved_blocks);
			exit (1);
		}
		sb->s_r_blocks_count = reserved_blocks;
		ext2fs_mark_super_dirty(fs);
		printf (_("Setting reserved blocks count to %lu\n"),
			reserved_blocks);
	}
	if (s_flag == 1) {
		if (sb->s_feature_ro_compat &
		    EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER)
			fprintf(stderr, _("\nThe filesystem already"
				" has sparse superblocks.\n"));
		else {
			sb->s_feature_ro_compat |=
				EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER;
			sb->s_state &= ~EXT2_VALID_FS;
			ext2fs_mark_super_dirty(fs);
			printf(_("\nSparse superblock flag set.  %s"),
			       _(please_fsck));
		}
	}
	if (s_flag == 0) {
		if (!(sb->s_feature_ro_compat &
		      EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER))
			fprintf(stderr, _("\nThe filesystem already"
				" has sparse superblocks disabled.\n"));
		else {
			sb->s_feature_ro_compat &=
				~EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER;
			sb->s_state &= ~EXT2_VALID_FS;
			fs->flags |= EXT2_FLAG_MASTER_SB_ONLY;
			ext2fs_mark_super_dirty(fs);
			printf(_("\nSparse superblock flag cleared.  %s"),
			       _(please_fsck));
		}
	}
	if (u_flag) {
		sb->s_def_resuid = resuid;
		ext2fs_mark_super_dirty(fs);
		printf (_("Setting reserved blocks uid to %lu\n"), resuid);
	}
	if (L_flag) {
		if (strlen(new_label) > sizeof(sb->s_volume_name))
			fprintf(stderr, _("Warning: label too "
				"long, truncating.\n"));
		memset(sb->s_volume_name, 0, sizeof(sb->s_volume_name));
		strncpy(sb->s_volume_name, new_label,
			sizeof(sb->s_volume_name));
		ext2fs_mark_super_dirty(fs);
	}
	if (M_flag) {
		memset(sb->s_last_mounted, 0, sizeof(sb->s_last_mounted));
		strncpy(sb->s_last_mounted, new_last_mounted,
			sizeof(sb->s_last_mounted));
		ext2fs_mark_super_dirty(fs);
	}
	if (features_cmd)
		update_feature_set(fs, features_cmd);
	if (journal_opts)
		add_journal(fs);
	
	if (U_flag) {
		if (strcasecmp(new_UUID, "null") == 0) {
			uuid_clear(sb->s_uuid);
		} else if (strcasecmp(new_UUID, "time") == 0) {
			uuid_generate_time(sb->s_uuid);
		} else if (strcasecmp(new_UUID, "random") == 0) {
			uuid_generate(sb->s_uuid);
		} else if (uuid_parse(new_UUID, sb->s_uuid)) {
			com_err(program_name, 0, _("Invalid UUID format\n"));
			exit(1);
		}
		ext2fs_mark_super_dirty(fs);
	}

	if (l_flag)
		list_super (sb);
	ext2fs_close (fs);
	exit (0);
}
