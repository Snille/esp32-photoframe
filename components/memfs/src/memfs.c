#include "memfs.h"

#include <string.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs.h"

static const char *TAG = "memfs";

typedef struct {
    char *name;
    uint8_t *data;
    size_t size;
    size_t capacity;
} mem_file_t;

typedef struct {
    mem_file_t *file;
    size_t offset;
    int flags;
} mem_fd_t;

static mem_file_t **files = NULL;
static size_t max_files_count = 0;
static mem_fd_t *fds = NULL;
static size_t max_fds_count = 0;

static int memfs_open_vfs(void *ctx, const char *path, int flags, int mode)
{
    // Skip leading slash
    if (path[0] == '/')
        path++;

    int fd = -1;
    for (int i = 0; i < max_fds_count; i++) {
        if (fds[i].file == NULL) {
            fd = i;
            break;
        }
    }

    if (fd == -1) {
        errno = ENFILE;
        return -1;
    }

    mem_file_t *file = NULL;
    for (int i = 0; i < max_files_count; i++) {
        if (files[i] && strcmp(files[i]->name, path) == 0) {
            file = files[i];
            break;
        }
    }

    if (file) {
        if ((flags & O_CREAT) && (flags & O_EXCL)) {
            errno = EEXIST;
            return -1;
        }
        if (flags & O_TRUNC) {
            file->size = 0;
        }
    } else {
        if (!(flags & O_CREAT)) {
            errno = ENOENT;
            return -1;
        }

        int file_idx = -1;
        for (int i = 0; i < max_files_count; i++) {
            if (files[i] == NULL) {
                file_idx = i;
                break;
            }
        }

        if (file_idx == -1) {
            errno = ENOSPC;
            return -1;
        }

        file = (mem_file_t *) calloc(1, sizeof(mem_file_t));
        if (!file) {
            errno = ENOMEM;
            return -1;
        }
        file->name = strdup(path);
        files[file_idx] = file;
    }

    fds[fd].file = file;
    fds[fd].offset = (flags & O_APPEND) ? file->size : 0;
    fds[fd].flags = flags;

    return fd;
}

static ssize_t memfs_write_vfs(void *ctx, int fd, const void *data, size_t size)
{
    if (fd < 0 || fd >= max_fds_count || fds[fd].file == NULL) {
        errno = EBADF;
        return -1;
    }

    mem_file_t *file = fds[fd].file;
    size_t offset = fds[fd].offset;

    if (offset + size > file->capacity) {
        size_t new_capacity = (offset + size + 4095) & ~4095;
        uint8_t *new_data = heap_caps_realloc(file->data, new_capacity, MALLOC_CAP_SPIRAM);
        if (!new_data) {
            new_data = heap_caps_realloc(file->data, new_capacity, MALLOC_CAP_8BIT);
        }
        if (!new_data) {
            errno = ENOMEM;
            return -1;
        }
        file->data = new_data;
        file->capacity = new_capacity;
    }

    memcpy(file->data + offset, data, size);
    fds[fd].offset += size;
    if (fds[fd].offset > file->size) {
        file->size = fds[fd].offset;
    }

    return size;
}

static ssize_t memfs_read_vfs(void *ctx, int fd, void *dst, size_t size)
{
    if (fd < 0 || fd >= max_fds_count || fds[fd].file == NULL) {
        errno = EBADF;
        return -1;
    }

    mem_file_t *file = fds[fd].file;
    size_t offset = fds[fd].offset;

    if (offset >= file->size) {
        return 0;
    }

    size_t available = file->size - offset;
    size_t to_read = (size < available) ? size : available;

    memcpy(dst, file->data + offset, to_read);
    fds[fd].offset += to_read;

    return to_read;
}

static off_t memfs_lseek_vfs(void *ctx, int fd, off_t offset, int mode)
{
    if (fd < 0 || fd >= max_fds_count || fds[fd].file == NULL) {
        errno = EBADF;
        return -1;
    }

    mem_file_t *file = fds[fd].file;
    size_t new_offset = 0;

    switch (mode) {
    case SEEK_SET:
        new_offset = offset;
        break;
    case SEEK_CUR:
        new_offset = fds[fd].offset + offset;
        break;
    case SEEK_END:
        new_offset = file->size + offset;
        break;
    default:
        errno = EINVAL;
        return -1;
    }

    fds[fd].offset = new_offset;
    return new_offset;
}

static int memfs_close_vfs(void *ctx, int fd)
{
    if (fd < 0 || fd >= max_fds_count || fds[fd].file == NULL) {
        errno = EBADF;
        return -1;
    }

    fds[fd].file = NULL;
    return 0;
}

static int memfs_fstat_vfs(void *ctx, int fd, struct stat *st)
{
    if (fd < 0 || fd >= max_fds_count || fds[fd].file == NULL) {
        errno = EBADF;
        return -1;
    }

    mem_file_t *file = fds[fd].file;
    memset(st, 0, sizeof(*st));
    st->st_size = file->size;
    st->st_mode = S_IFREG | 0666;
    return 0;
}

static int memfs_stat_vfs(void *ctx, const char *path, struct stat *st)
{
    if (path[0] == '/')
        path++;

    for (int i = 0; i < max_files_count; i++) {
        if (files[i] && strcmp(files[i]->name, path) == 0) {
            memset(st, 0, sizeof(*st));
            st->st_size = files[i]->size;
            st->st_mode = S_IFREG | 0666;
            return 0;
        }
    }

    errno = ENOENT;
    return -1;
}
static int memfs_unlink_vfs(void *ctx, const char *path)
{
    if (path[0] == '/')
        path++;

    for (int i = 0; i < max_files_count; i++) {
        if (files[i] && strcmp(files[i]->name, path) == 0) {
            // Check if any FD is open for this file
            for (int j = 0; j < max_fds_count; j++) {
                if (fds[j].file == files[i]) {
                    errno = EBUSY;
                    return -1;
                }
            }

            free(files[i]->name);
            if (files[i]->data)
                heap_caps_free(files[i]->data);
            free(files[i]);
            files[i] = NULL;
            return 0;
        }
    }

    errno = ENOENT;
    return -1;
}

static int memfs_rename_vfs(void *ctx, const char *src, const char *dst)
{
    if (src[0] == '/')
        src++;
    if (dst[0] == '/')
        dst++;

    mem_file_t *src_file = NULL;
    for (int i = 0; i < max_files_count; i++) {
        if (files[i] && strcmp(files[i]->name, src) == 0) {
            src_file = files[i];
            break;
        }
    }

    if (!src_file) {
        errno = ENOENT;
        return -1;
    }

    // Check if src is open
    for (int j = 0; j < max_fds_count; j++) {
        if (fds[j].file == src_file) {
            errno = EBUSY;
            return -1;
        }
    }

    // If destination exists, unlink it
    for (int i = 0; i < max_files_count; i++) {
        if (files[i] && strcmp(files[i]->name, dst) == 0) {
            // Check if dst is open
            for (int j = 0; j < max_fds_count; j++) {
                if (fds[j].file == files[i]) {
                    errno = EBUSY;
                    return -1;
                }
            }
            free(files[i]->name);
            if (files[i]->data)
                heap_caps_free(files[i]->data);
            free(files[i]);
            files[i] = NULL;
            break;
        }
    }

    char *new_name = strdup(dst);
    if (!new_name) {
        errno = ENOMEM;
        return -1;
    }

    free(src_file->name);
    src_file->name = new_name;

    return 0;
}

esp_err_t memfs_mount(const char *base_path, size_t max_files)
{
    if (files)
        return ESP_ERR_INVALID_STATE;

    max_files_count = max_files;
    files = (mem_file_t **) calloc(max_files_count, sizeof(mem_file_t *));

    max_fds_count = max_files * 2;
    fds = (mem_fd_t *) calloc(max_fds_count, sizeof(mem_fd_t));

    esp_vfs_t vfs = {
        .flags = ESP_VFS_FLAG_CONTEXT_PTR,
        .open_p = &memfs_open_vfs,
        .write_p = &memfs_write_vfs,
        .read_p = &memfs_read_vfs,
        .lseek_p = &memfs_lseek_vfs,
        .close_p = &memfs_close_vfs,
        .fstat_p = &memfs_fstat_vfs,
        .stat_p = &memfs_stat_vfs,
        .unlink_p = &memfs_unlink_vfs,
        .rename_p = &memfs_rename_vfs,
    };

    ESP_ERROR_CHECK(esp_vfs_register(base_path, &vfs, NULL));
    ESP_LOGI(TAG, "Mounted RAM filesystem at %s", base_path);
    return ESP_OK;
}

esp_err_t memfs_unmount(const char *base_path)
{
    // Simplified: unregister and free all
    esp_vfs_unregister(base_path);

    for (int i = 0; i < max_files_count; i++) {
        if (files[i]) {
            free(files[i]->name);
            if (files[i]->data)
                heap_caps_free(files[i]->data);
            free(files[i]);
        }
    }
    free(files);
    files = NULL;
    free(fds);
    fds = NULL;

    return ESP_OK;
}

size_t memfs_get_total_used(void)
{
    size_t total = 0;
    if (files) {
        for (int i = 0; i < max_files_count; i++) {
            if (files[i])
                total += files[i]->capacity;
        }
    }
    return total;
}
