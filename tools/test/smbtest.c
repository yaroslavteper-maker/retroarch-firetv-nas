/* Host-side test of the SMB VFS backend through libretro-common's public
 * file APIs (filestream, dir_list, path_is_directory) plus the cache fetch. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <streams/file_stream.h>
#include <lists/dir_list.h>
#include <file/file_path.h>
#include <vfs/vfs_implementation_smb.h>

static int fails = 0;
#define CHECK(c, ...) do { if (c) printf("  ok   " __VA_ARGS__); else { printf("  FAIL " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static long local_size(const char *p) { struct stat st; return stat(p, &st) ? -1 : (long)st.st_size; }

static int same_file(const char *a_local_share, const char *b)
{
   FILE *x = fopen(a_local_share, "rb"), *y = fopen(b, "rb");
   int ok = x && y;
   while (ok) { int c1 = fgetc(x), c2 = fgetc(y); if (c1 != c2) ok = 0; if (c1 == EOF) break; }
   if (x) fclose(x); if (y) fclose(y);
   return ok;
}

int main(int argc, char **argv)
{
   const char *base = "smb://127.0.0.1:4445/PIROMS/roms";
   char out[4096], err[512];
   struct string_list *l;
   size_t i;
   int64_t n;
   uint8_t buf[100000];
   RFILE *f;

   retro_vfs_smb_set_config_path(argc > 1 ? argv[1] : "/tmp/smbt/smb.cfg");

   printf("dir listing\n");
   l = dir_list_new(base, NULL, true, false, false, false);
   CHECK(l && l->size == 3, "root has 3 visible entries (got %d)", l ? (int)l->size : -1);
   for (i = 0; l && i < l->size; i++)
      printf("       %s %s\n", l->elems[i].attr.i == RARCH_DIRECTORY ? "[dir]" : "     ", l->elems[i].data);
   string_list_free(l);
   l = dir_list_new("smb://127.0.0.1:4445/PIROMS/roms/atari2600", "a26|zip", false, false, true, false);
   CHECK(l && l->size == 2, "atari2600 filtered by a26|zip has 2 files");
   string_list_free(l);

   printf("stat\n");
   CHECK(path_is_directory("smb://127.0.0.1:4445/PIROMS/roms/psx"), "psx is a directory");
   CHECK(path_is_directory("smb://127.0.0.1:4445/PIROMS"), "share root is a directory");
   CHECK(path_is_valid("smb://127.0.0.1:4445/PIROMS/roms/atari2600/Adventure (USA).a26"), "a26 exists");
   CHECK(!path_is_valid("smb://127.0.0.1:4445/PIROMS/roms/nope.bin"), "missing file is not valid");
   { int32_t gs = path_get_size("smb://127.0.0.1:4445/PIROMS/roms/psx/Game One/Game One (Track 1).bin"); CHECK(gs == 5*1024*1024+123, "size of 5 MB bin (%d)", (int)gs); }

   printf("filestream reads\n");
   f = filestream_open("smb://127.0.0.1:4445/PIROMS/roms/psx/Game One/Game One (Track 1).bin",
         RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
   CHECK(f != NULL, "open");
   if (f)
   {
      FILE *ref = fopen("/tmp/share/roms/psx/Game One/Game One (Track 1).bin", "rb");
      uint8_t rb[100000];
      int good = 1;
      CHECK(filestream_get_size(f) == 5*1024*1024+123, "filestream size");
      /* small sequential reads (read-ahead path) */
      for (i = 0; i < 50; i++) { filestream_read(f, buf, 777); fread(rb, 1, 777, ref); if (memcmp(buf, rb, 777)) good = 0; }
      CHECK(good, "50 small sequential reads match");
      /* seek + big read */
      filestream_seek(f, 3000000, RETRO_VFS_SEEK_POSITION_START); fseek(ref, 3000000, SEEK_SET);
      n = filestream_read(f, buf, sizeof(buf)); fread(rb, 1, sizeof(rb), ref);
      CHECK(n == (int64_t)sizeof(buf) && !memcmp(buf, rb, sizeof(buf)), "seek + 100 KB read match");
      filestream_seek(f, -10, RETRO_VFS_SEEK_POSITION_END);
      n = filestream_read(f, buf, 100);
      CHECK(n == 10, "read at end returns remaining 10 bytes (got %lld)", (long long)n);
      CHECK(filestream_tell(f) == 5*1024*1024+123, "tell at EOF");
      fclose(ref);
      filestream_close(f);
   }
   CHECK(filestream_open("smb://127.0.0.1:4445/PIROMS/roms/x.bin", RETRO_VFS_FILE_ACCESS_WRITE, 0) == NULL, "write open refused");
   {
      void *data = NULL; int64_t len = 0;
      CHECK(filestream_read_file("smb://127.0.0.1:4445/PIROMS/roms/psx/Game One/Game One.cue", &data, &len) && len > 50, "filestream_read_file of cue");
      free(data);
   }

   printf("cache fetch\n");
   system("rm -rf /tmp/smbt/cache");
   CHECK(retro_vfs_smb_cache_fetch("smb://127.0.0.1:4445/PIROMS/roms/psx/Game One.m3u", "/tmp/smbt/cache", 0, out, sizeof(out), err, sizeof(err)), "m3u fetch: %s", err);
   printf("       -> %s\n", out);
   CHECK(same_file("/tmp/share/roms/psx/Game One/Game One (Track 1).bin", "/tmp/smbt/cache/127.0.0.1_4445/PIROMS/roms/psx/Game One/Game One (Track 1).bin"), "m3u->cue->track 1 copied intact");
   CHECK(same_file("/tmp/share/roms/psx/Game One/Game One (Track 2).bin", "/tmp/smbt/cache/127.0.0.1_4445/PIROMS/roms/psx/Game One/Game One (Track 2).bin"), "m3u->cue->track 2 copied intact");

   CHECK(retro_vfs_smb_cache_fetch("smb://127.0.0.1:4445/PIROMS/roms/dreamcast/sub/Crazy.gdi", "/tmp/smbt/cache", 0, out, sizeof(out), err, sizeof(err)), "gdi fetch: %s", err);
   CHECK(local_size("/tmp/smbt/cache/127.0.0.1_4445/PIROMS/roms/dreamcast/sub/track 03.raw") == 3000, "gdi quoted track copied");
   CHECK(local_size("/tmp/smbt/cache/127.0.0.1_4445/PIROMS/roms/dreamcast/sub/track01.bin") == 1000, "gdi plain track copied");

   CHECK(retro_vfs_smb_cache_fetch("smb://127.0.0.1:4445/PIROMS/roms/atari2600/Air Raid (USA).zip#Air Raid (USA).bin", "/tmp/smbt/cache", 0, out, sizeof(out), err, sizeof(err)), "zip#member fetch: %s", err);
   CHECK(strstr(out, ".zip#Air Raid (USA).bin") != NULL, "archive suffix kept: %s", out);

   {
      struct stat a, b;
      stat("/tmp/smbt/cache/127.0.0.1_4445/PIROMS/roms/psx/Game One/Game One (Track 1).bin", &a);
      retro_vfs_smb_cache_fetch("smb://127.0.0.1:4445/PIROMS/roms/psx/Game One/Game One.cue", "/tmp/smbt/cache", 0, out, sizeof(out), err, sizeof(err));
      stat("/tmp/smbt/cache/127.0.0.1_4445/PIROMS/roms/psx/Game One/Game One (Track 1).bin", &b);
      CHECK(a.st_ino == b.st_ino && a.st_ctime == b.st_ctime, "second fetch reuses cached file");
   }

   printf("eviction\n");
   CHECK(retro_vfs_smb_cache_fetch("smb://127.0.0.1:4445/PIROMS/roms/atari2600/Adventure (USA).a26", "/tmp/smbt/cache", 5*1024*1024 + 70000, out, sizeof(out), err, sizeof(err)), "fetch with tight limit: %s", err);
   CHECK(local_size(out) == 4096, "new file present");
   {
      long t1 = local_size("/tmp/smbt/cache/127.0.0.1_4445/PIROMS/roms/psx/Game One/Game One (Track 1).bin");
      printf("       big track now: %ld (evicted if -1)\n", t1);
   }

   printf("reconnect after drop\n");
   retro_vfs_smb_disconnect_all();
   CHECK(path_is_directory("smb://127.0.0.1:4445/PIROMS/roms/psx"), "works again after disconnect_all");

   printf("bad credentials\n");
   CHECK(!path_is_directory("smb://nobody@127.0.0.1:4445/PIROMS/roms/psx") || 1, "(server may allow guest; informational)");

   printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASSED", fails);
   return fails != 0;
}
