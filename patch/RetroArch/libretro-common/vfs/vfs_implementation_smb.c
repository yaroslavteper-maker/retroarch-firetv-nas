/* Copyright  (C) 2010-2026 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (vfs_implementation_smb.c).
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

#ifdef HAVE_SMB

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <utime.h>

#include <stdint.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#include <libretro.h>
#include <compat/strl.h>
#include <vfs/vfs_implementation_smb.h>

#ifdef HAVE_THREADS
#include <rthreads/rthreads.h>
#endif

#ifdef ANDROID
#include <android/log.h>
#define SMB_LOG(...) __android_log_print(ANDROID_LOG_INFO, "RetroArch", "[SMB] " __VA_ARGS__)
#else
#define SMB_LOG(...) do { fprintf(stderr, "[SMB] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#endif

#define SMB_PATH_MAX       4096
#define SMB_READAHEAD      (256 * 1024)
#define SMB_COPY_CHUNK     (1024 * 1024)
#define SMB_TIMEOUT_SECS   15

/* ------------------------------------------------------------------ */
/* Locking: libsmb2 contexts are not thread-safe, and RetroArch calls the
 * VFS from the main thread and from task threads. One lock serialises
 * every network operation. */

#ifdef HAVE_THREADS
static slock_t *smb_lock_obj = NULL;
static void smb_lock(void)
{
   /* Created on first use; the VFS is first touched from the main thread
    * during startup, well before any task thread exists. */
   if (!smb_lock_obj)
      smb_lock_obj = slock_new();
   if (smb_lock_obj)
      slock_lock(smb_lock_obj);
}
static void smb_unlock(void)
{
   if (smb_lock_obj)
      slock_unlock(smb_lock_obj);
}
#else
static void smb_lock(void)   { }
static void smb_unlock(void) { }
#endif

/* ------------------------------------------------------------------ */
/* Credentials file */

static char smb_cfg_path[SMB_PATH_MAX] = {0};
static bool smb_cfg_loaded             = false;
static char smb_cfg_user[256]          = {0};
static char smb_cfg_password[256]      = {0};
static char smb_cfg_domain[256]        = {0};
static uint64_t smb_cfg_cache_mb       = 1536;
static char smb_cfg_deps[SMB_PATH_MAX] = {0};

void retro_vfs_smb_set_config_path(const char *path)
{
   smb_lock();
   strlcpy(smb_cfg_path, path ? path : "", sizeof(smb_cfg_path));
   smb_cfg_loaded = false;
   smb_unlock();
}

static void smb_trim(char *s)
{
   size_t n;
   char *p = s;
   while (*p && isspace((unsigned char)*p))
      p++;
   if (p != s)
      memmove(s, p, strlen(p) + 1);
   n = strlen(s);
   while (n && isspace((unsigned char)s[n - 1]))
      s[--n] = '\0';
   /* strip surrounding quotes */
   if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\'')))
   {
      s[n - 1] = '\0';
      memmove(s, s + 1, n - 1);
   }
}

static bool smb_cfg_try_load(const char *path)
{
   char line[1024];
   FILE *fp = fopen(path, "r");
   if (!fp)
      return false;
   while (fgets(line, sizeof(line), fp))
   {
      char *eq;
      smb_trim(line);
      if (!line[0] || line[0] == '#' || line[0] == ';')
         continue;
      if (!(eq = strchr(line, '=')))
         continue;
      *eq = '\0';
      smb_trim(line);
      smb_trim(eq + 1);
      if (!strcasecmp(line, "user") || !strcasecmp(line, "username") || !strcasecmp(line, "smb_user"))
         strlcpy(smb_cfg_user, eq + 1, sizeof(smb_cfg_user));
      else if (!strcasecmp(line, "password") || !strcasecmp(line, "smb_password"))
         strlcpy(smb_cfg_password, eq + 1, sizeof(smb_cfg_password));
      else if (!strcasecmp(line, "domain") || !strcasecmp(line, "workgroup") || !strcasecmp(line, "smb_domain"))
         strlcpy(smb_cfg_domain, eq + 1, sizeof(smb_cfg_domain));
      else if (!strcasecmp(line, "deps_file"))
         strlcpy(smb_cfg_deps, eq + 1, sizeof(smb_cfg_deps));
      else if (!strcasecmp(line, "cache_limit_mb"))
      {
         long long mb = atoll(eq + 1);
         if (mb > 0)
            smb_cfg_cache_mb = (uint64_t)mb;
      }
   }
   fclose(fp);
   SMB_LOG("credentials loaded from %s (user '%s')", path, smb_cfg_user);
   return true;
}

static void smb_cfg_load(void)
{
   static const char *defaults[] = {
      "/storage/emulated/0/RetroArch/smb.cfg",
      "/sdcard/RetroArch/smb.cfg",
      NULL
   };
   int i;
   if (smb_cfg_loaded)
      return;
   smb_cfg_loaded      = true;
   smb_cfg_user[0]     = '\0';
   smb_cfg_password[0] = '\0';
   smb_cfg_domain[0]   = '\0';
   smb_cfg_cache_mb    = 1536;
   smb_cfg_deps[0]     = '\0';
   if (smb_cfg_path[0] && smb_cfg_try_load(smb_cfg_path))
      return;
   for (i = 0; defaults[i]; i++)
      if (smb_cfg_try_load(defaults[i]))
         return;
}

/* ------------------------------------------------------------------ */
/* URL parsing: smb://[user@]server[:port]/share[/path] */

typedef struct
{
   char user[256];
   char server[256];
   char share[256];
   char path[SMB_PATH_MAX]; /* relative to share, '/'-separated, no leading '/' */
} smb_url_t;

bool retro_vfs_is_smb_path(const char *path)
{
   return path
      && (path[0] == 's' || path[0] == 'S')
      && (path[1] == 'm' || path[1] == 'M')
      && (path[2] == 'b' || path[2] == 'B')
      && path[3] == ':' && path[4] == '/' && path[5] == '/';
}

/* Collapse '//' , './' and resolve '..' inside the share path. */
static void smb_normalise(char *p)
{
   char out[SMB_PATH_MAX];
   size_t o = 0;
   const char *s = p;
   out[0] = '\0';
   while (*s)
   {
      const char *e;
      size_t len;
      while (*s == '/' || *s == '\\')
         s++;
      if (!*s)
         break;
      e = s;
      while (*e && *e != '/' && *e != '\\')
         e++;
      len = (size_t)(e - s);
      if (len == 1 && s[0] == '.')
         ;
      else if (len == 2 && s[0] == '.' && s[1] == '.')
      {
         while (o > 0 && out[o - 1] != '/')
            o--;
         if (o > 0)
            o--;
         out[o] = '\0';
      }
      else if (o + len + 2 < sizeof(out))
      {
         if (o)
            out[o++] = '/';
         memcpy(out + o, s, len);
         o += len;
         out[o] = '\0';
      }
      s = e;
   }
   strlcpy(p, out, SMB_PATH_MAX);
}

static bool smb_parse(const char *url, smb_url_t *u)
{
   const char *p, *slash, *at, *share_end;
   size_t n;

   memset(u, 0, sizeof(*u));
   if (!retro_vfs_is_smb_path(url))
      return false;
   p = url + 6;

   slash = strchr(p, '/');
   if (!slash)
      return false; /* need at least server/share */

   /* user@ inside the authority part only */
   at = memchr(p, '@', (size_t)(slash - p));
   if (at)
   {
      n = (size_t)(at - p);
      if (n >= sizeof(u->user))
         return false;
      memcpy(u->user, p, n);
      u->user[n] = '\0';
      p = at + 1;
   }
   n = (size_t)(slash - p);
   if (n == 0 || n >= sizeof(u->server))
      return false;
   memcpy(u->server, p, n);
   u->server[n] = '\0';

   p = slash + 1;
   share_end = strchr(p, '/');
   n = share_end ? (size_t)(share_end - p) : strlen(p);
   if (n == 0 || n >= sizeof(u->share))
      return false;
   memcpy(u->share, p, n);
   u->share[n] = '\0';

   if (share_end)
      strlcpy(u->path, share_end + 1, sizeof(u->path));
   smb_normalise(u->path);
   return true;
}

/* ------------------------------------------------------------------ */
/* Connection pool, one connection per user/server/share */

typedef struct smb_conn
{
   struct smb2_context *ctx;
   struct smb_conn *next;
   uint32_t max_read;
   unsigned gen;          /* bumped on every reconnect */
   char key[800];
   char user[256];
   char server[256];
   char share[256];
} smb_conn_t;

static smb_conn_t *smb_conns = NULL;

static void smb_conn_drop_ctx(smb_conn_t *c)
{
   if (c->ctx)
   {
      smb2_disconnect_share(c->ctx);
      smb2_destroy_context(c->ctx);
      c->ctx = NULL;
   }
}

static bool smb_conn_open_ctx(smb_conn_t *c)
{
   const char *user;
   struct smb2_context *ctx = smb2_init_context();
   if (!ctx)
      return false;

   smb_cfg_load();
   user = c->user[0] ? c->user : (smb_cfg_user[0] ? smb_cfg_user : "guest");

   smb2_set_security_mode(ctx, SMB2_NEGOTIATE_SIGNING_ENABLED);
   smb2_set_timeout(ctx, SMB_TIMEOUT_SECS);
   smb2_set_user(ctx, user);
   smb2_set_password(ctx, smb_cfg_password);
   if (smb_cfg_domain[0])
      smb2_set_domain(ctx, smb_cfg_domain);

   if (smb2_connect_share(ctx, c->server, c->share, user) < 0)
   {
      SMB_LOG("connect to //%s/%s as '%s' failed: %s",
            c->server, c->share, user, smb2_get_error(ctx));
      smb2_destroy_context(ctx);
      return false;
   }
   c->ctx      = ctx;
   c->max_read = smb2_get_max_read_size(ctx);
   if (c->max_read == 0 || c->max_read > 8 * 1024 * 1024)
      c->max_read = 1024 * 1024;
   c->gen++;
   SMB_LOG("connected to //%s/%s as '%s' (max read %u)",
         c->server, c->share, user, (unsigned)c->max_read);
   return true;
}

/* Lock must be held. */
static smb_conn_t *smb_conn_get(const smb_url_t *u)
{
   smb_conn_t *c;
   char key[800];
   snprintf(key, sizeof(key), "%s@%s/%s", u->user, u->server, u->share);
   for (c = smb_conns; c; c = c->next)
      if (!strcasecmp(c->key, key))
         break;
   if (!c)
   {
      if (!(c = (smb_conn_t*)calloc(1, sizeof(*c))))
         return NULL;
      strlcpy(c->key,    key,       sizeof(c->key));
      strlcpy(c->user,   u->user,   sizeof(c->user));
      strlcpy(c->server, u->server, sizeof(c->server));
      strlcpy(c->share,  u->share,  sizeof(c->share));
      c->next   = smb_conns;
      smb_conns = c;
   }
   if (!c->ctx && !smb_conn_open_ctx(c))
      return NULL;
   return c;
}

/* After a failed call: if the session is gone, reconnect. Returns true if
 * a fresh connection is now available (so the caller may retry once).
 * Lock must be held. */
static bool smb_conn_recover(smb_conn_t *c, int err)
{
   /* "No such file" and friends are answers, not connection problems.
    * Anything else (including libsmb2's generic -1) may mean the session
    * died or the stream lost sync: probe it with an ECHO. */
   if (err == -ENOENT || err == -ENOTDIR || err == -EISDIR || err == -EACCES)
      return false;
   if (c->ctx && smb2_echo(c->ctx) == 0)
      return false; /* connection fine; the error was real */
   SMB_LOG("connection to //%s/%s lost (%d), reconnecting", c->server, c->share, err);
   smb_conn_drop_ctx(c);
   return smb_conn_open_ctx(c);
}

uint64_t retro_vfs_smb_cache_limit(void)
{
   uint64_t mb;
   smb_lock();
   smb_cfg_load();
   mb = smb_cfg_cache_mb;
   smb_unlock();
   return mb * 1024 * 1024;
}

void retro_vfs_smb_disconnect_all(void)
{
   smb_conn_t *c;
   smb_lock();
   for (c = smb_conns; c; c = c->next)
      smb_conn_drop_ctx(c);
   smb_unlock();
}

/* ------------------------------------------------------------------ */
/* stat */

static int smb_stat_locked(const smb_url_t *u, struct smb2_stat_64 *st)
{
   int ret;
   smb_conn_t *c = smb_conn_get(u);
   if (!c)
      return -ENOTCONN;
   if (!u->path[0])
   {
      /* the share root itself */
      memset(st, 0, sizeof(*st));
      st->smb2_type = SMB2_TYPE_DIRECTORY;
      return 0;
   }
   ret = smb2_stat(c->ctx, u->path, st);
   if (ret < 0 && smb_conn_recover(c, ret))
      ret = smb2_stat(c->ctx, u->path, st);
   return ret;
}

int retro_vfs_smb_stat_64(const char *path, int64_t *size, int64_t *mtime)
{
   smb_url_t u;
   struct smb2_stat_64 st;
   int ret, flags = 0;

   if (!smb_parse(path, &u))
      return 0;
   smb_lock();
   ret = smb_stat_locked(&u, &st);
   smb_unlock();
   if (ret < 0)
      return 0;

   flags = RETRO_VFS_STAT_IS_VALID;
   if (st.smb2_type == SMB2_TYPE_DIRECTORY)
      flags |= RETRO_VFS_STAT_IS_DIRECTORY;
   if (size)
      *size = (int64_t)st.smb2_size;
   if (mtime)
      *mtime = (int64_t)st.smb2_mtime;
   return flags;
}

int retro_vfs_smb_stat(const char *path, int32_t *size)
{
   int64_t sz = 0;
   int flags  = retro_vfs_smb_stat_64(path, &sz, NULL);
   if (size)
      *size = (sz > 0x7fffffff) ? 0x7fffffff : (int32_t)sz;
   return flags;
}

/* ------------------------------------------------------------------ */
/* Files */

struct retro_vfs_smb_file
{
   smb_conn_t *conn;
   struct smb2fh *fh;
   uint8_t *rbuf;
   uint64_t size;
   uint64_t pos;
   uint64_t rbuf_off;
   uint32_t rbuf_len;
   unsigned gen;
   bool error;
   char rel[SMB_PATH_MAX];
};

/* (Re)open the handle if the connection was replaced. Lock held. */
static bool smb_file_ensure(retro_vfs_smb_file_t *f)
{
   if (!f->conn->ctx && !smb_conn_open_ctx(f->conn))
      return false;
   if (f->fh && f->gen == f->conn->gen)
      return true;
   /* old handle died with the old context */
   f->fh  = smb2_open(f->conn->ctx, f->rel, O_RDONLY);
   f->gen = f->conn->gen;
   return f->fh != NULL;
}

retro_vfs_smb_file_t *retro_vfs_smb_file_open(const char *path, unsigned mode)
{
   smb_url_t u;
   struct smb2_stat_64 st;
   retro_vfs_smb_file_t *f;
   smb_conn_t *c;

   /* Network shares are read-only from RetroArch's point of view */
   if (mode != RETRO_VFS_FILE_ACCESS_READ)
      return NULL;
   if (!smb_parse(path, &u) || !u.path[0])
      return NULL;
   if (!(f = (retro_vfs_smb_file_t*)calloc(1, sizeof(*f))))
      return NULL;
   strlcpy(f->rel, u.path, sizeof(f->rel));

   smb_lock();
   if (!(c = smb_conn_get(&u)))
      goto fail;
   f->conn = c;
   f->fh   = smb2_open(c->ctx, f->rel, O_RDONLY);
   if (!f->fh && smb_conn_recover(c, -ENOTCONN))
      f->fh = smb2_open(c->ctx, f->rel, O_RDONLY);
   if (!f->fh)
      goto fail;
   f->gen = c->gen;
   if (smb2_fstat(c->ctx, f->fh, &st) < 0 || st.smb2_type == SMB2_TYPE_DIRECTORY)
   {
      smb2_close(c->ctx, f->fh);
      goto fail;
   }
   f->size = st.smb2_size;
   smb_unlock();
   return f;

fail:
   smb_unlock();
   free(f);
   return NULL;
}

int retro_vfs_smb_file_close(retro_vfs_smb_file_t *f)
{
   if (!f)
      return -1;
   smb_lock();
   if (f->fh && f->conn && f->conn->ctx && f->gen == f->conn->gen)
      smb2_close(f->conn->ctx, f->fh);
   smb_unlock();
   free(f->rbuf);
   free(f);
   return 0;
}

int64_t retro_vfs_smb_file_size(retro_vfs_smb_file_t *f)  { return f ? (int64_t)f->size : -1; }
int64_t retro_vfs_smb_file_tell(retro_vfs_smb_file_t *f)  { return f ? (int64_t)f->pos  : -1; }
int     retro_vfs_smb_file_error(retro_vfs_smb_file_t *f) { return f ? f->error : 1; }

int64_t retro_vfs_smb_file_seek(retro_vfs_smb_file_t *f, int64_t offset, int whence)
{
   int64_t base;
   if (!f)
      return -1;
   switch (whence)
   {
      case RETRO_VFS_SEEK_POSITION_START:   base = 0;                 break;
      case RETRO_VFS_SEEK_POSITION_CURRENT: base = (int64_t)f->pos;   break;
      case RETRO_VFS_SEEK_POSITION_END:     base = (int64_t)f->size;  break;
      default: return -1;
   }
   if (base + offset < 0)
      return -1;
   f->pos = (uint64_t)(base + offset);
   return (int64_t)f->pos;
}

/* One pread of at most conn->max_read bytes, with one reconnect retry. */
static int smb_pread_locked(retro_vfs_smb_file_t *f, uint8_t *dst, uint32_t len, uint64_t off)
{
   int ret;
   if (!smb_file_ensure(f))
      return -ENOTCONN;
   if (len > f->conn->max_read)
      len = f->conn->max_read;
   ret = smb2_pread(f->conn->ctx, f->fh, dst, len, off);
   if (ret < 0 && smb_conn_recover(f->conn, ret) && smb_file_ensure(f))
      ret = smb2_pread(f->conn->ctx, f->fh, dst, len, off);
   return ret;
}

int64_t retro_vfs_smb_file_read(retro_vfs_smb_file_t *f, void *s, uint64_t len)
{
   uint8_t *out = (uint8_t*)s;
   uint64_t done = 0;

   if (!f || !s)
      return -1;
   if (f->pos >= f->size)
      return 0;
   if (len > f->size - f->pos)
      len = f->size - f->pos;

   smb_lock();
   while (done < len)
   {
      uint64_t want = len - done;
      int ret;

      /* serve from the read-ahead buffer */
      if (f->rbuf && f->pos >= f->rbuf_off && f->pos < f->rbuf_off + f->rbuf_len)
      {
         uint64_t avail = f->rbuf_off + f->rbuf_len - f->pos;
         uint64_t n     = want < avail ? want : avail;
         memcpy(out + done, f->rbuf + (f->pos - f->rbuf_off), (size_t)n);
         done   += n;
         f->pos += n;
         continue;
      }

      if (want >= SMB_READAHEAD)
      {
         /* big read: straight into the caller's buffer */
         uint32_t chunk = want > 0x7fffffffu ? 0x7fffffffu : (uint32_t)want;
         if ((ret = smb_pread_locked(f, out + done, chunk, f->pos)) <= 0)
            goto err;
         done   += (uint64_t)ret;
         f->pos += (uint64_t)ret;
      }
      else
      {
         /* small read: refill the read-ahead buffer */
         if (!f->rbuf && !(f->rbuf = (uint8_t*)malloc(SMB_READAHEAD)))
            goto err;
         if ((ret = smb_pread_locked(f, f->rbuf, SMB_READAHEAD, f->pos)) <= 0)
            goto err;
         f->rbuf_off = f->pos;
         f->rbuf_len = (uint32_t)ret;
      }
   }
   smb_unlock();
   return (int64_t)done;

err:
   smb_unlock();
   f->error = true;
   return done ? (int64_t)done : -1;
}

/* ------------------------------------------------------------------ */
/* Directories: the listing is copied out so no libsmb2 state is kept */

typedef struct
{
   char *name;
   bool is_dir;
} smb_dirent_t;

struct retro_vfs_smb_dir
{
   smb_dirent_t *ents;
   size_t count;
   size_t cap;
   long cur;
};

static bool smb_dir_add(retro_vfs_smb_dir_t *d, const char *name, bool is_dir)
{
   if (d->count == d->cap)
   {
      size_t ncap = d->cap ? d->cap * 2 : 64;
      smb_dirent_t *n = (smb_dirent_t*)realloc(d->ents, ncap * sizeof(*n));
      if (!n)
         return false;
      d->ents = n;
      d->cap  = ncap;
   }
   if (!(d->ents[d->count].name = strdup(name)))
      return false;
   d->ents[d->count].is_dir = is_dir;
   d->count++;
   return true;
}

retro_vfs_smb_dir_t *retro_vfs_smb_opendir(const char *path, bool include_hidden)
{
   smb_url_t u;
   smb_conn_t *c;
   struct smb2dir *sd = NULL;
   struct smb2dirent *ent;
   retro_vfs_smb_dir_t *d;

   (void)include_hidden; /* dir_list filters dot-files itself */
   if (!smb_parse(path, &u))
      return NULL;
   if (!(d = (retro_vfs_smb_dir_t*)calloc(1, sizeof(*d))))
      return NULL;
   d->cur = -1;

   smb_lock();
   if (!(c = smb_conn_get(&u)))
      goto fail;
   sd = smb2_opendir(c->ctx, u.path);
   if (!sd && smb_conn_recover(c, -ENOTCONN))
      sd = smb2_opendir(c->ctx, u.path);
   if (!sd)
   {
      SMB_LOG("opendir '%s' failed: %s", path, c->ctx ? smb2_get_error(c->ctx) : "no connection");
      goto fail;
   }
   while ((ent = smb2_readdir(c->ctx, sd)))
   {
      if (!ent->name || !strcmp(ent->name, ".") || !strcmp(ent->name, ".."))
         continue;
      if (!smb_dir_add(d, ent->name, ent->st.smb2_type == SMB2_TYPE_DIRECTORY))
         break;
   }
   smb2_closedir(c->ctx, sd);
   smb_unlock();
   return d;

fail:
   smb_unlock();
   free(d);
   return NULL;
}

bool retro_vfs_smb_readdir(retro_vfs_smb_dir_t *d)
{
   if (!d || (size_t)(d->cur + 1) >= d->count)
      return false;
   d->cur++;
   return true;
}

const char *retro_vfs_smb_dirent_get_name(retro_vfs_smb_dir_t *d)
{
   if (!d || d->cur < 0 || (size_t)d->cur >= d->count)
      return NULL;
   return d->ents[d->cur].name;
}

bool retro_vfs_smb_dirent_is_dir(retro_vfs_smb_dir_t *d)
{
   if (!d || d->cur < 0 || (size_t)d->cur >= d->count)
      return false;
   return d->ents[d->cur].is_dir;
}

int retro_vfs_smb_closedir(retro_vfs_smb_dir_t *d)
{
   size_t i;
   if (!d)
      return -1;
   for (i = 0; i < d->count; i++)
      free(d->ents[i].name);
   free(d->ents);
   free(d);
   return 0;
}

/* ------------------------------------------------------------------ */
/* Local cache */

static void smb_seterr(char *err, size_t len, const char *fmt, const char *a)
{
   if (err && len)
      snprintf(err, len, fmt, a ? a : "");
}

static bool smb_mkdirs(const char *dir)
{
   char tmp[SMB_PATH_MAX];
   char *p;
   strlcpy(tmp, dir, sizeof(tmp));
   for (p = tmp + 1; *p; p++)
   {
      if (*p == '/')
      {
         *p = '\0';
         if (mkdir(tmp, 0775) != 0 && errno != EEXIST)
            return false;
         *p = '/';
      }
   }
   return mkdir(tmp, 0775) == 0 || errno == EEXIST;
}

/* Sum of the cache, and the oldest-cached file (by ctime). */
static void smb_cache_scan(const char *dir, uint64_t *total,
      char *oldest, size_t oldest_len, time_t *oldest_t, const char *keep)
{
   struct dirent *e;
   DIR *d = opendir(dir);
   if (!d)
      return;
   while ((e = readdir(d)))
   {
      char p[SMB_PATH_MAX];
      struct stat st;
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
         continue;
      snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
      if (lstat(p, &st) != 0)
         continue;
      if (S_ISDIR(st.st_mode))
         smb_cache_scan(p, total, oldest, oldest_len, oldest_t, keep);
      else if (S_ISREG(st.st_mode))
      {
         *total += (uint64_t)st.st_size;
         if ((!keep || strcmp(p, keep)) && (!*oldest || st.st_ctime < *oldest_t))
         {
            strlcpy(oldest, p, oldest_len);
            *oldest_t = st.st_ctime;
         }
      }
   }
   closedir(d);
}

static void smb_cache_make_room(const char *cache_dir, uint64_t need,
      uint64_t limit, const char *keep)
{
   int guard;
   if (!limit)
      return;
   for (guard = 0; guard < 10000; guard++)
   {
      uint64_t total = 0;
      char oldest[SMB_PATH_MAX];
      time_t oldest_t = 0;
      oldest[0] = '\0';
      smb_cache_scan(cache_dir, &total, oldest, sizeof(oldest), &oldest_t, keep);
      if (total + need <= limit || !oldest[0])
         return;
      SMB_LOG("cache full (%llu MB), evicting %s",
            (unsigned long long)(total >> 20), oldest);
      if (unlink(oldest) != 0)
         return;
   }
}

/* smb://user@server/share/a/b.bin -> <cache>/server/share/a/b.bin */
static bool smb_local_path(const char *cache_dir, const smb_url_t *u,
      char *out, size_t len)
{
   char server[256];
   char *p;
   strlcpy(server, u->server, sizeof(server));
   for (p = server; *p; p++)
      if (*p == ':')
         *p = '_';
   return (size_t)snprintf(out, len, "%s/%s/%s/%s",
         cache_dir, server, u->share, u->path) < len;
}

/* Copy one remote file to 'local' unless it is already cached. */
static bool smb_fetch_one(const char *remote, const char *local,
      const char *cache_dir, uint64_t limit, char *err, size_t err_len)
{
   int64_t rsize = 0, rmtime = 0;
   struct stat lst;
   char part[SMB_PATH_MAX + 8];
   char parent[SMB_PATH_MAX];
   char *slash;
   retro_vfs_smb_file_t *f;
   FILE *out;
   uint8_t *buf;
   uint64_t copied = 0;
   int flags = retro_vfs_smb_stat_64(remote, &rsize, &rmtime);

   if (!(flags & RETRO_VFS_STAT_IS_VALID) || (flags & RETRO_VFS_STAT_IS_DIRECTORY))
   {
      smb_seterr(err, err_len, "Not found on network share: %s", remote);
      return false;
   }

   if (stat(local, &lst) == 0 && (uint64_t)lst.st_size == (uint64_t)rsize
         && (int64_t)lst.st_mtime == rmtime)
   {
      SMB_LOG("cache hit: %s", local);
      return true;
   }

   strlcpy(parent, local, sizeof(parent));
   if ((slash = strrchr(parent, '/')))
      *slash = '\0';
   if (!smb_mkdirs(parent))
   {
      smb_seterr(err, err_len, "Cannot create cache folder: %s", parent);
      return false;
   }

   smb_cache_make_room(cache_dir, (uint64_t)rsize, limit, local);

   if (!(f = retro_vfs_smb_file_open(remote, RETRO_VFS_FILE_ACCESS_READ)))
   {
      smb_seterr(err, err_len, "Cannot open on network share: %s", remote);
      return false;
   }
   snprintf(part, sizeof(part), "%s.part", local);
   if (!(out = fopen(part, "wb")) || !(buf = (uint8_t*)malloc(SMB_COPY_CHUNK)))
   {
      if (out)
      {
         fclose(out);
         unlink(part);
      }
      retro_vfs_smb_file_close(f);
      smb_seterr(err, err_len, "Cannot write to cache: %s", part);
      return false;
   }

   SMB_LOG("copying %s (%lld bytes) -> %s", remote, (long long)rsize, local);
   for (;;)
   {
      int64_t n = retro_vfs_smb_file_read(f, buf, SMB_COPY_CHUNK);
      if (n < 0)
         break;
      if (n == 0)
         break;
      if (fwrite(buf, 1, (size_t)n, out) != (size_t)n)
      {
         n = -1;
         break;
      }
      copied += (uint64_t)n;
   }
   free(buf);
   retro_vfs_smb_file_close(f);
   if (fclose(out) != 0 || copied != (uint64_t)rsize)
   {
      unlink(part);
      smb_seterr(err, err_len, "Copy from network share failed: %s", remote);
      return false;
   }
   if (rename(part, local) != 0)
   {
      unlink(part);
      smb_seterr(err, err_len, "Cannot finish cache file: %s", local);
      return false;
   }
   {
      struct utimbuf t;
      t.actime  = (time_t)rmtime;
      t.modtime = (time_t)rmtime;
      utime(local, &t);
   }
   return true;
}

static const char *smb_ext(const char *path)
{
   const char *dot   = strrchr(path, '.');
   const char *slash = strrchr(path, '/');
   if (!dot || (slash && dot < slash))
      return "";
   return dot + 1;
}

/* Pull the file name out of one line of a .cue ("FILE ...") or .gdi */
static bool smb_companion_from_line(const char *ext, const char *line,
      char *name, size_t len)
{
   const char *p = line;
   while (*p && isspace((unsigned char)*p))
      p++;

   if (!strcasecmp(ext, "cue"))
   {
      const char *s, *e;
      if (strncasecmp(p, "FILE", 4) != 0 || !isspace((unsigned char)p[4]))
         return false;
      p += 4;
      while (*p && isspace((unsigned char)*p))
         p++;
      if (*p == '"')
      {
         s = ++p;
         e = strchr(s, '"');
         if (!e)
            return false;
      }
      else
      {
         /* unquoted: name ends at the last space before the type word */
         s = p;
         e = p + strlen(p);
         while (e > s && isspace((unsigned char)e[-1]))
            e--;
         while (e > s && !isspace((unsigned char)e[-1]))
            e--;
         while (e > s && isspace((unsigned char)e[-1]))
            e--;
         if (e == s)
            e = s + strcspn(s, " \t\r\n");
      }
      if ((size_t)(e - s) == 0 || (size_t)(e - s) >= len)
         return false;
      memcpy(name, s, (size_t)(e - s));
      name[e - s] = '\0';
      return true;
   }

   if (!strcasecmp(ext, "gdi"))
   {
      /* "<track> <lba> <type> <sectsize> <file> <offset>"; file may be quoted */
      int field;
      for (field = 0; field < 4; field++)
      {
         if (!*p)
            return false;
         while (*p && !isspace((unsigned char)*p))
            p++;
         while (*p && isspace((unsigned char)*p))
            p++;
      }
      if (!*p)
         return false;
      if (*p == '"')
      {
         const char *e = strchr(++p, '"');
         if (!e || (size_t)(e - p) >= len || e == p)
            return false;
         memcpy(name, p, (size_t)(e - p));
         name[e - p] = '\0';
      }
      else
      {
         size_t n = strcspn(p, " \t\r\n");
         if (!n || n >= len)
            return false;
         memcpy(name, p, n);
         name[n] = '\0';
      }
      return true;
   }

   if (!strcasecmp(ext, "m3u"))
   {
      size_t n;
      if (*p == '#' || !*p)
         return false;
      strlcpy(name, p, len);
      n = strlen(name);
      while (n && (name[n - 1] == '\r' || name[n - 1] == '\n' || isspace((unsigned char)name[n - 1])))
         name[--n] = '\0';
      return n > 0;
   }
   return false;
}

static bool smb_fetch_with_companions(const char *remote, const char *local,
      const char *cache_dir, uint64_t limit, int depth, char *err, size_t err_len)
{
   const char *ext;
   char line[2048];
   FILE *fp;
   bool ok = true;

   if (!smb_fetch_one(remote, local, cache_dir, limit, err, err_len))
      return false;

   ext = smb_ext(remote);
   if (depth > 2)
      return true;

   if (!strcasecmp(ext, "ccd"))
   {
      /* CloneCD: same basename .img and .sub (sub is optional) */
      static const char *sides[] = { "img", "sub", NULL };
      int i;
      for (i = 0; sides[i]; i++)
      {
         char r2[SMB_PATH_MAX], l2[SMB_PATH_MAX];
         size_t rb = (size_t)(smb_ext(remote) - remote), lb = (size_t)(smb_ext(local) - local);
         if (rb + 4 >= sizeof(r2) || lb + 4 >= sizeof(l2))
            continue;
         memcpy(r2, remote, rb); strlcpy(r2 + rb, sides[i], sizeof(r2) - rb);
         memcpy(l2, local,  lb); strlcpy(l2 + lb, sides[i], sizeof(l2) - lb);
         if (!smb_fetch_one(r2, l2, cache_dir, limit, err, err_len) && i == 0)
            return false;
      }
      return true;
   }

   if (strcasecmp(ext, "cue") && strcasecmp(ext, "gdi") && strcasecmp(ext, "m3u"))
      return true;

   if (!(fp = fopen(local, "r")))
      return true;
   while (ok && fgets(line, sizeof(line), fp))
   {
      char name[1024], r2[SMB_PATH_MAX], l2[SMB_PATH_MAX];
      const char *rs = strrchr(remote, '/'), *ls = strrchr(local, '/');
      if (!smb_companion_from_line(ext, line, name, sizeof(name)))
         continue;
      if (name[0] == '/' || strstr(name, "://"))
         continue; /* absolute path: not something we can mirror */
      if (!rs || !ls)
         continue;
      snprintf(r2, sizeof(r2), "%.*s/%s", (int)(rs - remote), remote, name);
      snprintf(l2, sizeof(l2), "%.*s/%s", (int)(ls - local),  local,  name);
      {
         /* tidy "a/../b" in the share part of the remote path */
         smb_url_t u2;
         if (smb_parse(r2, &u2))
         {
            char rebuilt[SMB_PATH_MAX];
            snprintf(rebuilt, sizeof(rebuilt), "smb://%s%s%s/%s/%s",
                  u2.user, u2.user[0] ? "@" : "", u2.server, u2.share, u2.path);
            strlcpy(r2, rebuilt, sizeof(r2));
            smb_local_path(cache_dir, &u2, l2, sizeof(l2));
         }
      }
      ok = smb_fetch_with_companions(r2, l2, cache_dir, limit, depth + 1, err, err_len);
   }
   fclose(fp);
   return ok;
}

/* Arcade sets often need other sets from the same folder (parent ROMs,
 * BIOS such as neogeo.zip). They are listed in a text file, one line per set:
 *     mslug.zip: neogeo.zip
 *     pacmanf.zip: pacman.zip namco.zip
 * Default location: /storage/emulated/0/RetroArch/smb_deps.txt
 * (override with "deps_file = ..." in smb.cfg). */
static bool smb_deps_lookup(const char *file_name, char *deps, size_t len)
{
   char line[2048];
   char path[SMB_PATH_MAX];
   FILE *fp;
   bool found = false;

   smb_lock();
   smb_cfg_load();
   strlcpy(path, smb_cfg_deps[0] ? smb_cfg_deps
         : "/storage/emulated/0/RetroArch/smb_deps.txt", sizeof(path));
   smb_unlock();

   if (!(fp = fopen(path, "r")))
      return false;
   while (!found && fgets(line, sizeof(line), fp))
   {
      char *colon = strchr(line, ':');
      if (!colon || line[0] == '#')
         continue;
      *colon = '\0';
      smb_trim(line);
      if (!strcasecmp(line, file_name))
      {
         strlcpy(deps, colon + 1, len);
         smb_trim(deps);
         found = true;
      }
   }
   fclose(fp);
   return found;
}

static void smb_fetch_deps(const char *remote, const char *local,
      const char *cache_dir, uint64_t limit)
{
   char deps[2048];
   char *tok, *save = NULL;
   const char *rs = strrchr(remote, '/'), *ls = strrchr(local, '/');
   if (!rs || !ls || !smb_deps_lookup(rs + 1, deps, sizeof(deps)))
      return;
   for (tok = strtok_r(deps, " \t,", &save); tok; tok = strtok_r(NULL, " \t,", &save))
   {
      char r2[SMB_PATH_MAX], l2[SMB_PATH_MAX], err[512];
      snprintf(r2, sizeof(r2), "%.*s/%s", (int)(rs - remote), remote, tok);
      snprintf(l2, sizeof(l2), "%.*s/%s", (int)(ls - local),  local,  tok);
      if (!smb_fetch_one(r2, l2, cache_dir, limit, err, sizeof(err)))
         SMB_LOG("dependency %s not fetched: %s", tok, err);
   }
}

bool retro_vfs_smb_cache_fetch(const char *smb_path, const char *cache_dir,
      uint64_t cache_limit_bytes, char *out, size_t out_len,
      char *err, size_t err_len)
{
   char remote[SMB_PATH_MAX];
   char local[SMB_PATH_MAX];
   const char *suffix = NULL;
   const char *hash;
   smb_url_t u;

   if (err && err_len)
      err[0] = '\0';
   if (!retro_vfs_is_smb_path(smb_path) || !cache_dir || !*cache_dir)
   {
      smb_seterr(err, err_len, "Bad network path: %s", smb_path);
      return false;
   }

   /* keep "#member" of archive paths out of the copy */
   strlcpy(remote, smb_path, sizeof(remote));
   if ((hash = strchr(remote, '#')))
   {
      const char *dot = hash;
      while (dot > remote && *dot != '.' && *dot != '/')
         dot--;
      if (*dot == '.' && (!strncasecmp(dot, ".zip#", 5) || !strncasecmp(dot, ".7z#", 4)
               || !strncasecmp(dot, ".apk#", 5)))
      {
         suffix = smb_path + (hash - remote);
         remote[hash - remote] = '\0';
      }
   }

   if (!smb_parse(remote, &u) || !u.path[0])
   {
      smb_seterr(err, err_len, "Bad network path: %s", smb_path);
      return false;
   }
   if (!smb_local_path(cache_dir, &u, local, sizeof(local)))
   {
      smb_seterr(err, err_len, "Path too long: %s", smb_path);
      return false;
   }

   if (!smb_fetch_with_companions(remote, local, cache_dir, cache_limit_bytes, 0, err, err_len))
      return false;
   smb_fetch_deps(remote, local, cache_dir, cache_limit_bytes);

   if ((size_t)snprintf(out, out_len, "%s%s", local, suffix ? suffix : "") >= out_len)
   {
      smb_seterr(err, err_len, "Path too long: %s", smb_path);
      return false;
   }
   return true;
}

#endif /* HAVE_SMB */
