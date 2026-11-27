/* smbls: tiny CLI over the RetroArch SMB VFS backend, for checking a share from adb.
 *   smbls <smb://dir>            list a directory ("D name" / "F size name")
 *   smbls -r <smb://dir>         list recursively (full paths)
 *   smbls -cat <smb://file> > out  copy a file to stdout
 *   smbls -c < paths.txt         check existence; prints MISSING <path> for each absent file
 *                                ("#member" suffixes of archive paths are ignored) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <libretro.h>
#include <vfs/vfs_implementation_smb.h>

static void list(const char *dir, int rec)
{
   retro_vfs_smb_dir_t *d = retro_vfs_smb_opendir(dir, true);
   if (!d) { fprintf(stderr, "cannot open %s\n", dir); return; }
   while (retro_vfs_smb_readdir(d))
   {
      char p[4096];
      const char *n = retro_vfs_smb_dirent_get_name(d);
      int isdir = retro_vfs_smb_dirent_is_dir(d);
      snprintf(p, sizeof(p), "%s/%s", dir, n);
      if (rec)
      {
         if (isdir) list(p, 1);
         else printf("%s\n", p);
      }
      else if (isdir) printf("D %s\n", n);
      else { int64_t sz = 0; retro_vfs_smb_stat_64(p, &sz, NULL); printf("F %lld %s\n", (long long)sz, n); }
   }
   retro_vfs_smb_closedir(d);
}

/* print "crc size name" for each member of a .zip on the share (reads only the central directory) */
static int ziplist(const char *path)
{
   static uint8_t buf[1 << 20];
   int64_t size, tail, n, i, cd_off, cd_size, cd_n;
   retro_vfs_smb_file_t *f = retro_vfs_smb_file_open(path, RETRO_VFS_FILE_ACCESS_READ);
   if (!f) return 1;
   size = retro_vfs_smb_file_size(f);
   tail = size < 65557 ? size : 65557;
   retro_vfs_smb_file_seek(f, size - tail, RETRO_VFS_SEEK_POSITION_START);
   n = retro_vfs_smb_file_read(f, buf, tail);
   for (i = n - 22; i >= 0; i--)
      if (buf[i]==0x50 && buf[i+1]==0x4b && buf[i+2]==5 && buf[i+3]==6) break;
   if (i < 0) { retro_vfs_smb_file_close(f); return 2; }
   cd_n    = buf[i+10] | (buf[i+11]<<8);
   cd_size = (int64_t)(buf[i+12] | (buf[i+13]<<8) | (buf[i+14]<<16) | ((uint32_t)buf[i+15]<<24));
   cd_off  = (int64_t)(buf[i+16] | (buf[i+17]<<8) | (buf[i+18]<<16) | ((uint32_t)buf[i+19]<<24));
   if (cd_size > (int64_t)sizeof(buf)) { retro_vfs_smb_file_close(f); return 3; }
   retro_vfs_smb_file_seek(f, cd_off, RETRO_VFS_SEEK_POSITION_START);
   n = retro_vfs_smb_file_read(f, buf, cd_size);
   for (i = 0; i + 46 <= n && cd_n-- > 0; )
   {
      uint32_t crc  = buf[i+16] | (buf[i+17]<<8) | (buf[i+18]<<16) | ((uint32_t)buf[i+19]<<24);
      uint32_t usz  = buf[i+24] | (buf[i+25]<<8) | (buf[i+26]<<16) | ((uint32_t)buf[i+27]<<24);
      int nl = buf[i+28] | (buf[i+29]<<8), el = buf[i+30] | (buf[i+31]<<8), cl = buf[i+32] | (buf[i+33]<<8);
      printf("%08x %u %.*s\n", crc, usz, nl, (char*)buf + i + 46);
      i += 46 + nl + el + cl;
   }
   retro_vfs_smb_file_close(f);
   return 0;
}

int main(int argc, char **argv)
{
   if (argc >= 3 && !strcmp(argv[1], "-zip"))
   {
      /* -zip dir: list every .zip in dir as "== name" followed by members */
      retro_vfs_smb_dir_t *d = retro_vfs_smb_opendir(argv[2], true);
      if (!d) return 1;
      while (retro_vfs_smb_readdir(d))
      {
         char p[4096]; const char *nm = retro_vfs_smb_dirent_get_name(d); size_t l = strlen(nm);
         if (retro_vfs_smb_dirent_is_dir(d) || l < 4 || strcasecmp(nm + l - 4, ".zip")) continue;
         snprintf(p, sizeof(p), "%s/%s", argv[2], nm);
         printf("== %s\n", nm); fflush(stdout);
         if (ziplist(p)) printf("!! unreadable\n");
      }
      retro_vfs_smb_closedir(d);
      return 0;
   }
   if (argc >= 2 && !strcmp(argv[1], "-c"))
   {
      char line[4096]; int miss = 0, total = 0;
      while (fgets(line, sizeof(line), stdin))
      {
         char *h; size_t n = strlen(line);
         while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
         if (!n) continue;
         if ((h = strstr(line, ".zip#")) || (h = strstr(line, ".7z#")) || (h = strstr(line, ".ZIP#")))
            *strchr(h, '#') = 0;
         total++;
         if (!(retro_vfs_smb_stat(line, NULL) & RETRO_VFS_STAT_IS_VALID)) { printf("MISSING %s\n", line); miss++; }
      }
      printf("checked %d, missing %d\n", total, miss);
      return 0;
   }
   if (argc >= 3 && !strcmp(argv[1], "-cat"))
   {
      /* copy a file from the share to stdout */
      char buf[65536]; int64_t n;
      retro_vfs_smb_file_t *f = retro_vfs_smb_file_open(argv[2], RETRO_VFS_FILE_ACCESS_READ);
      if (!f) { fprintf(stderr, "cannot open %s\n", argv[2]); return 1; }
      while ((n = retro_vfs_smb_file_read(f, buf, sizeof(buf))) > 0)
         fwrite(buf, 1, (size_t)n, stdout);
      retro_vfs_smb_file_close(f);
      return 0;
   }
   if (argc >= 3 && !strcmp(argv[1], "-r")) { list(argv[2], 1); return 0; }
   if (argc >= 2) { list(argv[1], 0); return 0; }
   fprintf(stderr, "usage: smbls [-r] smb://server/share/dir | smbls -c < paths\n");
   return 1;
}
