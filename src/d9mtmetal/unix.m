/* d9mtmetal.so: unix side. Plain Metal calls, no wine APIs needed.
 * Exports __wine_unix_call_funcs / __wine_unix_call_wow64_funcs which the
 * wine loader resolves when the matching builtin PE loads.
 * Param structs are identical for 32/64-bit callers (all-u64 pointers),
 * so both tables point at the same implementations.
 */
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <sqlite3.h>

#include "d9mtmetal.h"

extern char **environ;

typedef int NTSTATUS; /* wine unixlib_entry_t contract */
#define STATUS_SUCCESS 0

static NTSTATUS d9mt_new_library_from_source(void *args) {
  struct d9mt_newlibrary_params *p = args;
  p->ret_library = 0;
  p->ret_error = 0;

  id<MTLDevice> device = (id<MTLDevice>)(uintptr_t)p->device;
  NSString *src =
      [[NSString alloc] initWithBytes:(const void *)(uintptr_t)p->source_ptr
                               length:(NSUInteger)p->source_len
                             encoding:NSUTF8StringEncoding];
  if (!src)
    return STATUS_SUCCESS;

  MTLCompileOptions *opts = [[MTLCompileOptions alloc] init];
  opts.languageVersion = MTLLanguageVersion3_0;
  // FAST math, unconditionally — the only mode that keeps D3D9 shader semantics
  // correct here. D3D9 assumes fast-math float behavior; .relaxed/.safe change
  // reassociation/rounding enough to corrupt shader-computed positions (collapsed
  // geometry) and cost framerate. Any precision artifact from .fast is a per-shader
  // translation bug (e.g. unguarded normalize()/rsqrt producing unclamped NaN),
  // fixed in the SPIR-V->MSL path — never by globally slowing every game's math.
  opts.mathMode = MTLMathModeFast;

  NSError *err = nil;
  id<MTLLibrary> lib = [device newLibraryWithSource:src
                                            options:opts
                                              error:&err];
  [opts release];
  [src release];

  p->ret_library = (uint64_t)(uintptr_t)lib; /* newLibrary* returns +1 */
  p->ret_error = (uint64_t)(uintptr_t)[err retain];
  return STATUS_SUCCESS;
}

static NTSTATUS d9mt_new_render_pso(void *args) {
  struct d9mt_newpso_params *p = args;
  p->ret_pso = 0;
  p->ret_error = 0;

  id<MTLDevice> device = (id<MTLDevice>)(uintptr_t)p->device;
  const struct d9mt_pso_info *info =
      (const struct d9mt_pso_info *)(uintptr_t)p->info_ptr;

  // Metal validation reports descriptor errors as NSExceptions, and
  // [MTLVertexDescriptor vertexDescriptor] is autoreleased: pool + catch
  // or a bad descriptor kills the whole process
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
  @try {

  // NOTE: vertex/fragment functions MUST be fully specialized (all their
  // function constants supplied at newFunction time). Building a PSO from
  // an unspecialized function does not return an NSError - it crashes the
  // process inside newRenderPipelineStateWithDescriptor.
  MTLRenderPipelineDescriptor *desc =
      [[MTLRenderPipelineDescriptor alloc] init];
  desc.vertexFunction = (id<MTLFunction>)(uintptr_t)info->vertex_function;
  desc.fragmentFunction = (id<MTLFunction>)(uintptr_t)info->fragment_function;

  for (unsigned i = 0; i < 8; i++) {
    const struct d9mt_color_attachment *c = &info->colors[i];
    MTLRenderPipelineColorAttachmentDescriptor *att = desc.colorAttachments[i];
    att.pixelFormat = (MTLPixelFormat)c->pixel_format;
    att.blendingEnabled = c->blending_enabled != 0;
    att.rgbBlendOperation = (MTLBlendOperation)c->rgb_blend_op;
    att.alphaBlendOperation = (MTLBlendOperation)c->alpha_blend_op;
    att.sourceRGBBlendFactor = (MTLBlendFactor)c->src_rgb_blend_factor;
    att.destinationRGBBlendFactor = (MTLBlendFactor)c->dst_rgb_blend_factor;
    att.sourceAlphaBlendFactor = (MTLBlendFactor)c->src_alpha_blend_factor;
    att.destinationAlphaBlendFactor =
        (MTLBlendFactor)c->dst_alpha_blend_factor;
    att.writeMask = (MTLColorWriteMask)c->write_mask;
  }

  desc.depthAttachmentPixelFormat = (MTLPixelFormat)info->depth_pixel_format;
  desc.stencilAttachmentPixelFormat =
      (MTLPixelFormat)info->stencil_pixel_format;
  desc.rasterSampleCount =
      info->raster_sample_count ? info->raster_sample_count : 1;
  desc.alphaToCoverageEnabled = info->alpha_to_coverage != 0;

  if (info->num_attributes) {
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    for (uint32_t i = 0; i < info->num_attributes && i < 18; i++) {
      const struct d9mt_vertex_attribute *a = &info->attributes[i];
      MTLVertexAttributeDescriptor *ad = vd.attributes[a->location];
      ad.format = (MTLVertexFormat)a->format;
      ad.offset = a->offset;
      ad.bufferIndex = a->buffer_index;
    }
    for (uint32_t i = 0; i < info->num_layouts && i < 16; i++) {
      const struct d9mt_vertex_layout *l = &info->layouts[i];
      MTLVertexBufferLayoutDescriptor *ld = vd.layouts[l->buffer_index];
      ld.stride = l->stride;
      ld.stepFunction = (MTLVertexStepFunction)l->step_function;
      /* Constant step requires stepRate 0; everything else needs >= 1 */
      if (ld.stepFunction == MTLVertexStepFunctionConstant)
        ld.stepRate = 0;
      else
        ld.stepRate = l->step_rate ? l->step_rate : 1;
    }
    desc.vertexDescriptor = vd;
  }

  NSError *err = nil;
  id<MTLRenderPipelineState> pso =
      [device newRenderPipelineStateWithDescriptor:desc error:&err];
  [desc release];

  p->ret_pso = (uint64_t)(uintptr_t)pso;
  p->ret_error = (uint64_t)(uintptr_t)[err retain];

  } @catch (NSException *ex) {
    NSString *msg = [NSString
        stringWithFormat:@"d9mtmetal PSO exception: %@: %@", ex.name,
                         ex.reason];
    NSError *exErr = [[NSError alloc]
        initWithDomain:@"d9mtmetal"
                  code:1
              userInfo:@{NSLocalizedDescriptionKey : msg}];
    p->ret_pso = 0;
    p->ret_error = (uint64_t)(uintptr_t)exErr;
  }
  [pool release];
  return STATUS_SUCCESS;
}

/* ==========================================================================
 * Compiled-shader metallib disk cache (Layer C, native-side).
 *
 * On a cache hit we load the stored .metallib with newLibraryWithData (no
 * source compiler -> no Metal compile lock -> no stutter). On a miss a
 * compile backend produces the .metallib bytes, which we store and then load
 * the same way. The live newLibraryWithSource is the guaranteed floor: any
 * miss / corruption / toolchain-absence / load-rejection silently degrades to
 * it, so the cache can never break a draw.
 *
 * The .metallib bytes never cross the PE<->unixlib ABI: only the content key
 * goes down, only an MTLLibrary handle comes back up. The cache lives here
 * because its value type is a .metallib and its consumer is
 * newLibraryWithData, both native.
 *
 * sqlite3 is linked directly rather than reusing winemetal's CacheReader/
 * CacheWriter: those are private ObjC classes inside the prebuilt
 * winemetal.so reachable only through winemetal's own unixcall thunk table,
 * with no native C ABI another unixlib can invoke. Linking -lsqlite3 here
 * keeps the implementation self-contained and the bytes native, honoring the
 * architecture's central boundary rule.
 * ========================================================================== */

/* Bump when our native compile/store path changes the bytes for the same
 * input (this is the native-side belt-and-suspenders companion to the PE-side
 * codegen_epoch, which is already folded into the key). */
#define D9MT_CACHE_SCHEMA_VERSION 1

/* MTLB container magic, little-endian, at offset 0 of every .metallib. */
static const uint8_t k_mtlb_magic[4] = { 'M', 'T', 'L', 'B' };

static bool d9mt_cache_enabled(void) {
  static int s_enabled = -1;
  if (s_enabled < 0) {
    const char *v = getenv("D9MT_SHADER_CACHE");
    s_enabled = (v && v[0] == '0' && v[1] == '\0') ? 0 : 1;
  }
  return s_enabled != 0;
}

/* Per-exe cache db path under the OS Caches dir (auto-purgeable derived
 * data), e.g. <_CS_DARWIN_USER_CACHE_DIR>/d9mt/<exe>/shaders.db. Override
 * with D9MT_SHADER_CACHE_PATH (absolute file path). */
static NSString *d9mt_cache_db_path(void) {
  const char *override = getenv("D9MT_SHADER_CACHE_PATH");
  if (override && override[0])
    return [NSString stringWithUTF8String:override];

  char base[PATH_MAX] = { 0 };
  size_t len = confstr(_CS_DARWIN_USER_CACHE_DIR, base, sizeof(base));
  NSString *root = (len && len <= sizeof(base))
    ? [NSString stringWithUTF8String:base]
    : NSTemporaryDirectory();

  /* exe basename for a per-title subdir; getprogname under wine reports the
   * unix loader, so derive from argv0/program name and strip the path. */
  const char *prog = getprogname();
  NSString *exe = prog && prog[0] ? [NSString stringWithUTF8String:prog]
                                  : @"unknown";
  exe = [exe lastPathComponent];

  NSString *dir = [[root stringByAppendingPathComponent:@"d9mt"]
                    stringByAppendingPathComponent:exe];
  return [dir stringByAppendingPathComponent:@"shaders.db"];
}

/* A fingerprint of the Metal toolchain / OS build, folded into the cache key
 * native-side so an OS Metal update proactively invalidates the cache (AIR
 * ABI can shift across Metal versions). Combines the CoreFoundation version
 * with the Metal framework bundle version. */
static uint64_t d9mt_toolchain_id(void) {
  static uint64_t s_id = 0;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    uint64_t id = (uint64_t)(uint32_t)kCFCoreFoundationVersionNumber;
    NSBundle *metal = [NSBundle bundleWithIdentifier:@"com.apple.Metal"];
    NSString *ver = metal.infoDictionary[@"CFBundleVersion"];
    if (ver)
      id = (id << 1) ^ (uint64_t)[ver hash];
    id ^= ((uint64_t)D9MT_CACHE_SCHEMA_VERSION) << 56;
    s_id = id;
  });
  return s_id;
}

/* Process-global sqlite handle (NOMUTEX) guarded by our own mutex; the heavy
 * compile always runs outside this lock, so contention is negligible. */
static sqlite3 *s_cache_db = NULL;
static sqlite3_stmt *s_cache_get = NULL;
static sqlite3_stmt *s_cache_set = NULL;
static pthread_mutex_t s_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool s_cache_open_failed = false;

/* Opens (creating if needed) the cache db once. Returns true if usable.
 * Caller must hold s_cache_mutex. */
static bool d9mt_cache_ensure_open_locked(void) {
  if (s_cache_db)
    return true;
  if (s_cache_open_failed)
    return false;

  NSString *dbPath = d9mt_cache_db_path();
  NSString *dir = [dbPath stringByDeletingLastPathComponent];
  [[NSFileManager defaultManager] createDirectoryAtPath:dir
                            withIntermediateDirectories:YES
                                             attributes:nil
                                                  error:nil];

  /* flock sidecar guards only the open/CREATE TABLE cross-process race. */
  NSString *lockPath = [dbPath stringByAppendingString:@"-lock"];
  int lockfd = open([lockPath fileSystemRepresentation], O_RDWR | O_CREAT, 0666);
  if (lockfd >= 0)
    flock(lockfd, LOCK_EX);

  sqlite3 *db = NULL;
  int rc = sqlite3_open_v2([dbPath fileSystemRepresentation], &db,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, NULL);
  if (rc != SQLITE_OK) {
    /* Possibly corrupt: rename aside and recreate empty. */
    if (db) sqlite3_close(db);
    db = NULL;
    NSString *aside = [dbPath stringByAppendingString:@".corrupt"];
    [[NSFileManager defaultManager] removeItemAtPath:aside error:nil];
    [[NSFileManager defaultManager] moveItemAtPath:dbPath toPath:aside error:nil];
    rc = sqlite3_open_v2([dbPath fileSystemRepresentation], &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, NULL);
  }

  if (rc == SQLITE_OK) {
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    /* One physical table; the version axes live inside the key. */
    rc = sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS shaders ("
        "key BLOB PRIMARY KEY, value BLOB NOT NULL) WITHOUT ROWID;",
        NULL, NULL, NULL);
  }

  if (lockfd >= 0) { flock(lockfd, LOCK_UN); close(lockfd); }

  if (rc != SQLITE_OK) {
    if (db) sqlite3_close(db);
    s_cache_open_failed = true;
    NSLog(@"[d9mtmetal] shader cache disabled (open failed: %d)", rc);
    return false;
  }

  sqlite3_prepare_v2(db, "SELECT value FROM shaders WHERE key = ?;", -1,
                     &s_cache_get, NULL);
  sqlite3_prepare_v2(db,
      "INSERT OR REPLACE INTO shaders (key, value) VALUES (?, ?);", -1,
      &s_cache_set, NULL);
  s_cache_db = db;
  NSLog(@"[d9mtmetal] shader cache open: %@", dbPath);
  return true;
}

/* Builds the full cache key blob: the PE-supplied content key (already a
 * digest over MSL bytes + codegen_epoch + lang version + fast_math) prefixed
 * with the native toolchain_id so an OS Metal update invalidates. */
static NSData *d9mt_cache_key(const void *key_ptr, uint64_t key_len) {
  uint64_t tid = d9mt_toolchain_id();
  NSMutableData *k = [NSMutableData dataWithCapacity:(NSUInteger)(8 + key_len)];
  [k appendBytes:&tid length:sizeof(tid)];
  if (key_len)
    [k appendBytes:key_ptr length:(NSUInteger)key_len];
  return k;
}

/* Validates a stored blob is a plausible .metallib before handing it to
 * newLibraryWithData (cheap integrity net against truncation / corruption /
 * a 1-in-2^160 key collision feeding a foreign blob). */
static bool d9mt_metallib_looks_valid(const void *bytes, size_t len) {
  return len >= sizeof(k_mtlb_magic) &&
         memcmp(bytes, k_mtlb_magic, sizeof(k_mtlb_magic)) == 0;
}

/* Cache lookup. Returns a retained MTLLibrary on a verified hit, else nil. */
static id<MTLLibrary>
d9mt_cache_lookup(id<MTLDevice> device, NSData *fullKey) {
  if (!d9mt_cache_enabled())
    return nil;

  pthread_mutex_lock(&s_cache_mutex);
  if (!d9mt_cache_ensure_open_locked()) {
    pthread_mutex_unlock(&s_cache_mutex);
    return nil;
  }

  /* Copy the blob out under the lock; release the lock before the (heavier)
   * newLibraryWithData. */
  NSData *blob = nil;
  sqlite3_reset(s_cache_get);
  sqlite3_clear_bindings(s_cache_get);
  sqlite3_bind_blob64(s_cache_get, 1, fullKey.bytes, fullKey.length,
                      SQLITE_STATIC);
  if (sqlite3_step(s_cache_get) == SQLITE_ROW) {
    const void *p = sqlite3_column_blob(s_cache_get, 0);
    int n = sqlite3_column_bytes(s_cache_get, 0);
    if (p && n > 0)
      blob = [NSData dataWithBytes:p length:(NSUInteger)n];
  }
  sqlite3_reset(s_cache_get);

  bool poisoned = false;
  if (blob && !d9mt_metallib_looks_valid(blob.bytes, blob.length)) {
    blob = nil;
    poisoned = true;
  }
  pthread_mutex_unlock(&s_cache_mutex);

  if (!blob) {
    if (poisoned) {
      /* Drop the bad row so a fresh compile heals it. */
      pthread_mutex_lock(&s_cache_mutex);
      sqlite3_stmt *del = NULL;
      if (sqlite3_prepare_v2(s_cache_db,
              "DELETE FROM shaders WHERE key = ?;", -1, &del, NULL) == SQLITE_OK) {
        sqlite3_bind_blob64(del, 1, fullKey.bytes, fullKey.length, SQLITE_STATIC);
        sqlite3_step(del);
        sqlite3_finalize(del);
      }
      pthread_mutex_unlock(&s_cache_mutex);
    }
    return nil;
  }

  dispatch_data_t dd = dispatch_data_create(
      blob.bytes, blob.length, NULL,
      DISPATCH_DATA_DESTRUCTOR_DEFAULT); /* copies; safe past blob lifetime */
  NSError *err = nil;
  id<MTLLibrary> lib = [device newLibraryWithData:dd error:&err];
  dispatch_release(dd);

  if (!lib) {
    /* Toolchain ABI drift not caught by toolchain_id: delete + degrade. */
    NSLog(@"[d9mtmetal] cached metallib rejected by newLibraryWithData: %@",
          err);
    pthread_mutex_lock(&s_cache_mutex);
    sqlite3_stmt *del = NULL;
    if (sqlite3_prepare_v2(s_cache_db, "DELETE FROM shaders WHERE key = ?;",
                           -1, &del, NULL) == SQLITE_OK) {
      sqlite3_bind_blob64(del, 1, fullKey.bytes, fullKey.length, SQLITE_STATIC);
      sqlite3_step(del);
      sqlite3_finalize(del);
    }
    pthread_mutex_unlock(&s_cache_mutex);
    return nil;
  }
  return lib; /* newLibrary* returns +1 */
}

/* Cache store (write-through). Best-effort: swallow all errors. */
static void d9mt_cache_store(NSData *fullKey, const void *bytes, size_t len) {
  if (!d9mt_cache_enabled() || !d9mt_metallib_looks_valid(bytes, len))
    return;
  pthread_mutex_lock(&s_cache_mutex);
  if (d9mt_cache_ensure_open_locked()) {
    sqlite3_reset(s_cache_set);
    sqlite3_clear_bindings(s_cache_set);
    sqlite3_bind_blob64(s_cache_set, 1, fullKey.bytes, fullKey.length,
                        SQLITE_STATIC);
    sqlite3_bind_blob64(s_cache_set, 2, bytes, len, SQLITE_STATIC);
    if (sqlite3_step(s_cache_set) != SQLITE_DONE)
      NSLog(@"[d9mtmetal] cache store failed: %s", sqlite3_errmsg(s_cache_db));
    sqlite3_reset(s_cache_set);
  }
  pthread_mutex_unlock(&s_cache_mutex);
}

/* --------------------------------------------------------------------------
 * Compile backends. Phase 1: CliBackend (out-of-process xcrun metal) is the
 * preferred producer; SourceBackend (newLibraryWithSource) is the guaranteed
 * floor. A future MscBackend/AirconvBackend is a new function registered
 * ahead of Cli, not a rewrite of this chain.
 * -------------------------------------------------------------------------- */

/* Resolve the absolute `metal` compiler path ONCE via `xcrun --find`. Returns
 * nil if the toolchain is absent (CliBackend then reports unavailable). */
static NSString *d9mt_metal_tool_path(void) {
  static NSString *s_path = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    int fds[2];
    if (pipe(fds) != 0)
      return;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);

    const char *argv[] = { "/usr/bin/xcrun", "--sdk", "macosx",
                           "--find", "metal", NULL };
    pid_t pid = 0;
    int rc = posix_spawn(&pid, argv[0], &fa, NULL,
                         (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0) { close(fds[0]); return; }

    char buf[PATH_MAX + 1] = { 0 };
    ssize_t total = 0, n;
    while (total < (ssize_t)sizeof(buf) - 1 &&
           (n = read(fds[0], buf + total, sizeof(buf) - 1 - total)) > 0)
      total += n;
    close(fds[0]);
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);

    /* strip trailing newline */
    while (total > 0 && (buf[total - 1] == '\n' || buf[total - 1] == '\r'))
      buf[--total] = '\0';
    if (total > 0 && WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0 &&
        access(buf, X_OK) == 0)
      s_path = [[NSString stringWithUTF8String:buf] retain];
  });
  return s_path;
}

/* CliBackend: write source to a temp .metal, spawn `metal` to produce a
 * .metallib, read the bytes back. Returns +0 NSData on success, nil on any
 * failure (so the caller degrades to SourceBackend). Out-of-process => does
 * NOT hold the Metal compile lock => no stutter even on a miss. */
static NSData *d9mt_cli_compile(const char *source, size_t source_len,
                                bool fast_math) {
  NSString *tool = d9mt_metal_tool_path();
  if (!tool)
    return nil;

  NSString *tmpl = [NSTemporaryDirectory()
      stringByAppendingPathComponent:@"d9mt-XXXXXX"];
  char tmpdir[PATH_MAX];
  strlcpy(tmpdir, [tmpl fileSystemRepresentation], sizeof(tmpdir));
  if (!mkdtemp(tmpdir))
    return nil;

  NSData *result = nil;
  char inPath[PATH_MAX], outPath[PATH_MAX];
  snprintf(inPath, sizeof(inPath), "%s/in.metal", tmpdir);
  snprintf(outPath, sizeof(outPath), "%s/out.metallib", tmpdir);

  int infd = open(inPath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  bool wrote = false;
  if (infd >= 0) {
    size_t off = 0;
    wrote = true;
    while (off < source_len) {
      ssize_t w = write(infd, source + off, source_len - off);
      if (w <= 0) { wrote = false; break; }
      off += (size_t)w;
    }
    close(infd);
  }

  if (wrote) {
    const char *argv[] = {
        tool.fileSystemRepresentation,
        "-std=metal3.0",
        // FAST, unconditionally — matches opts.mathMode=fast in the in-process
        // path (the fast_math arg is ignored; both paths must agree so the disk
        // cache is consistent regardless of which backend compiled an entry).
        "-ffast-math",
        "-o", outPath, inPath, NULL };
    pid_t pid = 0;
    int rc = posix_spawn(&pid, argv[0], NULL, NULL,
                         (char *const *)argv, environ);
    if (rc == 0) {
      int wstatus = 0;
      waitpid(pid, &wstatus, 0);
      if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0)
        result = [NSData dataWithContentsOfFile:
                    [NSString stringWithUTF8String:outPath]];
    }
  }

  unlink(inPath);
  unlink(outPath);
  rmdir(tmpdir);

  if (result && !d9mt_metallib_looks_valid(result.bytes, result.length))
    result = nil;
  return result;
}

/* SourceBackend: the live, always-available floor. Returns a retained
 * MTLLibrary directly (the one backend exempt from the bytes contract). */
static id<MTLLibrary>
d9mt_source_compile(id<MTLDevice> device, const char *source,
                    size_t source_len, bool fast_math, NSError **err_out) {
  NSString *src = [[NSString alloc] initWithBytes:source
                                           length:source_len
                                         encoding:NSUTF8StringEncoding];
  if (!src)
    return nil;
  MTLCompileOptions *opts = [[MTLCompileOptions alloc] init];
  opts.languageVersion = MTLLanguageVersion3_0;
  opts.mathMode = MTLMathModeFast;  // fast always — see d9mt_library_for_source
  id<MTLLibrary> lib = [device newLibraryWithSource:src options:opts
                                              error:err_out];
  [opts release];
  [src release];
  return lib;
}

static NTSTATUS d9mt_library_for_key(void *args) {
  struct d9mt_library_params *p = args;
  p->ret_library = 0;
  p->ret_status = D9MT_LIBRARY_FAILED;
  p->ret_error = 0;

  /* A wine unixcall thread has no ambient autorelease pool; without this every
   * autoreleased temporary (NSData/NSString/cache + the +0 out-param NSErrors)
   * would leak for the process lifetime, once per compile. Retained returns
   * (the +1 MTLLibrary handles, the [err retain] error) survive the drain.
   * Mirrors d9mt_new_render_pso, which wraps its body for the same reason. */
  @autoreleasepool {
    id<MTLDevice> device = (id<MTLDevice>)(uintptr_t)p->device;
    if (!device)
      return STATUS_SUCCESS;

    const char *source = (const char *)(uintptr_t)p->source_ptr;
    size_t source_len = (size_t)p->source_len;
    bool fast_math = (p->target_flags & D9MT_TARGET_FAST_MATH) != 0;

    NSData *fullKey = d9mt_cache_key((const void *)(uintptr_t)p->key_ptr,
                                     p->key_len);

    /* 1. cache hit -> load, no source compiler. */
    id<MTLLibrary> lib = d9mt_cache_lookup(device, fullKey);
    if (lib) {
      p->ret_library = (uint64_t)(uintptr_t)lib;
      p->ret_status = D9MT_LIBRARY_HIT;
      return STATUS_SUCCESS;
    }

    /* 2. miss -> compile out-of-process (Cli), store bytes, load. Only MSL
     * source is supported by the Phase 1 backends. */
    if (source && source_len &&
        p->source_kind == (uint32_t)D9MT_SOURCE_MSL_TEXT) {
      NSData *bytes = d9mt_cli_compile(source, source_len, fast_math);
      if (bytes) {
        d9mt_cache_store(fullKey, bytes.bytes, bytes.length);
        dispatch_data_t dd = dispatch_data_create(
            bytes.bytes, bytes.length, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NSError *err = nil;
        lib = [device newLibraryWithData:dd error:&err];
        dispatch_release(dd);
        if (lib) {
          p->ret_library = (uint64_t)(uintptr_t)lib;
          p->ret_status = D9MT_LIBRARY_COMPILED;
          return STATUS_SUCCESS;
        }
        /* Freshly compiled bytes that won't load: fall through to source. err
         * is a +0 out-param (autoreleased) — do NOT release it, the pool
         * reclaims it. */
      }
    }

    /* 3. floor: live source compile, never worse than today. */
    if (source && source_len) {
      NSError *err = nil;
      lib = d9mt_source_compile(device, source, source_len, fast_math, &err);
      if (lib) {
        p->ret_library = (uint64_t)(uintptr_t)lib;
        p->ret_status = D9MT_LIBRARY_FELL_BACK;
        /* err here is at most a +0 autoreleased warning — leave it to the
         * pool, never release a +0 out-param. */
        return STATUS_SUCCESS;
      }
      /* retain to hand the failure error up across the ABI (+1). */
      p->ret_error = (uint64_t)(uintptr_t)[err retain];
    }
  }

  return STATUS_SUCCESS;
}

typedef NTSTATUS (*unixlib_entry_t)(void *args);

__attribute__((visibility("default")))
const unixlib_entry_t __wine_unix_call_funcs[D9MT_FUNC_COUNT] = {
    d9mt_new_library_from_source,
    d9mt_new_render_pso,
    d9mt_library_for_key,
};

/* identical param layouts for 32-bit callers (see header ABI rule) */
__attribute__((visibility("default")))
const unixlib_entry_t __wine_unix_call_wow64_funcs[D9MT_FUNC_COUNT] = {
    d9mt_new_library_from_source,
    d9mt_new_render_pso,
    d9mt_library_for_key,
};
