/* tar — the root filesystem's format: a POSIX ustar archive, read in place.
 *
 * Every 512-byte header is checksum-verified and its octal fields are
 * parsed with bounds; an entry whose data would run past the archive is
 * rejected, not truncated. Names are normalised ("./bin/x", "/bin/x" and
 * "bin/x" are one file; a directory's trailing "/" is dropped). PAX and GNU
 * extension records are skipped. Pure: nothing is copied but the name. */
#ifndef MINIKERN_CORE_TAR_H
#define MINIKERN_CORE_TAR_H

#include <stddef.h>
#include <stdint.h>

#define TAR_BLOCK 512u
#define TAR_NAME_MAX 256u  /* ustar's 155-byte prefix + '/' + 100-byte name */

#define TAR_FILE    '0'
#define TAR_SYMLINK '2'
#define TAR_DIR     '5'

struct tar_entry {
    char name[TAR_NAME_MAX];  /* normalised, no leading "/" or "./" */
    char type;                /* TAR_FILE, TAR_DIR, TAR_SYMLINK, ... */
    uint32_t mode;            /* permission bits */
    uint32_t size;
    const uint8_t *data;      /* inside the archive */
    char link[101];           /* a symlink's target */
};

/* Reads the entry at *offset and advances it past the entry's data.
 * Returns KERR_OK, KERR_NOENT at the end-of-archive marker (or the end of
 * the buffer), or KERR_INVAL for a corrupt header. */
int tar_next(const uint8_t *archive, size_t len, size_t *offset, struct tar_entry *out);

/* Finds path (normalised as above). KERR_OK, KERR_NOENT, or KERR_INVAL
 * when a corrupt header is met before the entry. */
int tar_lookup(const uint8_t *archive, size_t len, const char *path, struct tar_entry *out);

/* Normalises path into out (TAR_NAME_MAX bytes). KERR_NAMETOOLONG if it
 * does not fit. */
int tar_normalise(const char *path, char *out);

#endif
