/* wolf3d_storage.cpp — POSIX-IO Wrapper für Wolf4SDL.
 *
 * Wolf4SDL nutzt `open/read/write/lseek/close` mit O_RDONLY/O_BINARY und
 * `stat()` direkt. Auf dem ESP32 hängt die SD-Karte an Furi-Storage,
 * Newlibs VFS-Layer kennt die /ext-Pfade nicht. Daher: alle low-level IO-Aufrufe
 * werden via Macros in SDL.h auf wolf_open / wolf_read / … umgelenkt,
 * implementiert hier mit Furi-Storage-Records.
 *
 * Wolf4SDL übergibt typischerweise relative Pfade ("vswap.wl1"). Wir
 * prepend automatisch /ext/apps_data/wolf3d/ wenn der Pfad nicht absolut
 * ist.
 *
 * Save-Game-IO (FILE*-basiert) ist Stage-2; aktuell als Stub.
 */

#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include <furi.h>
#include <storage/storage.h>

#include "SDL.h"

#define TAG "Wolf3D-IO"

#define WOLF3D_DATA_DIR "/ext/apps_data/wolf3d/"

#define MAX_FDS 16

namespace {

struct WolfFd {
    File*   f;
    bool    in_use;
};

WolfFd      s_fds[MAX_FDS];
Storage*    s_storage = nullptr;
FuriMutex*  s_storage_mutex = nullptr;

Storage* get_storage() {
    if(!s_storage) {
        s_storage = (Storage*)furi_record_open(RECORD_STORAGE);
        s_storage_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    }
    return s_storage;
}

int alloc_fd(File* f) {
    for(int i = 1; i < MAX_FDS; i++) {
        if(!s_fds[i].in_use) {
            s_fds[i].f = f;
            s_fds[i].in_use = true;
            return i;
        }
    }
    return -1;
}

File* lookup(int fd) {
    if(fd <= 0 || fd >= MAX_FDS) return nullptr;
    return s_fds[fd].in_use ? s_fds[fd].f : nullptr;
}

void rewrite_path(const char* in, char* out, size_t outsz) {
    if(!in) { if(outsz) out[0] = 0; return; }
    if(in[0] == '/') {
        snprintf(out, outsz, "%s", in);
    } else {
        snprintf(out, outsz, WOLF3D_DATA_DIR "%s", in);
    }
}

} /* namespace */

extern "C" int wolf_open(const char* path, int flags, ...) {
    Storage* s = get_storage();
    if(!s) return -1;

    char p[256];
    rewrite_path(path, p, sizeof(p));

    File* f = storage_file_alloc(s);
    FS_AccessMode am = (flags & 0x01) ? FSAM_WRITE : FSAM_READ;
    FS_OpenMode om = (flags & 0x40) ? FSOM_OPEN_ALWAYS : FSOM_OPEN_EXISTING;
    if(am == FSAM_WRITE) om = FSOM_CREATE_ALWAYS;

    if(!storage_file_open(f, p, am, om)) {
        FURI_LOG_W(TAG, "open '%s' failed", p);
        storage_file_free(f);
        return -1;
    }
    int fd = alloc_fd(f);
    if(fd < 0) {
        storage_file_close(f);
        storage_file_free(f);
        return -1;
    }
    return fd;
}

extern "C" int wolf_close(int fd) {
    File* f = lookup(fd);
    if(!f) return -1;
    storage_file_close(f);
    storage_file_free(f);
    s_fds[fd].in_use = false;
    s_fds[fd].f = nullptr;
    return 0;
}

extern "C" int32_t wolf_read(int fd, void* buf, size_t n) {
    File* f = lookup(fd);
    if(!f || !buf) return -1;
    return (int32_t)storage_file_read(f, buf, n);
}

extern "C" int32_t wolf_write(int fd, const void* buf, size_t n) {
    File* f = lookup(fd);
    if(!f || !buf) return -1;
    return (int32_t)storage_file_write(f, buf, n);
}

/* lseek: whence 0=SET, 1=CUR, 2=END. Furi unterstützt SET und CUR; END muss
 * via Größe simuliert werden. */
extern "C" int32_t wolf_lseek(int fd, int32_t offset, int whence) {
    File* f = lookup(fd);
    if(!f) return -1;
    uint64_t target = 0;
    if(whence == 0) {
        target = (uint64_t)offset;
    } else if(whence == 1) {
        target = storage_file_tell(f) + offset;
    } else if(whence == 2) {
        target = storage_file_size(f) + offset;
    } else {
        return -1;
    }
    if(!storage_file_seek(f, (uint32_t)target, true)) return -1;
    return (int32_t)target;
}

extern "C" int wolf_stat(const char* path, void* /*statbuf*/) {
    Storage* s = get_storage();
    if(!s) return -1;
    char p[256];
    rewrite_path(path, p, sizeof(p));
    FileInfo info;
    FS_Error err = storage_common_stat(s, p, &info);
    if(err != FSE_OK) return -1;
    /* statbuf ist (struct stat*); wir füllen es nicht, weil Wolf4SDL nur
     * den Rückgabewert prüft (`if(!stat(...))` = "Datei existiert"). */
    return 0;
}

extern "C" int wolf_unlink(const char* path) {
    Storage* s = get_storage();
    if(!s) return -1;
    char p[256];
    rewrite_path(path, p, sizeof(p));
    FS_Error err = storage_common_remove(s, p);
    return (err == FSE_OK) ? 0 : -1;
}

extern "C" int wolf_mkdir(const char* path, int /*mode*/) {
    Storage* s = get_storage();
    if(!s) return -1;
    char p[256];
    rewrite_path(path, p, sizeof(p));
    FS_Error err = storage_common_mkdir(s, p);
    return (err == FSE_OK || err == FSE_EXIST) ? 0 : -1;
}

/* ---- stdio FILE* wrapper ---- */
struct WolfFile {
    File* f;
    bool  eof;
};

extern "C" WolfFile* wolf_fopen(const char* path, const char* mode) {
    Storage* s = get_storage();
    if(!s || !path || !mode) return nullptr;
    char p[256];
    rewrite_path(path, p, sizeof(p));

    bool write = (strchr(mode, 'w') != nullptr) || (strchr(mode, 'a') != nullptr);
    FS_AccessMode am = write ? FSAM_WRITE : FSAM_READ;
    FS_OpenMode  om = write ? FSOM_CREATE_ALWAYS : FSOM_OPEN_EXISTING;

    File* f = storage_file_alloc(s);
    if(!storage_file_open(f, p, am, om)) {
        storage_file_free(f);
        return nullptr;
    }
    WolfFile* wf = (WolfFile*)heap_caps_malloc(sizeof(WolfFile), MALLOC_CAP_INTERNAL);
    if(!wf) { storage_file_close(f); storage_file_free(f); return nullptr; }
    wf->f = f;
    wf->eof = false;
    return wf;
}

extern "C" int wolf_fclose(WolfFile* wf) {
    if(!wf) return -1;
    if(wf->f) { storage_file_close(wf->f); storage_file_free(wf->f); }
    heap_caps_free(wf);
    return 0;
}

extern "C" size_t wolf_fread(void* buf, size_t sz, size_t n, WolfFile* wf) {
    if(!wf || !wf->f || !buf) return 0;
    size_t total = sz * n;
    if(!total) return 0;
    uint16_t got = storage_file_read(wf->f, buf, total);
    if(got < total) wf->eof = true;
    return got / sz;  /* Anzahl der vollständigen Items */
}

extern "C" size_t wolf_fwrite(const void* buf, size_t sz, size_t n, WolfFile* wf) {
    if(!wf || !wf->f || !buf) return 0;
    size_t total = sz * n;
    if(!total) return 0;
    uint16_t put = storage_file_write(wf->f, buf, total);
    return put / sz;
}

extern "C" int wolf_fseek(WolfFile* wf, long off, int whence) {
    if(!wf || !wf->f) return -1;
    uint64_t target = 0;
    if(whence == 0)      target = (uint64_t)off;
    else if(whence == 1) target = storage_file_tell(wf->f) + off;
    else if(whence == 2) target = storage_file_size(wf->f) + off;
    else return -1;
    wf->eof = false;
    return storage_file_seek(wf->f, (uint32_t)target, true) ? 0 : -1;
}

extern "C" long wolf_ftell(WolfFile* wf) {
    if(!wf || !wf->f) return -1;
    return (long)storage_file_tell(wf->f);
}

extern "C" int wolf_feof(WolfFile* wf) {
    return (wf && wf->eof) ? 1 : 0;
}
