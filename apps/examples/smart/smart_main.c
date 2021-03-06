/****************************************************************************
 * examples/smart/smart_main.c
 *
 *   Copyright (C) 2011, 2013 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/mount.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <crc32.h>
#include <debug.h>

#include <nuttx/mtd.h>
#include <nuttx/smart.h>
#include <nuttx/fs/ioctl.h>
#include <nuttx/fs/mksmartfs.h>

/****************************************************************************
 * Definitions
 ****************************************************************************/
/* Configuration ************************************************************/
/* The default is to use the RAM MTD device at drivers/mtd/rammtd.c.  But
 * an architecture-specific MTD driver can be used instead by defining
 * CONFIG_EXAMPLES_SMART_ARCHINIT.  In this case, the initialization logic
 * will call smart_archinitialize() to obtain the MTD driver instance.
 */

#ifndef CONFIG_EXAMPLES_SMART_ARCHINIT

/* This must exactly match the default configuration in drivers/mtd/rammtd.c */

#  ifndef CONFIG_RAMMTD_ERASESIZE
#    define CONFIG_RAMMTD_ERASESIZE 4096
#  endif

#  ifndef CONFIG_EXAMPLES_SMART_NEBLOCKS
#    define CONFIG_EXAMPLES_SMART_NEBLOCKS (256)
#  endif

#  define EXAMPLES_SMART_BUFSIZE \
  (CONFIG_RAMMTD_ERASESIZE * CONFIG_EXAMPLES_SMART_NEBLOCKS)
#endif

#ifndef CONFIG_EXAMPLES_SMART_MAXNAME
#  define CONFIG_EXAMPLES_SMART_MAXNAME 128
#endif

#if CONFIG_EXAMPLES_SMART_MAXNAME > 255
#  undef CONFIG_EXAMPLES_SMART_MAXNAME
#  define CONFIG_EXAMPLES_SMART_MAXNAME 255
#endif

#ifndef CONFIG_EXAMPLES_SMART_MAXFILE
#  define CONFIG_EXAMPLES_SMART_MAXFILE 8192
#endif

#ifndef CONFIG_EXAMPLES_SMART_MAXIO
#  define CONFIG_EXAMPLES_SMART_MAXIO 347
#endif

#ifndef CONFIG_EXAMPLES_SMART_MAXOPEN
#  define CONFIG_EXAMPLES_SMART_MAXOPEN 512
#endif

#ifndef CONFIG_EXAMPLES_SMART_MOUNTPT
#  define CONFIG_EXAMPLES_SMART_MOUNTPT "/mnt/smart"
#endif

#ifndef CONFIG_EXAMPLES_SMART_NLOOPS
#  define CONFIG_EXAMPLES_SMART_NLOOPS 100
#endif

#ifndef CONFIG_EXAMPLES_SMART_VERBOSE
#  define CONFIG_EXAMPLES_SMART_VERBOSE 0
#endif

#if defined(CONFIG_DEBUG) && defined(CONFIG_DEBUG_FS)
#  define message    syslog
#  define msgflush()
#else
#  define message    printf
#  define msgflush() fflush(stdout);
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct smart_filedesc_s
{
  FAR char *name;
  bool deleted;
  size_t len;
  uint32_t crc;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/
/* Pre-allocated simulated flash */

#ifndef CONFIG_EXAMPLES_SMART_ARCHINIT
static uint8_t g_simflash[EXAMPLES_SMART_BUFSIZE];
#endif

static uint8_t g_fileimage[CONFIG_EXAMPLES_SMART_MAXFILE];
static struct smart_filedesc_s g_files[CONFIG_EXAMPLES_SMART_MAXOPEN];
static const char g_mountdir[] = CONFIG_EXAMPLES_SMART_MOUNTPT "/";
static int g_nfiles;
static int g_ndeleted;

static struct mallinfo g_mmbefore;
static struct mallinfo g_mmprevious;
static struct mallinfo g_mmafter;

/****************************************************************************
 * External Functions
 ****************************************************************************/

#ifdef CONFIG_EXAMPLES_SMART_ARCHINIT
extern FAR struct mtd_dev_s *smart_archinitialize(void);
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: smart_memusage
 ****************************************************************************/

static void smart_showmemusage(struct mallinfo *mmbefore,
                               struct mallinfo *mmafter)
{
  message("VARIABLE  BEFORE   AFTER\n");
  message("======== ======== ========\n");
  message("arena    %8x %8x\n", mmbefore->arena,    mmafter->arena);
  message("ordblks  %8d %8d\n", mmbefore->ordblks,  mmafter->ordblks);
  message("mxordblk %8x %8x\n", mmbefore->mxordblk, mmafter->mxordblk);
  message("uordblks %8x %8x\n", mmbefore->uordblks, mmafter->uordblks);
  message("fordblks %8x %8x\n", mmbefore->fordblks, mmafter->fordblks);
}

/****************************************************************************
 * Name: smart_loopmemusage
 ****************************************************************************/

static void smart_loopmemusage(void)
{
  /* Get the current memory usage */

#ifdef CONFIG_CAN_PASS_STRUCTS
  g_mmafter = mallinfo();
#else
  (void)mallinfo(&g_mmafter);
#endif

  /* Show the change from the previous loop */

  message("\nEnd of loop memory usage:\n");
  smart_showmemusage(&g_mmprevious, &g_mmafter);

  /* Set up for the next test */

#ifdef CONFIG_CAN_PASS_STRUCTS
  g_mmprevious = g_mmafter;
#else
  memcpy(&g_mmprevious, &g_mmafter, sizeof(struct mallinfo));
#endif
}

/****************************************************************************
 * Name: smart_endmemusage
 ****************************************************************************/

static void smart_endmemusage(void)
{
#ifdef CONFIG_CAN_PASS_STRUCTS
  g_mmafter = mallinfo();
#else
  (void)mallinfo(&g_mmafter);
#endif
  message("\nFinal memory usage:\n");
  smart_showmemusage(&g_mmbefore, &g_mmafter);
}

/****************************************************************************
 * Name: smart_randchar
 ****************************************************************************/

static inline char smart_randchar(void)
{
  int value = rand() % 63;
  if (value == 0)
    {
      return '0';
    }
  else if (value <= 10)
    {
      return value + '0' - 1;
    }
  else if (value <= 36)
    {
      return value + 'a' - 11;
    }
  else /* if (value <= 62) */
    {
      return value + 'A' - 37;
    }
}

/****************************************************************************
 * Name: smart_randname
 ****************************************************************************/

static inline void smart_randname(FAR struct smart_filedesc_s *file)
{
  int dirlen;
  int maxname;
  int namelen;
  int alloclen;
  int i;

  dirlen   = strlen(g_mountdir);
  maxname  = CONFIG_EXAMPLES_SMART_MAXNAME - dirlen;
  namelen  = (rand() % maxname) + 1;
  alloclen = namelen + dirlen;

  file->name = (FAR char*)malloc(alloclen + 1);
  if (!file->name)
    {
      message("ERROR: Failed to allocate name, length=%d\n", namelen);
      msgflush();
      exit(5);
    }

  memcpy(file->name, g_mountdir, dirlen);
  for (i = dirlen; i < alloclen; i++)
    {
      file->name[i] = smart_randchar();
    }
 
  file->name[alloclen] = '\0';
}

/****************************************************************************
 * Name: smart_randfile
 ****************************************************************************/

static inline void smart_randfile(FAR struct smart_filedesc_s *file)
{
  int i;

  file->len = (rand() % CONFIG_EXAMPLES_SMART_MAXFILE) + 1;
  for (i = 0; i < file->len; i++)
    {
      g_fileimage[i] = smart_randchar();
    }

  file->crc = crc32(g_fileimage, file->len);
}

/****************************************************************************
 * Name: smart_freefile
 ****************************************************************************/

static void smart_freefile(FAR struct smart_filedesc_s *file)
{
  if (file->name)
    {
      free(file->name);
    }

  memset(file, 0, sizeof(struct smart_filedesc_s));
}

/****************************************************************************
 * Name: smart_wrfile
 ****************************************************************************/

static inline int smart_wrfile(FAR struct smart_filedesc_s *file)
{
  size_t offset;
  int fd;
  int ret;

  /* Create a random file */

  smart_randname(file);
  smart_randfile(file);
  fd = open(file->name, O_WRONLY | O_CREAT | O_EXCL, 0666);
  if (fd < 0)
    {
      /* If it failed because there is no space on the device, then don't
       * complain.
       */

      if (errno != ENOSPC)
        {
          message("ERROR: Failed to open file for writing: %d\n", errno);
          message("  File name: %s\n", file->name);
          message("  File size: %d\n", file->len);
        }

      smart_freefile(file);
      return ERROR;
    }

  /* Write a random amount of data to the file */

  for (offset = 0; offset < file->len; )
    {
      size_t maxio = (rand() % CONFIG_EXAMPLES_SMART_MAXIO) + 1;
      size_t nbytestowrite = file->len - offset;
      ssize_t nbyteswritten;

      if (nbytestowrite > maxio)
        {
          nbytestowrite = maxio;
        }

      nbyteswritten = write(fd, &g_fileimage[offset], nbytestowrite);
      if (nbyteswritten < 0)
        {
          int err = errno;

          /* If the write failed because there is no space on the device,
           * then don't complain.
           */

          if (err != ENOSPC)
            {
              message("ERROR: Failed to write file: %d\n", err);
              message("  File name:    %s\n", file->name);
              message("  File size:    %d\n", file->len);
              message("  Write offset: %d\n", offset);
              message("  Write size:   %d\n", nbytestowrite);
              ret = ERROR;
            }
          close(fd);

          /* Remove any garbage file that might have been left behind */

          ret = unlink(file->name);
          if (ret < 0)
            {
              message("  Failed to remove partial file\n");
            }
          else
            {
#if CONFIG_EXAMPLES_SMART_VERBOSE != 0
              message("  Successfully removed partial file\n");
#endif
            }

          smart_freefile(file);
          return ERROR;
        }
      else if (nbyteswritten != nbytestowrite)
        {
          message("ERROR: Partial write:\n");
          message("  File name:    %s\n", file->name);
          message("  File size:    %d\n", file->len);
          message("  Write offset: %d\n", offset);
          message("  Write size:   %d\n", nbytestowrite);
          message("  Written:      %d\n", nbyteswritten);
        }

      offset += nbyteswritten;
    }

  close(fd);
  return OK;
}

/****************************************************************************
 * Name: smart_fillfs
 ****************************************************************************/

static int smart_fillfs(void)
{
  FAR struct smart_filedesc_s *file;
  int ret;
  int i;

  /* Create a file for each unused file structure */

  for (i = 0; i < CONFIG_EXAMPLES_SMART_MAXOPEN; i++)
    {
      file = &g_files[i];
      if (file->name == NULL)
        {
          ret = smart_wrfile(file);
          if (ret < 0)
            {
#if CONFIG_EXAMPLES_SMART_VERBOSE != 0
              message("ERROR: Failed to write file %d\n", i);
#endif
              return ERROR;
            }

#if CONFIG_EXAMPLES_SMART_VERBOSE != 0
         message("  Created file %s\n", file->name);
#endif
         g_nfiles++;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: smart_rdblock
 ****************************************************************************/

static ssize_t smart_rdblock(int fd, FAR struct smart_filedesc_s *file,
                             size_t offset, size_t len)
{
  size_t maxio = (rand() % CONFIG_EXAMPLES_SMART_MAXIO) + 1;
  ssize_t nbytesread;

  if (len > maxio)
    {
      len = maxio;
    }

  nbytesread = read(fd, &g_fileimage[offset], len);
  if (nbytesread < 0)
    {
      message("ERROR: Failed to read file: %d\n", errno);
      message("  File name:    %s\n", file->name);
      message("  File size:    %d\n", file->len);
      message("  Read offset:  %d\n", offset);
      message("  Read size:    %d\n", len);
      return ERROR;
    }
  else if (nbytesread == 0)
    {
#if 0 /* No... we do this on purpose sometimes */
      message("ERROR: Unexpected end-of-file:\n");
      message("  File name:    %s\n", file->name);
      message("  File size:    %d\n", file->len);
      message("  Read offset:  %d\n", offset);
      message("  Read size:    %d\n", len);
#endif
      return ERROR;
    }
  else if (nbytesread != len)
    {
      message("ERROR: Partial read:\n");
      message("  File name:    %s\n", file->name);
      message("  File size:    %d\n", file->len);
      message("  Read offset:  %d\n", offset);
      message("  Read size:    %d\n", len);
      message("  Bytes read:   %d\n", nbytesread);
    }

  return nbytesread;
}

/****************************************************************************
 * Name: smart_rdfile
 ****************************************************************************/

static inline int smart_rdfile(FAR struct smart_filedesc_s *file)
{
  size_t ntotalread;
  ssize_t nbytesread;
  uint32_t crc;
  int fd;

  /* Open the file for reading */

  fd = open(file->name, O_RDONLY);
  if (fd < 0)
    {
      if (!file->deleted)
        {
          message("ERROR: Failed to open file for reading: %d\n", errno);
          message("  File name: %s\n", file->name);
          message("  File size: %d\n", file->len);
        }

      return ERROR;
    }

  /* Read all of the data info the fileimage buffer using random read sizes */

  for (ntotalread = 0; ntotalread < file->len; )
    {
      nbytesread = smart_rdblock(fd, file, ntotalread, file->len - ntotalread);
      if (nbytesread < 0)
        {
          close(fd);
          return ERROR;
        }

      ntotalread += nbytesread;
    }

  /* Verify the file image CRC */

  crc = crc32(g_fileimage, file->len);
  if (crc != file->crc)
    {
      message("ERROR: Bad CRC: %d vs %d\n", crc, file->crc);
      message("  File name: %s\n", file->name);
      message("  File size: %d\n", file->len);
      close(fd);
      return ERROR;
    }

  /* Try reading past the end of the file */

  nbytesread = smart_rdblock(fd, file, ntotalread, 1024) ;
  if (nbytesread > 0)
    {
      message("ERROR: Read past the end of file\n");
      message("  File name:  %s\n", file->name);
      message("  File size:  %d\n", file->len);
      message("  Bytes read: %d\n", nbytesread);
      close(fd);
      return ERROR;
    }

  close(fd);
  return OK;
}

/****************************************************************************
 * Name: smart_verifyfs
 ****************************************************************************/

static int smart_verifyfs(void)
{
  FAR struct smart_filedesc_s *file;
  int ret;
  int i;

  /* Create a file for each unused file structure */

  for (i = 0; i < CONFIG_EXAMPLES_SMART_MAXOPEN; i++)
    {
      file = &g_files[i];
      if (file->name != NULL)
        {
          ret = smart_rdfile(file);
          if (ret < 0)
            {
              if (file->deleted)
                {
#if CONFIG_EXAMPLES_SMART_VERBOSE != 0
                  message("Deleted file %d OK\n", i);
#endif
                  smart_freefile(file);
                  g_ndeleted--;
                  g_nfiles--;
                }
              else
                {
                  message("ERROR: Failed to read a file: %d\n", i);
                  message("  File name: %s\n", file->name);
                  message("  File size: %d\n", file->len);
                  return ERROR;
                }
            }
          else
            {
              if (file->deleted)
                {
#if CONFIG_EXAMPLES_SMART_VERBOSE != 0
                  message("Succesffully read a deleted file\n");
                  message("  File name: %s\n", file->name);
                  message("  File size: %d\n", file->len);
#endif
                  smart_freefile(file);
                  g_ndeleted--;
                  g_nfiles--;
                  return ERROR;
                }
              else
                {
#if CONFIG_EXAMPLES_SMART_VERBOSE != 0
                  message("  Verifed file %s\n", file->name);
#endif
                }
            }
        }
    }

  return OK;
}

/****************************************************************************
 * Name: smart_delfiles
 ****************************************************************************/

static int smart_delfiles(void)
{
  FAR struct smart_filedesc_s *file;
  int ndel;
  int ret;
  int i;
  int j;

  /* Are there any files to be deleted? */

  int nfiles = g_nfiles - g_ndeleted;
  if (nfiles < 1)
    {
      return 0;
    }

  /* Yes... How many files should we delete? */

  ndel = (rand() % nfiles) + 1;

  /* Now pick which files to delete */

  for (i = 0; i < ndel; i++)
    {
      /* Guess a file index */

      int ndx = (rand() % (g_nfiles - g_ndeleted));

      /* And delete the next undeleted file after that random index */

      for (j = ndx + 1; j != ndx;)
        {
          file = &g_files[j];
          if (file->name && !file->deleted)
            {
              ret = unlink(file->name);
              if (ret < 0)
                {
                  message("ERROR: Unlink %d failed: %d\n", i+1, errno);
                  message("  File name:  %s\n", file->name);
                  message("  File size:  %d\n", file->len);
                  message("  File index: %d\n", j);
                }
              else
                {
#if CONFIG_EXAMPLES_SMART_VERBOSE != 0
                  message("  Deleted file %s\n", file->name);
#endif
                  file->deleted = true;
                  g_ndeleted++;
                  break;
                }
            }

          /* Increment the index and test for wrap-around */

          if (++j >= CONFIG_EXAMPLES_SMART_MAXOPEN)
            {
              j = 0;
            }

        }
    }

  return OK;
}

/****************************************************************************
 * Name: smart_delallfiles
 ****************************************************************************/

static int smart_delallfiles(void)
{
  FAR struct smart_filedesc_s *file;
  int ret;
  int i;

  for (i = 0; i < CONFIG_EXAMPLES_SMART_MAXOPEN; i++)
    {
      file = &g_files[i];
      if (file->name)
        {
          ret = unlink(file->name);
          if (ret < 0)
            {
               message("ERROR: Unlink %d failed: %d\n", i+1, errno);
               message("  File name:  %s\n", file->name);
               message("  File size:  %d\n", file->len);
               message("  File index: %d\n", i);
            }
          else
            {
#if CONFIG_EXAMPLES_SMART_VERBOSE != 0
              message("  Deleted file %s\n", file->name);
#endif
              smart_freefile(file);
            }
        }
    }

  g_nfiles = 0;
  g_ndeleted = 0;
  return OK;
}

/****************************************************************************
 * Name: smart_directory
 ****************************************************************************/

static int smart_directory(void)
{
  DIR *dirp;
  FAR struct dirent *entryp;
  int number;

  /* Open the directory */

  dirp = opendir(CONFIG_EXAMPLES_SMART_MOUNTPT);

  if (!dirp)
    {
      /* Failed to open the directory */

      message("ERROR: Failed to open directory '%s': %d\n",
              CONFIG_EXAMPLES_SMART_MOUNTPT, errno);
      return ERROR;
    }

  /* Read each directory entry */

  message("Directory:\n");
  number = 1;
  do
    {
      entryp = readdir(dirp);
      if (entryp)
        {
          message("%2d. Type[%d]: %s Name: %s\n",
                  number, entryp->d_type,
                  entryp->d_type == DTYPE_FILE ? "File " : "Error",
                  entryp->d_name);
        }

      number++;
    }
  while (entryp != NULL);

  closedir(dirp);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: smart_main
 ****************************************************************************/

int smart_main(int argc, char *argv[])
{
  FAR struct mtd_dev_s *mtd;
  unsigned int i;
  int ret;

  /* Seed the random number generated */

  srand(0x93846);

  /* Create and initialize a RAM MTD device instance */

#ifdef CONFIG_EXAMPLES_SMART_ARCHINIT
  mtd = smart_archinitialize();
#else
  mtd = rammtd_initialize(g_simflash, EXAMPLES_SMART_BUFSIZE);
#endif
  if (!mtd)
    {
      message("ERROR: Failed to create RAM MTD instance\n");
      msgflush();
      exit(1);
    }

  /* Initialize to provide SMART on an MTD interface */

  MTD_IOCTL(mtd, MTDIOC_BULKERASE, 0);
  ret = smart_initialize(1, mtd);
  if (ret < 0)
    {
      message("ERROR: SMART initialization failed: %d\n", -ret);
      msgflush();
      exit(2);
    }

  /* Creaet a SMARTFS filesystem */

  ret = mksmartfs("/dev/smart1");

  /* Mount the file system */

  ret = mount("/dev/smart1", CONFIG_EXAMPLES_SMART_MOUNTPT, "smartfs", 0, NULL);
  if (ret < 0)
    {
      message("ERROR: Failed to mount the SMART volume: %d\n", errno);
      msgflush();
      exit(3);
    }

  /* Set up memory monitoring */

#ifdef CONFIG_CAN_PASS_STRUCTS
  g_mmbefore = mallinfo();
  g_mmprevious = g_mmbefore;
#else
  (void)mallinfo(&g_mmbefore);
  memcpy(&g_mmprevious, &g_mmbefore, sizeof(struct mallinfo));
#endif

  /* Loop a few times ... file the file system with some random, files,
   * delete some files randomly, fill the file system with more random file,
   * delete, etc.  This beats the FLASH very hard!
   */

#if CONFIG_EXAMPLES_SMART_NLOOPS == 0
  for (i = 0; ; i++)
#else
  for (i = 1; i <= CONFIG_EXAMPLES_SMART_NLOOPS; i++)
#endif
    {
      /* Write a files to the SMART file system until either (1) all of the
       * open file structures are utilized or until (2) SMART reports an error
       * (hopefully that the file system is full)
       */

      message("\n=== FILLING %d =============================\n", i);
      ret = smart_fillfs();
      message("Filled file system\n");
      message("  Number of files: %d\n", g_nfiles);
      message("  Number deleted:  %d\n", g_ndeleted);

      /* Directory listing */

      smart_directory();

      /* Verify all files written to FLASH */

      ret = smart_verifyfs();
      if (ret < 0)
        {
          message("ERROR: Failed to verify files\n");
          message("  Number of files: %d\n", g_nfiles);
          message("  Number deleted:  %d\n", g_ndeleted);
        }
      else
        {
#if CONFIG_EXAMPLES_SMART_VERBOSE != 0
          message("Verified!\n");
          message("  Number of files: %d\n", g_nfiles);
          message("  Number deleted:  %d\n", g_ndeleted);
#endif
        }

      /* Delete some files */

      message("\n=== DELETING %d ============================\n", i);
      ret = smart_delfiles();
      if (ret < 0)
        {
          message("ERROR: Failed to delete files\n");
          message("  Number of files: %d\n", g_nfiles);
          message("  Number deleted:  %d\n", g_ndeleted);
        }
      else
        {
          message("Deleted some files\n");
          message("  Number of files: %d\n", g_nfiles);
          message("  Number deleted:  %d\n", g_ndeleted);
        }

      /* Directory listing */

      smart_directory();

      /* Verify all files written to FLASH */

      ret = smart_verifyfs();
      if (ret < 0)
        {
          message("ERROR: Failed to verify files\n");
          message("  Number of files: %d\n", g_nfiles);
          message("  Number deleted:  %d\n", g_ndeleted);
        }
      else
        {
#if CONFIG_EXAMPLES_SMART_VERBOSE != 0
          message("Verified!\n");
          message("  Number of files: %d\n", g_nfiles);
          message("  Number deleted:  %d\n", g_ndeleted);
#endif
        }

      /* Show memory usage */

      smart_loopmemusage();
      msgflush();
    }

  /* Delete all files then show memory usage again */

  smart_delallfiles();
  smart_endmemusage();
  msgflush();
  return 0;
}

