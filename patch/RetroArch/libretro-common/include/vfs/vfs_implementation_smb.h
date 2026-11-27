/* Copyright  (C) 2010-2026 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (vfs_implementation_smb.h).
 * ---------------------------------------------------------------------------------------
 *
 * Permission is hereby granted, free of charge,
 * to any person obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/* Read-only SMB2/SMB3 network share backend for the libretro VFS,
 * built on libsmb2.
 *
 * Paths look like:   smb://[user@]server[:port]/share/dir/file.ext
 *
 * Credentials that are not in the URL are read from a small text file
 * (see retro_vfs_smb_set_config_path()):
 *
 *     user           = pi
 *     password       = secret
 *     domain         = WORKGROUP   (optional)
 *     cache_limit_mb = 1536        (optional, local cache size)
 *
 * Only reading is supported: files on the share are never written,
 * renamed or deleted. Saves, states and config stay on local storage.
 */

#ifndef __LIBRETRO_SDK_VFS_IMPLEMENTATION_SMB_H
#define __LIBRETRO_SDK_VFS_IMPLEMENTATION_SMB_H

#include <stdint.h>
#include <stddef.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

typedef struct retro_vfs_smb_file retro_vfs_smb_file_t;
typedef struct retro_vfs_smb_dir  retro_vfs_smb_dir_t;

/* True if 'path' uses the smb:// scheme. */
bool retro_vfs_is_smb_path(const char *path);

/* Optional: where to read user/password/domain from.
 * If never called, a few standard locations are tried. */
void retro_vfs_smb_set_config_path(const char *path);

/* Files (read-only) */
retro_vfs_smb_file_t *retro_vfs_smb_file_open(const char *path, unsigned mode);
int      retro_vfs_smb_file_close(retro_vfs_smb_file_t *f);
int64_t  retro_vfs_smb_file_size(retro_vfs_smb_file_t *f);
int64_t  retro_vfs_smb_file_tell(retro_vfs_smb_file_t *f);
int64_t  retro_vfs_smb_file_seek(retro_vfs_smb_file_t *f, int64_t offset, int whence);
int64_t  retro_vfs_smb_file_read(retro_vfs_smb_file_t *f, void *s, uint64_t len);
int      retro_vfs_smb_file_error(retro_vfs_smb_file_t *f);

/* Returns RETRO_VFS_STAT_* flags (0 if missing); fills *size if non-NULL */
int      retro_vfs_smb_stat(const char *path, int32_t *size);
int      retro_vfs_smb_stat_64(const char *path, int64_t *size, int64_t *mtime);

/* Directories */
retro_vfs_smb_dir_t *retro_vfs_smb_opendir(const char *path, bool include_hidden);
bool        retro_vfs_smb_readdir(retro_vfs_smb_dir_t *d);
const char *retro_vfs_smb_dirent_get_name(retro_vfs_smb_dir_t *d);
bool        retro_vfs_smb_dirent_is_dir(retro_vfs_smb_dir_t *d);
int         retro_vfs_smb_closedir(retro_vfs_smb_dir_t *d);

/* Copy 'smb_path' (and, for .cue/.gdi/.m3u, the files it references)
 * into 'cache_dir', mirroring the server/share/path layout.
 * A file that is already cached with the same size and timestamp is reused.
 * An archive sub-path ("game.zip#rom.bin") is kept: only the archive is
 * copied and the "#rom.bin" suffix is appended to the local path.
 * On success writes the local path to 'out' and returns true. */
bool retro_vfs_smb_cache_fetch(const char *smb_path, const char *cache_dir,
      uint64_t cache_limit_bytes, char *out, size_t out_len,
      char *err, size_t err_len);

/* Cache size limit from smb.cfg ("cache_limit_mb"), default 1536 MB. */
uint64_t retro_vfs_smb_cache_limit(void);

/* Drop all open connections (e.g. on network change). */
void retro_vfs_smb_disconnect_all(void);

RETRO_END_DECLS

#endif
