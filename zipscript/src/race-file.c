#include <ctype.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>

#include "crc.h"
#include "race-file.h" 

#include "objects.h"
#include "macros.h"

#ifdef _WITH_SS5
#include "constants.ss5.h"
#else
#include "constants.h"
#endif

#include "stats.h"
#include "zsfunctions.h"

#include "helpfunctions.h"

#include "../conf/zsconfig.h"
#include "../include/zsconfig.defaults.h"

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifndef HAVE_STRLCPY
# include "strl/strl.h"
#endif


/*
 * Modified	: 02.19.2002 Author	: Dark0n3
 * Modified	: 06.02.2005 by		: js
 * Description	: Creates directory where all race information will be
 * stored.
 */
void 
maketempdir(char *path)
{
	char		full_path[PATH_MAX], *p;

	snprintf(full_path, PATH_MAX, "%s/%s", storage, path);

	/* work recursively */
	for (p = full_path; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(full_path, 0777);
			*p = '/';
		}
	}

	/* the final entry */
	mkdir(full_path, 0777);
}

/*
 * Modified	: 01.16.2002 Author	: Dark0n3
 * 
 * Description	: Reads crc for current file from preparsed sfv file.
 */
unsigned int 
readsfv(const char *path, struct VARS *raceI, int getfcount)
{
	int		insfv = 0;
	unsigned int	crc = 0;
	FILE		*sfvfile;
	DIR		*dir;
	
	SFVDATA		sd;

	if (!(sfvfile = fopen(path, "r"))) {
		d_log("Failed to open sfv (%s): %s\n", path, strerror(errno));
		return 0;
	}

	/* release_type is stored in the beginning of sfvdata */
	fread(&raceI->misc.release_type, sizeof(short int), 1, sfvfile);
	
	d_log("readsfv: Reading data from sfv for (%s)\n", raceI->file.name);
	
	dir = opendir(".");
	
	raceI->total.files = 0;
	while (fread(&sd, sizeof(SFVDATA), 1, sfvfile)) {
		raceI->total.files++;
#if (sfv_cleanup && sfv_cleanup_lowercase)
		if (!strcasecmp(raceI->file.name, sd.fname))
#else
		if (!strcmp(raceI->file.name, sd.fname))
#endif
		{
			d_log("readsfv: crc read from sfv-file (%s): %.8x\n", sd.fname, sd.crc32);
			crc = sd.crc32;
			insfv = 1;
		}
		if (getfcount && findfile(dir, sd.fname)) {
			raceI->total.files_missing--;
		}
	}
	
	closedir(dir);
	fclose(sfvfile);
	
	raceI->total.files_missing += raceI->total.files;
	
	if (insfv)
		errno = 1;
	else
		errno = 0;

	return crc;
}

void
update_sfvdata(const char *path, const unsigned int crc)
{
	int		fd;
	struct flock	fl;
	
	SFVDATA		sd;

	if ((fd = open(path, O_RDWR, 0666)) == -1) {
		d_log("Failed to open sfvdata (%s): %s\n", path, strerror(errno));
		return;
	}

	xlock(&fl, fd);
	
	sd.crc32 = crc;
	
	lseek(fd, sizeof(short int), SEEK_CUR);

	while (read(fd, &sd, sizeof(SFVDATA))) {
		if (strcasecmp(path, sd.fname) == 0) {
			sd.crc32 = crc;
			break;
		}
	}
	
	lseek(fd, -sizeof(SFVDATA), SEEK_CUR);
	write(fd, &sd, sizeof(SFVDATA));
	
	xunlock(&fl, fd);
	
	close(fd);
}

/* convert the sfvdata file source to the sfv dest */
void
sfvdata_to_sfv(const char *source, const char *dest)
{
	int		infd, outfd;
	char		crctmp[8];
	struct flock	fl;
	
	SFVDATA		sd;

	if ((infd = open(source, O_RDONLY)) == -1) {
		d_log("Failed to open (%s): %s\n", source, strerror(errno));
		return;
	}

	if ((outfd = open(source, O_CREAT | O_WRONLY, 0644)) == -1) {
		d_log("Failed to open (.tmpsfv): %s\n", strerror(errno));
		return;
	}

	xlock(&fl, outfd);
	
	lseek(infd, sizeof(short int), SEEK_CUR);

	while (read(infd, &sd, sizeof(SFVDATA))) {
		
		sprintf(crctmp, "%.8x", sd.crc32);
		
		write(outfd, sd.fname, PATH_MAX);
		write(outfd, " ", 1);
		write(outfd, &crctmp, 8),
#if (sfv_cleanup_crlf == TRUE )
		write(outfd, "\r", 1);
#endif
		write(outfd, "\n", 1);

	}
	
	xunlock(&fl, outfd);

	close(infd);
	close(outfd);

	rename(".tmpsfv", dest);	
}

/*
 * Modified	: 01.16.2002 Author	: Dark0n3
 * 
 * Description	: Deletes all -missing files with preparsed sfv.
 */
void 
delete_sfv(const char *path)
{
	char		*f = 0, missing_fname[PATH_MAX];
	FILE		*sfvfile;

	SFVDATA		sd;

	if (!(sfvfile = fopen(path, "r"))) {
		d_log("Couldn't fopen %s: %s\n", path, strerror(errno));
		exit(EXIT_FAILURE);
	}
	
	/* jump past release_type */
	fseek(sfvfile, sizeof(short int), SEEK_CUR);

	while (fread(&sd, sizeof(SFVDATA), 1, sfvfile)) {
		snprintf(missing_fname, PATH_MAX, "%s-missing", sd.fname);
		if ((f = findfilename(missing_fname, f)))
			unlink(missing_fname);
	}
	
	fclose(sfvfile);
}

/*
 * Modified	: 01.16.2002 Author	: Dark0n3
 * 
 * Description	: Reads name of old race leader and writes name of new leader
 * 		  into temporary file.
 * 
 */
void 
read_write_leader(const char *path, struct VARS *raceI, struct USERINFO *userI)
{
	int		fd;
	struct flock	fl;
	struct stat		sb;

	if ((fd = open(path, O_CREAT | O_RDWR, 0666)) == -1) {
		d_log("read_write_leader: open(%s): %s\n", path, strerror(errno));
		return;
	}
	
	xlock(&fl, fd);
	fstat(fd, &sb);

	if (sb.st_size == 0) {
		*raceI->misc.old_leader = '\0';
	} else {
		read(fd, &raceI->misc.old_leader, 24);
		lseek(fd, 0L, SEEK_SET);
	}

	write(fd, userI->name, 24);
	
	xunlock(&fl, fd);
	close(fd);
}

/*
 * Modified	: 01.16.2002 Author	: Dark0n3
 * 
 * Description	: Goes through all untested files and compares crc of file
 * with one that is reported in sfv.
 * 
 */
void 
testfiles(struct LOCATIONS *locations, struct VARS *raceI, int rstatus)
{
	int		fd, lret, count;
	char		*realfile, target[256], *ext;
	unsigned int	Tcrc;
	struct stat	filestat;
	struct flock	fl;

	RACEDATA	rd;

	if ((fd = open(locations->race, O_CREAT | O_RDWR, 0666)) == -1) {
		if (errno != EEXIST) {
			d_log("testfiles: open(%s): %s\n", locations->race, strerror(errno));
			exit(EXIT_FAILURE);
		}
	}
	
	xlock(&fl, fd);

	realfile = raceI->file.name;

	if (rstatus)
		printf("\n");

	count = 0;
	while ((read(fd, &rd, sizeof(RACEDATA)))) {


		d_log("testfiles:  Checking: %s\n", rd.fname);
		ext = find_last_of(realfile, ".");
		if (*ext == '.')
			ext++;

		if (rd.status == F_NOTCHECKED) {
			strlcpy(raceI->file.name, rd.fname, PATH_MAX);
			Tcrc = readsfv(locations->sfv, raceI, 0);
			stat(rd.fname, &filestat);
			if (S_ISDIR(filestat.st_mode))
				rd.status = F_IGNORED;
			else if (rd.crc32 != 0 && Tcrc == rd.crc32)
				rd.status = F_CHECKED;
			else if (rd.crc32 != 0 && strcomp(ignored_types, ext))
				rd.status = F_IGNORED;
			else if (rd.crc32 != 0 && Tcrc == 0 && strcomp(allowed_types, ext))
				rd.status = F_IGNORED;
			else if ((rd.crc32 != 0) && (Tcrc != rd.crc32) &&
					   (strcomp(allowed_types, ext) &&
				   !matchpath(allowed_types_exemption_dirs, locations->path)))
				rd.status = F_IGNORED;
			else {
				mark_as_bad(rd.fname);
				if (rd.fname)
					unlink(rd.fname);
				rd.status = F_BAD;

#if ( create_missing_files )
				if (Tcrc != 0)
					create_missing(rd.fname);
#endif

				if (rstatus)
					printf("File: %s FAILED!\n", rd.fname);

				d_log("marking %s bad.\n", rd.fname);
				if (enable_unduper_script == TRUE) {
					if (!fileexists(unduper_script)) {
						d_log("Failed to undupe '%s' - '%s' does not exist.\n",
							  rd.fname, unduper_script);
					} else {
						sprintf(target, unduper_script " \"%s\"", rd.fname);
						if (execute(target) == 0)
							d_log("undupe of %s successful.\n", rd.fname);
						else
							d_log("undupe of %s failed.\n", rd.fname);
					}
				}
			}
			if (rd.status == F_BAD) {
				xunlock(&fl, fd);
				remove_from_race(locations->race, rd.fname);
				xlock(&fl, fd);
			} else {
				if ((lret = lseek(fd, sizeof(RACEDATA) * count, SEEK_SET)) == -1) {
					d_log("testfiles: lseek: %s\n", strerror(errno));
					exit(EXIT_FAILURE);
				}
				write(fd, &rd, sizeof(RACEDATA));
				unlink_missing(rd.fname);
			}
		}
		count++;
	}
	strlcpy(raceI->file.name, realfile, strlen(realfile)+1);
	raceI->total.files = raceI->total.files_missing = 0;
	xunlock(&fl, fd);
	close(fd);
}

/*
 * Modified	: 01.20.2002 Author	: Dark0n3
 *
 * Description	: Parses file entries from sfv file and store them in a file.
 * 
 * Todo		: Add dupefile remover.
 *
 * Totally rewritten by js on 08.02.2005
 */
int
copysfv(const char *source, const char *target)
{
	int		infd, outfd, i, retval = 0;
	short int	music, rars, video, others, type;
	
	char		*ptr, fbuf[2048];
	FILE		*insfv;

	DIR		*dir;

	SFVDATA		sd;

	struct flock	fl;
	
#if ( sfv_dupecheck == TRUE )
	int		skip = 0;
	SFVDATA		tempsd;
#endif
	
#if ( sfv_cleanup == TRUE )
	int		tmpfd;
	char		crctmp[8];
	
	if ((tmpfd = open(".tmpsfv", O_CREAT | O_RDWR, 0644)) == -1)
		d_log("copysfv: open(.tmpsfv): %s\n", strerror(errno));
	else
		xlock(&fl, tmpfd);
#endif

	if ((infd = open(source, O_RDONLY)) == -1) {
		d_log("copysfv: open(%s): %s\n", source, strerror(errno));
		exit(EXIT_FAILURE);
	}
	
	if ((outfd = open(target, O_CREAT | O_RDWR, 0666)) == -1) {
		d_log("copysfv: open(%s): %s\n", target, strerror(errno));
		exit(EXIT_FAILURE);
	}
	
	xlock(&fl, infd);
	xlock(&fl, outfd);

	video = music = rars = others = type = 0;

	dir = opendir(".");

	write(outfd, &type, sizeof(short int));
	
	insfv = fdopen(infd, "r");
	while ((fgets(fbuf, sizeof(fbuf), insfv))) {
		
		/* remove comment */
		if ((ptr = find_first_of(fbuf, ";")))
			*ptr = '\0';
			
		strip_whitespaces(fbuf);

		if (strlen(fbuf) == 0)
			continue;
	
#if (sfv_cleanup == TRUE)
#if (sfv_cleanup_lowercase == TRUE)
		for (ptr = fbuf; *ptr; ptr++)
			*ptr = tolower(*ptr);
#endif
#endif
		sd.crc32 = 0;
		bzero(sd.fname, sizeof(sd.fname));
		if ((ptr = find_last_of(fbuf, " \t"))) {
			
			/* pass the " \t" */
			ptr++;
			
			/* what we have now is hopefully a crc */
			for (i = 0; isxdigit(*ptr) != 0; i++)
				ptr++;
			
			ptr -= i;
			if (i != 8) {
				/* we didn't get an 8 digit crc number */
#if (sfv_cleanup == TRUE)
				/* do stuff  */
#else
				retval = 1;
				goto END;
#endif
			} else {
				sd.crc32 = hexstrtodec(ptr);
				
				/* cut off crc string */
				*ptr = '\0';
				
				/* nobody should be stupid enough to have spaces
				 * at the end of the file name */
				strip_whitespaces(fbuf);
			}
		
		} else {
			/* we have a filename only. */
#if (sfv_cleanup == TRUE)
			/* do stuff  */
#else
			retval = 1;
			goto END;
#endif
		}

		/* we assume what's left is a filename */
		if (strlen(fbuf) > 0) {
			strlcpy(sd.fname, fbuf, PATH_MAX);

#if (sfv_calc_single_fname == TRUE)
			/* TODO */
			/* calculate file's crc if it exists */
			if (sd.crc32 == 0) {
				d_log("Got filename (%s) without crc, trying to calculate.\n", sd.fname);
				sd.crc32 = calc_crc32(sd.fname);
			}
#endif
			
			/* get file extension */
			ptr = find_last_of(fbuf, ".");
			if (*ptr == '.')
				ptr++;
			
			if (!strcomp(ignored_types, ptr)) {

#if ( sfv_dupecheck == TRUE )
				lseek(outfd, sizeof(short int), SEEK_SET);
				
				/* read from sfvdata - no parsing */
				skip = 0;
				while (read(outfd, &tempsd, sizeof(SFVDATA)))
					if (strcmp(sd.fname, tempsd.fname) == 0)
						skip = 1;
						
				lseek(outfd, 0L, SEEK_END);

				if (skip)
					continue;
#endif

				d_log("File in sfv: '%s' (%x)\n", sd.fname, sd.crc32);

#if ( sfv_cleanup == TRUE )
				/* write good stuff to .tmpsfv */
				if (tmpfd != -1) {
					sprintf(crctmp, "%.8x", sd.crc32);
					write(tmpfd, sd.fname, strlen(sd.fname));
					write(tmpfd, " ", 1);
					write(tmpfd, crctmp, 8);
#if (sfv_cleanup_crlf == TRUE )
					write(tmpfd, "\r", 1);
#endif
					write(tmpfd, "\n", 1);
				}
#endif

				if (strncasecmp(ptr, "mp3", 4) == 0)
					music++;
				else if (israr(ptr))
					rars++;
				else if (isvideo(ptr))
					video++;
				else
					others++;
				
#if ( create_missing_files == TRUE )
				if (!findfile(dir, sd.fname))
					create_missing(sd.fname);
#endif

				write(outfd, &sd, sizeof(SFVDATA));
			}
		}
	}
	
	if (music > rars) {
		if (video > music)
			type = (video >= others ? 4 : 2);
		else
			type = (music >= others ? 3 : 2);
	} else {
		if (video > rars)
			type = (video >= others ? 4 : 2);
		else
			type = (rars >= others ? 1 : 2);
	}

#if ( sfv_cleanup == FALSE )
END:
#endif
	xunlock(&fl, infd);
	close(infd);
#if ( sfv_cleanup == TRUE )
	if (tmpfd != -1) {
		unlink(source);
		xunlock(&fl, tmpfd);
		close(tmpfd);
		rename(".tmpsfv", source);
	}
#endif
	
	closedir(dir);
	lseek(outfd, 0L, SEEK_SET);
	write(outfd, &type, sizeof(short int));
	xunlock(&fl, infd);
	close(outfd);
	
	return retval;
}

/*
 * Modified	: 01.17.2002 Author	: Dark0n3
 * 
 * Description	: Creates a file that contains list of files in release in
 * alphabetical order.
 */
void 
create_indexfile(const char *racefile, struct VARS *raceI, char *f)
{
	int		fd;
	FILE		*r;
	int		l, n, m, c;
	int		pos[raceI->total.files],
			t_pos[raceI->total.files];
	char		fname[raceI->total.files][PATH_MAX];
	
	RACEDATA	rd;

	if ((fd = open(racefile, O_RDONLY)) == -1) {
		d_log("create_indexfile: open(%s): %s\n", racefile, strerror(errno));
		exit(EXIT_FAILURE);
	}

	/* Read filenames from race file */
	c = 0;
	while ((read(fd, &rd, sizeof(RACEDATA)))) {
		if (rd.status == F_CHECKED) {
			strlcpy(fname[c], rd.fname, PATH_MAX);
			t_pos[c] = 0;
			c++;
		}
	}
	close(fd);

	/* Sort with cache */
	for (n = 0; n < c; n++) {
		m = t_pos[n];
		for (l = n + 1; l < c; l++) {
			if (strcasecmp(fname[l], fname[n]) < 0)
				m++;
			else
				t_pos[l]++;
		}
		pos[m] = n;
	}

	/* Write to file and free memory */
	if ((r = fopen(f, "w"))) {
		for (n = 0; n < c; n++) {
			m = pos[n];
			fprintf(r, "%s\n", fname[m]);
		}
		fclose(r);
	}
}

/*
 * Modified	: 01.16.2002 Author	: Dark0n3
 * 
 * Description	: Marks file as deleted.
 *
 * Obsolete?
 */
short int 
clear_file(const char *path, char *f)
{
	int		n = 0;
	FILE           *file;

	RACEDATA	rd;

	if ((file = fopen(path, "r+"))) {
		while (fread(&rd, sizeof(RACEDATA), 1, file)) {
			if (strncmp(rd.fname, f, PATH_MAX) == 0) {
				rd.status = F_DELETED;
				fseek(file, -sizeof(RACEDATA), SEEK_CUR);
				fwrite(&rd, sizeof(RACEDATA), 1, file);
				n++;
			}
		}
		fclose(file);
	}

	return n;
}

/*
 * Modified	: 02.19.2002 Author	: Dark0n3
 * 
 * Description	: Reads current race statistics from fixed format file.
 * 				: "path" is the location of a racedata file.
 */
void 
readrace(const char *path, struct VARS *raceI, struct USERINFO **userI, struct GROUPINFO **groupI)
{
	int		fd, rlength = 0;

	RACEDATA	rd;

	if ((fd = open(path, O_RDONLY)) != -1) {
		while ((rlength = read(fd, &rd, sizeof(RACEDATA)))) {
			if (rlength != sizeof(RACEDATA)) {
				d_log("readrace: Agh! racedata seems to be broken!\n");
				exit(EXIT_FAILURE);
			}
			switch (rd.status) {
				case F_NOTCHECKED:
				case F_CHECKED:
					updatestats(raceI, userI, groupI, rd.uname, rd.group,
						    rd.size, rd.speed, rd.start_time);
					break;
				case F_BAD:
					raceI->total.files_bad++;
					raceI->total.bad_size += rd.size;
					break;
				case F_NFO:
					raceI->total.nfo_present = 1;
					break;
			}
		}
		close(fd);
	}
}

/*
 * Modified	: 01.18.2002 Author	: Dark0n3
 * 
 * Description	: Writes stuff into race file.
 */
void 
writerace(const char *path, struct VARS *raceI, unsigned int crc, unsigned char status)
{
	int		fd;
	struct flock	fl;

	RACEDATA	rd;

	/* create file if it doesn't exist */
	if ((fd = open(path, O_CREAT | O_RDWR, 0666)) == -1) {
		if (errno != EEXIST) {
			d_log("writerace: open(%s): %s\n", path, strerror(errno));
			exit(EXIT_FAILURE);
		}
	}

	xlock(&fl, fd);
	
	/* find an existing entry that we will overwrite */
	while (read(fd, &rd, sizeof(RACEDATA))) {
		if (strncmp(rd.fname, raceI->file.name, PATH_MAX) == 0) {
			lseek(fd, -sizeof(RACEDATA), SEEK_CUR);
			break;
		}
	}

	rd.status = status;
	rd.crc32 = crc;
	
	strlcpy(rd.fname, raceI->file.name, PATH_MAX);
	strlcpy(rd.uname, raceI->user.name, 24);
	strlcpy(rd.group, raceI->user.group, 24);
	
	rd.size = raceI->file.size;
	rd.speed = raceI->file.speed;
	rd.start_time = raceI->total.start_time;

	write(fd, &rd, sizeof(RACEDATA));
	
	xunlock(&fl, fd);

	close(fd);
}

/* remove file entry from racedata file */
void
remove_from_race(const char *path, const char *f)
{
	int		fd, tmpfd;
	char		tmppath[PATH_MAX];
	struct flock	fl;
	
	RACEDATA	rd;
	
	if ((fd = open(path, O_RDWR)) == -1) {
		d_log("remove_from_race: open(%s): %s\n", path, strerror(errno));
		return;
	}
	
	sprintf(tmppath, "%s/rd.%s.tmp", storage, f);

	if ((tmpfd = open(tmppath, O_CREAT | O_RDWR, 0666)) == -1) {
		d_log("remove_from_race: open(%s): %s\n", tmppath, strerror(errno));
		exit(EXIT_FAILURE);
	}

	xlock(&fl, fd);
	xlock(&fl, tmpfd);

	while ((read(fd, &rd, sizeof(RACEDATA))))
		if (strcmp(rd.fname, f) != 0)
			write(tmpfd, &rd, sizeof(RACEDATA));
	
	xunlock(&fl, fd);
	xunlock(&fl, tmpfd);

	close(fd);
	close(tmpfd);

	if (rename(tmppath, path) == -1) {
		d_log("remove_from_race: rename(\"%s\", \"%s\"): %s", tmppath, path, strerror(errno));
		exit(EXIT_FAILURE);
	}

}

int
verify_racedata(const char *path)
{
	int		fd, tmpfd;
	char		tmppath[PATH_MAX];
	struct flock	fl;
	
	RACEDATA	rd;
	
	if ((fd = open(path, O_RDWR)) == -1) {
		d_log("verify_racedata: open(%s): %s\n", path, strerror(errno));
		return 0;
	}
	
	sprintf(tmppath, "%s/rd.verifydata.tmp", storage);
	
	if ((tmpfd = open(tmppath, O_CREAT | O_RDWR, 0666)) == -1) {
		d_log("verify_racedata: open(%s): %s\n", tmppath, strerror(errno));
		exit(EXIT_FAILURE);
	}

	xlock(&fl, fd);
	xlock(&fl, tmpfd);

	while ((read(fd, &rd, sizeof(RACEDATA)))) {
		if (fileexists(rd.fname))
			write(tmpfd, &rd, sizeof(RACEDATA));
		else {
			d_log("verify_racedata: Oops! %s is missing - removing from racedata\n", rd.fname);
			create_missing(rd.fname);
		}
	}
	
	xunlock(&fl, fd);
	xunlock(&fl, tmpfd);

	close(fd);
	close(tmpfd);

	if (rename(tmppath, path) == -1) {
		d_log("verify_racedata: rename(\"%s\", \"%s\"): %s", tmppath, path, strerror(errno));
		exit(EXIT_FAILURE);
	}
	return 1;
}

