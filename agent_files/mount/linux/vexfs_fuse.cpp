#define FUSE_USE_VERSION 35

#include <fuse3/fuse.h>

#include "vexfs_runtime_io.h"

#include <fcntl.h>
#include <sys/statvfs.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1u << 0)
#endif
#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1u << 1)
#endif

namespace {

struct FuseState {
    vexfs_mount_session *session = nullptr;
    std::string backend;
    std::string connection;
    std::string request_prefix;
    std::recursive_mutex mutex;
    std::atomic<uint64_t> request_sequence{1};
};

struct FileHandle {
    std::string id;
    int64_t generation = 0;
    bool dirty = false;
    bool append = false;
};

FuseState *State() {
    const fuse_context *context = fuse_get_context();
    return context == nullptr ? nullptr : static_cast<FuseState *>(context->private_data);
}

std::string RequestId(FuseState *state, const char *operation) {
    return state->request_prefix + "-" + operation + "-" +
        std::to_string(state->request_sequence.fetch_add(1));
}

std::string NewRequestPrefix() {
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    std::random_device random;
    const uint64_t entropy =
        (static_cast<uint64_t>(random()) << 32) ^ static_cast<uint64_t>(random());
    return std::string("fuse-") + std::to_string(static_cast<uint64_t>(getpid())) + "-" +
        std::to_string(now) + "-" + std::to_string(entropy);
}

int Errno(vexfs_mount_status status) {
    switch (status) {
        case VEXFS_MOUNT_OK: return 0;
        case VEXFS_MOUNT_INVALID_ARGUMENT: return EINVAL;
        case VEXFS_MOUNT_NOT_FOUND: return ENOENT;
        case VEXFS_MOUNT_CONFLICT: return EEXIST;
        case VEXFS_MOUNT_READ_ONLY: return EROFS;
        case VEXFS_MOUNT_BUSY: return EBUSY;
        case VEXFS_MOUNT_PERMISSION_DENIED: return EACCES;
        case VEXFS_MOUNT_NO_SPACE: return ENOSPC;
        case VEXFS_MOUNT_UNSUPPORTED: return ENOTSUP;
        case VEXFS_MOUNT_NOT_EMPTY: return ENOTEMPTY;
        case VEXFS_MOUNT_CORRUPTION: return EIO;
        case VEXFS_MOUNT_DATABASE_ERROR: return EIO;
        case VEXFS_MOUNT_INTERNAL_ERROR: return EIO;
    }
    return EIO;
}

int Result(vexfs_mount_status status) {
    return status == VEXFS_MOUNT_OK ? 0 : -Errno(status);
}

std::string TakeBytes(vexfs_mount_bytes *bytes) {
    std::string result;
    if (bytes->data != nullptr) {
        result.assign(static_cast<const char *>(bytes->data), static_cast<size_t>(bytes->size));
        vexfs_mount_free(bytes->data);
    }
    bytes->data = nullptr;
    bytes->size = 0;
    return result;
}

std::string JsonUnescape(const std::string &json, size_t *position) {
    std::string value;
    while (*position < json.size()) {
        const char character = json[(*position)++];
        if (character == '"') break;
        if (character != '\\' || *position >= json.size()) {
            value.push_back(character);
            continue;
        }
        const char escaped = json[(*position)++];
        switch (escaped) {
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            default: value.push_back('?'); break;
        }
    }
    return value;
}

int64_t JsonInteger(const std::string &json, const std::string &name) {
    const std::string marker = "\"" + name + "\":";
    size_t position = json.find(marker);
    if (position == std::string::npos) throw std::runtime_error("missing JSON integer: " + name);
    position += marker.size();
    char *end = nullptr;
    const long long value = std::strtoll(json.c_str() + position, &end, 10);
    if (end == json.c_str() + position) throw std::runtime_error("invalid JSON integer: " + name);
    return static_cast<int64_t>(value);
}

int64_t JsonOptionalInteger(const std::string &json, const std::string &name,
                            int64_t fallback) {
    const std::string marker = "\"" + name + "\":";
    size_t position = json.find(marker);
    if (position == std::string::npos) throw std::runtime_error("missing JSON integer: " + name);
    position += marker.size();
    while (position < json.size() && std::isspace(static_cast<unsigned char>(json[position])))
        ++position;
    if (json.compare(position, 4, "null") == 0) return fallback;
    char *end = nullptr;
    const long long value = std::strtoll(json.c_str() + position, &end, 10);
    if (end == json.c_str() + position) throw std::runtime_error("invalid JSON integer: " + name);
    return static_cast<int64_t>(value);
}

std::string JsonString(const std::string &json, const std::string &name) {
    const std::string marker = "\"" + name + "\":\"";
    size_t position = json.find(marker);
    if (position == std::string::npos) throw std::runtime_error("missing JSON string: " + name);
    position += marker.size();
    return JsonUnescape(json, &position);
}

std::vector<std::string> JsonObjects(const std::string &json) {
    std::vector<std::string> objects;
    bool in_string = false;
    bool escaped = false;
    int depth = 0;
    size_t start = 0;
    for (size_t position = 0; position < json.size(); ++position) {
        const char character = json[position];
        if (in_string) {
            if (escaped) escaped = false;
            else if (character == '\\') escaped = true;
            else if (character == '"') in_string = false;
            continue;
        }
        if (character == '"') in_string = true;
        else if (character == '{') {
            if (depth++ == 0) start = position;
        } else if (character == '}' && depth > 0) {
            if (--depth == 0) objects.push_back(json.substr(start, position - start + 1));
        }
    }
    return objects;
}

std::vector<std::string> JsonStrings(const std::string &json) {
    std::vector<std::string> values;
    size_t position = 0;
    while (position < json.size()) {
        position = json.find('"', position);
        if (position == std::string::npos) break;
        ++position;
        values.push_back(JsonUnescape(json, &position));
    }
    return values;
}

int StatPath(FuseState *state, const char *path, std::string *json) {
    vexfs_mount_bytes bytes{};
    vexfs_mount_error error{};
    const vexfs_mount_status status = vexfs_mount_stat(state->session, path, &bytes, &error);
    if (status != VEXFS_MOUNT_OK) return -Errno(status);
    *json = TakeBytes(&bytes);
    return 0;
}

int ChownCreatedPath(FuseState *state, const char *path) {
    const fuse_context *context = fuse_get_context();
    if (context == nullptr) return -EIO;
    try {
        std::string json;
        const int stat_result = StatPath(state, path, &json);
        if (stat_result != 0) return stat_result;
        vexfs_mount_error error{};
        return Result(vexfs_mount_chown(state->session, JsonInteger(json, "inode"),
                                        static_cast<int64_t>(context->uid),
                                        static_cast<int64_t>(context->gid), &error));
    } catch (...) {
        return -EIO;
    }
}

void FillStat(const std::string &json, struct stat *attributes) {
    std::memset(attributes, 0, sizeof(*attributes));
    const std::string kind = JsonString(json, "kind");
    const mode_t permissions = static_cast<mode_t>(JsonInteger(json, "mode") & 0777);
    if (kind == "directory") attributes->st_mode = S_IFDIR | permissions;
    else if (kind == "symlink") attributes->st_mode = S_IFLNK | 0777;
    else attributes->st_mode = S_IFREG | permissions;
    attributes->st_ino = static_cast<ino_t>(JsonInteger(json, "inode"));
    attributes->st_nlink = static_cast<nlink_t>(JsonInteger(json, "link_count"));
    attributes->st_size = static_cast<off_t>(JsonInteger(json, "size"));
    attributes->st_uid = static_cast<uid_t>(JsonInteger(json, "uid"));
    attributes->st_gid = static_cast<gid_t>(JsonInteger(json, "gid"));
    attributes->st_blksize = 4096;
    attributes->st_blocks = (attributes->st_size + 511) / 512;
    const int64_t created_ms = JsonInteger(json, "created_at");
    const int64_t accessed_ms = JsonInteger(json, "accessed_at");
    const int64_t updated_ms = JsonInteger(json, "updated_at");
    const int64_t changed_ms = JsonInteger(json, "changed_at");
    (void)created_ms;  // Linux stat 没有独立 birthtime 字段。
    attributes->st_ctim = {changed_ms / 1000, (changed_ms % 1000) * 1000000};
    attributes->st_mtim = {updated_ms / 1000, (updated_ms % 1000) * 1000000};
    attributes->st_atim = {accessed_ms / 1000, (accessed_ms % 1000) * 1000000};
}

int GetAttr(const char *path, struct stat *attributes, fuse_file_info *) {
    FuseState *state = State();
    if (state == nullptr) return -EIO;
    std::lock_guard<std::recursive_mutex> guard(state->mutex);
    try {
        std::string json;
        const int result = StatPath(state, path, &json);
        if (result != 0) return result;
        FillStat(json, attributes);
        return 0;
    } catch (...) {
        return -EIO;
    }
}

int ReadLink(const char *path, char *buffer, size_t size) {
    if (size == 0) return -EINVAL;
    FuseState *state = State();
    if (state == nullptr) return -EIO;
    std::lock_guard<std::recursive_mutex> guard(state->mutex);
    try {
        std::string json;
        const int stat_result = StatPath(state, path, &json);
        if (stat_result != 0) return stat_result;
        vexfs_mount_bytes target{};
        vexfs_mount_error error{};
        const auto status = vexfs_mount_readlink(
            state->session, JsonInteger(json, "inode"), &target, &error);
        if (status != VEXFS_MOUNT_OK) return -Errno(status);
        const std::string value = TakeBytes(&target);
        const size_t count = std::min(value.size(), size - 1);
        std::memcpy(buffer, value.data(), count);
        buffer[count] = '\0';
        return 0;
    } catch (...) {
        return -EIO;
    }
}

int MakeNode(const char *path, mode_t mode, dev_t) {
    if (!S_ISREG(mode)) return -ENOTSUP;
    FuseState *state = State();
    if (state == nullptr) return -EIO;
    std::lock_guard<std::recursive_mutex> guard(state->mutex);
    vexfs_mount_error error{};
    const int create_result =
        Result(vexfs_mount_create(state->session, path, "file", mode & 0777, &error));
    return create_result == 0 ? ChownCreatedPath(state, path) : create_result;
}

int MakeDirectory(const char *path, mode_t mode) {
    FuseState *state = State();
    if (state == nullptr) return -EIO;
    std::lock_guard<std::recursive_mutex> guard(state->mutex);
    vexfs_mount_error error{};
    const auto status = vexfs_mount_mkdir(state->session, path, &error);
    if (status != VEXFS_MOUNT_OK) return -Errno(status);
    std::string json;
    const int stat_result = StatPath(state, path, &json);
    if (stat_result != 0) return stat_result;
    const int mode_result = Result(vexfs_mount_set_mode(
        state->session, JsonInteger(json, "inode"), mode & 0777, &error));
    return mode_result == 0 ? ChownCreatedPath(state, path) : mode_result;
}

int RemovePath(const char *path) {
    FuseState *state = State();
    if (state == nullptr) return -EIO;
    std::lock_guard<std::recursive_mutex> guard(state->mutex);
    vexfs_mount_error error{};
    return Result(vexfs_mount_remove(state->session, path, 0, &error));
}

int CreateSymlink(const char *target, const char *path) {
    FuseState *state = State();
    if (state == nullptr) return -EIO;
    std::lock_guard<std::recursive_mutex> guard(state->mutex);
    vexfs_mount_error error{};
    const int create_result = Result(vexfs_mount_symlink(
        state->session, path, target, std::strlen(target), &error));
    return create_result == 0 ? ChownCreatedPath(state, path) : create_result;
}

int RenamePath(const char *source, const char *destination, unsigned int flags) {
    if ((flags & RENAME_EXCHANGE) != 0 || (flags & ~(RENAME_EXCHANGE | RENAME_NOREPLACE)) != 0)
        return -ENOTSUP;
    FuseState *state = State();
    if (state == nullptr) return -EIO;
    std::lock_guard<std::recursive_mutex> guard(state->mutex);
    vexfs_mount_error error{};
    const int replace = (flags & RENAME_NOREPLACE) == 0 ? 1 : 0;
    return Result(vexfs_mount_rename(state->session, source, destination, replace, &error));
}

int LinkPath(const char *source, const char *destination) {
    FuseState *state = State();
    if (state == nullptr) return -EIO;
    std::lock_guard<std::recursive_mutex> guard(state->mutex);
    vexfs_mount_error error{};
    return Result(vexfs_mount_link(state->session, source, destination, &error));
}

int ChmodPath(const char *path, mode_t mode, fuse_file_info *) {
    FuseState *state = State();
    if (state == nullptr) return -EIO;
    std::lock_guard<std::recursive_mutex> guard(state->mutex);
    try {
        std::string json;
        const int stat_result = StatPath(state, path, &json);
        if (stat_result != 0) return stat_result;
        vexfs_mount_error error{};
        return Result(vexfs_mount_set_mode(state->session, JsonInteger(json, "inode"),
                                           mode & 0777, &error));
    } catch (...) {
        return -EIO;
    }
}

int ChownPath(const char *path, uid_t uid, gid_t gid, fuse_file_info *) {
    FuseState *state = State();
    if (state == nullptr) return -EIO;
    std::lock_guard<std::recursive_mutex> guard(state->mutex);
    try {
        std::string json;
        const int stat_result = StatPath(state, path, &json);
        if (stat_result != 0) return stat_result;
        vexfs_mount_error error{};
        const int64_t owner = uid == static_cast<uid_t>(-1) ? -1 : static_cast<int64_t>(uid);
        const int64_t group = gid == static_cast<gid_t>(-1) ? -1 : static_cast<int64_t>(gid);
        return Result(vexfs_mount_chown(state->session, JsonInteger(json, "inode"),
                                       owner, group, &error));
    } catch (...) {
        return -EIO;
    }
}

const char *OpenMode(int flags) {
    switch (flags & O_ACCMODE) {
        case O_WRONLY: return "w";
        case O_RDWR: return "rw";
        default: return "r";
    }
}

int OpenPath(const char *path, fuse_file_info *info) {
    FuseState *state = State();
    if (state == nullptr) return -EIO;
    std::lock_guard<std::recursive_mutex> guard(state->mutex);
    auto handle = std::make_unique<FileHandle>();
    vexfs_mount_bytes identifier{};
    vexfs_mount_error error{};
    const std::string request = RequestId(state, "open");
    auto status = vexfs_mount_handle_open(state->session, path, OpenMode(info->flags),
                                          request.c_str(), &identifier, &error);
    if (status != VEXFS_MOUNT_OK) return -Errno(status);
    handle->id = TakeBytes(&identifier);
    handle->append = (info->flags & O_APPEND) != 0;
    if ((info->flags & O_TRUNC) != 0) {
        const std::string truncate_request = RequestId(state, "truncate-open");
        status = vexfs_mount_handle_truncate(state->session, handle->id.c_str(), 0,
                                             truncate_request.c_str(), &handle->generation,
                                             &error);
        if (status != VEXFS_MOUNT_OK) {
            vexfs_mount_bytes close_state{};
            vexfs_mount_handle_close(state->session, handle->id.c_str(), 0,
                                     RequestId(state, "close-open-failure").c_str(),
                                     &close_state,
                                     &error);
            TakeBytes(&close_state);
            return -Errno(status);
        }
        handle->dirty = true;
    }
    info->fh = reinterpret_cast<uint64_t>(handle.release());
    info->direct_io = 1;
    return 0;
}

FileHandle *Handle(fuse_file_info *info) {
    return info == nullptr ? nullptr : reinterpret_cast<FileHandle *>(info->fh);
}

int Publish(FuseState *state, FileHandle *handle, const char *durability);

int CreatePath(const char *path, mode_t mode, fuse_file_info *info) {
    FuseState *state = State();
    if (state == nullptr) return -EIO;
    std::lock_guard<std::recursive_mutex> guard(state->mutex);
    auto handle = std::make_unique<FileHandle>();
    vexfs_mount_bytes identifier{};
    vexfs_mount_error error{};
    const std::string request = RequestId(state, "create-open");
    const auto status = vexfs_mount_handle_create(
        state->session, path, mode & 0777, request.c_str(), &identifier, &error);
    if (status != VEXFS_MOUNT_OK) return -Errno(status);
    handle->id = TakeBytes(&identifier);
    handle->append = (info->flags & O_APPEND) != 0;
    handle->generation = 1;
    handle->dirty = true;
    // FUSE asks getattr for the new path before open(2) returns. PostgreSQL keeps
    // handle_create as private staging until publish, so publish the POSIX-visible
    // empty file here. Later writes still use the same handle and atomic staging.
    const int publish_result = Publish(state, handle.get(), "data");
    if (publish_result != 0) {
        vexfs_mount_bytes close_state{};
        vexfs_mount_handle_close(state->session, handle->id.c_str(), 1,
                                 RequestId(state, "close-create-failure").c_str(),
                                 &close_state, &error);
        TakeBytes(&close_state);
        return publish_result;
    }
    const int chown_result = ChownCreatedPath(state, path);
    if (chown_result != 0) {
        vexfs_mount_bytes close_state{};
        vexfs_mount_handle_close(state->session, handle->id.c_str(), 0,
                                 RequestId(state, "close-chown-failure").c_str(),
                                 &close_state, &error);
        TakeBytes(&close_state);
        return chown_result;
    }
    info->fh = reinterpret_cast<uint64_t>(handle.release());
    info->direct_io = 1;
    return 0;
}

int ReadPath(const char *path, char *buffer, size_t size, off_t offset,
             fuse_file_info *info) {
    if (offset < 0) return -EINVAL;
    FuseState *state = State();
    if (state == nullptr) return -EIO;
    std::lock_guard<std::recursive_mutex> guard(state->mutex);
    vexfs_mount_bytes content{};
    vexfs_mount_error error{};
    vexfs_mount_status status;
    FileHandle *handle = Handle(info);
    if (handle != nullptr) {
        status = vexfs_mount_handle_read(state->session, handle->id.c_str(),
                                         static_cast<uint64_t>(offset), size, &content, &error);
    } else {
        status = vexfs_mount_read_file(state->session, path, &content, &error);
    }
    if (status != VEXFS_MOUNT_OK) return -Errno(status);
    std::string value = TakeBytes(&content);
    if (handle == nullptr) {
        if (static_cast<uint64_t>(offset) >= value.size()) return 0;
        value = value.substr(static_cast<size_t>(offset), size);
    }
    const size_t count = std::min(size, value.size());
    std::memcpy(buffer, value.data(), count);
    return static_cast<int>(count);
}

int WritePath(const char *, const char *buffer, size_t size, off_t offset,
              fuse_file_info *info) {
    if (offset < 0 || Handle(info) == nullptr) return -EINVAL;
    FuseState *state = State();
    if (state == nullptr) return -EIO;
    std::lock_guard<std::recursive_mutex> guard(state->mutex);
    FileHandle *handle = Handle(info);
    vexfs_mount_error error{};
    const std::string request = RequestId(state, "write");
    if (handle->append) {
        int64_t version = 0;
        const auto status = vexfs_mount_handle_append(
            state->session, handle->id.c_str(), buffer, size, request.c_str(), &version, &error);
        return status == VEXFS_MOUNT_OK ? static_cast<int>(size) : -Errno(status);
    }
    const auto status = vexfs_mount_handle_stage_write(
        state->session, handle->id.c_str(), static_cast<uint64_t>(offset), buffer, size,
        request.c_str(), &handle->generation, &error);
    if (status != VEXFS_MOUNT_OK) return -Errno(status);
    handle->dirty = true;
    return static_cast<int>(size);
}

int Publish(FuseState *state, FileHandle *handle, const char *durability) {
    if (handle == nullptr || !handle->dirty) return 0;
    vexfs_mount_error error{};
    int64_t version = 0;
    const std::string request = RequestId(state, "publish");
    const auto status = vexfs_mount_handle_publish(
        state->session, handle->id.c_str(), handle->generation, durability,
        request.c_str(), &version, &error);
    if (status != VEXFS_MOUNT_OK) return -Errno(status);
    handle->dirty = false;
    return 0;
}

int FlushPath(const char *, fuse_file_info *info) {
    FuseState *state = State();
    if (state == nullptr) return -EIO;
    std::lock_guard<std::recursive_mutex> guard(state->mutex);
    return Publish(state, Handle(info), "data");
}

int FsyncPath(const char *, int, fuse_file_info *info) {
    FuseState *state = State();
    if (state == nullptr) return -EIO;
    std::lock_guard<std::recursive_mutex> guard(state->mutex);
    int result = Publish(state, Handle(info), "full");
    if (result != 0) return result;
    int64_t published = 0;
    vexfs_mount_error error{};
    const std::string request = RequestId(state, "fsync");
    return Result(vexfs_mount_synchronize(state->session, request.c_str(), &published, &error));
}

int ReleasePath(const char *, fuse_file_info *info) {
    FuseState *state = State();
    FileHandle *handle = Handle(info);
    if (state == nullptr || handle == nullptr) return 0;
    std::lock_guard<std::recursive_mutex> guard(state->mutex);
    vexfs_mount_error error{};
    vexfs_mount_status status = VEXFS_MOUNT_OK;
    if (handle->dirty) {
        int64_t version = 0;
        status = vexfs_mount_handle_publish_close(
            state->session, handle->id.c_str(), handle->generation, "data", &version, &error);
        if (status != VEXFS_MOUNT_OK) {
            vexfs_mount_bytes close_state{};
            const std::string request = RequestId(state, "close-retain");
            vexfs_mount_handle_close(state->session, handle->id.c_str(), 1,
                                     request.c_str(), &close_state, &error);
            TakeBytes(&close_state);
        }
    } else {
        vexfs_mount_bytes close_state{};
        const std::string request = RequestId(state, "close");
        status = vexfs_mount_handle_close(state->session, handle->id.c_str(), 0,
                                          request.c_str(), &close_state, &error);
        TakeBytes(&close_state);
    }
    delete handle;
    info->fh = 0;
    return Result(status);
}

int TruncatePath(const char *path, off_t size, fuse_file_info *info) {
    if (size < 0) return -EINVAL;
    FuseState *state = State();
    if (state == nullptr) return -EIO;
    std::lock_guard<std::recursive_mutex> guard(state->mutex);
    bool temporary = Handle(info) == nullptr;
    fuse_file_info local{};
    if (temporary) {
        local.flags = O_RDWR;
        const int open_result = OpenPath(path, &local);
        if (open_result != 0) return open_result;
        info = &local;
    }
    FileHandle *handle = Handle(info);
    vexfs_mount_error error{};
    const std::string request = RequestId(state, "truncate");
    const auto status = vexfs_mount_handle_truncate(
        state->session, handle->id.c_str(), static_cast<uint64_t>(size), request.c_str(),
        &handle->generation, &error);
    int result = Result(status);
    if (result == 0) {
        handle->dirty = true;
        result = Publish(state, handle, "full");
    }
    if (temporary) {
        const int close_result = ReleasePath(path, info);
        if (result == 0) result = close_result;
    }
    return result;
}

int ReadDirectory(const char *path, void *buffer, fuse_fill_dir_t filler, off_t,
                  fuse_file_info *, fuse_readdir_flags) {
    FuseState *state = State();
    if (state == nullptr) return -EIO;
    std::lock_guard<std::recursive_mutex> guard(state->mutex);
    vexfs_mount_bytes listing{};
    vexfs_mount_error error{};
    const auto status = vexfs_mount_list(state->session, path, &listing, &error);
    if (status != VEXFS_MOUNT_OK) return -Errno(status);
    if (filler(buffer, ".", nullptr, 0, static_cast<fuse_fill_dir_flags>(0)) != 0 ||
        filler(buffer, "..", nullptr, 0, static_cast<fuse_fill_dir_flags>(0)) != 0) {
        TakeBytes(&listing);
        return 0;
    }
    try {
        for (const std::string &entry : JsonObjects(TakeBytes(&listing))) {
            struct stat attributes{};
            const std::string kind = JsonString(entry, "kind");
            if (kind == "directory") attributes.st_mode = S_IFDIR;
            else if (kind == "symlink") attributes.st_mode = S_IFLNK;
            else attributes.st_mode = S_IFREG;
            attributes.st_ino = static_cast<ino_t>(JsonInteger(entry, "inode"));
            const std::string name = JsonString(entry, "name");
            if (filler(buffer, name.c_str(), &attributes, 0,
                       static_cast<fuse_fill_dir_flags>(0)) != 0) break;
        }
        return 0;
    } catch (...) {
        return -EIO;
    }
}

int StatFs(const char *, struct statvfs *output) {
    FuseState *state = State();
    if (state == nullptr) return -EIO;
    if (state->backend == VEXFS_RUNTIME_BACKEND_SQLITE) {
        const std::filesystem::path database(state->connection);
        const std::filesystem::path directory = database.has_parent_path() ?
            database.parent_path() : std::filesystem::current_path();
        return statvfs(directory.c_str(), output) == 0 ? 0 : -errno;
    }

    std::lock_guard<std::recursive_mutex> guard(state->mutex);
    vexfs_mount_bytes quota{};
    vexfs_mount_error error{};
    const auto status = vexfs_mount_quota_get(state->session, &quota, &error);
    if (status != VEXFS_MOUNT_OK) return -Errno(status);
    try {
        const std::string json = TakeBytes(&quota);
        constexpr uint64_t block_size = 4096;
        constexpr uint64_t unlimited_bytes = 1ULL << 50;  // 1 PiB virtual ceiling.
        constexpr uint64_t unlimited_files = 1ULL << 30;
        const uint64_t live_bytes = static_cast<uint64_t>(
            std::max<int64_t>(0, JsonInteger(json, "live_bytes")));
        const uint64_t live_files = static_cast<uint64_t>(
            std::max<int64_t>(0, JsonInteger(json, "live_files")));
        const int64_t configured_bytes = JsonOptionalInteger(json, "max_bytes", -1);
        const int64_t configured_files = JsonOptionalInteger(json, "max_files", -1);
        const uint64_t capacity_bytes = configured_bytes < 0
            ? std::max(unlimited_bytes, live_bytes)
            : std::max(static_cast<uint64_t>(configured_bytes), live_bytes);
        const uint64_t capacity_files = configured_files < 0
            ? std::max(unlimited_files, live_files)
            : std::max(static_cast<uint64_t>(configured_files), live_files);
        std::memset(output, 0, sizeof(*output));
        output->f_bsize = block_size;
        output->f_frsize = block_size;
        output->f_blocks = (capacity_bytes + block_size - 1) / block_size;
        const uint64_t used_blocks = (live_bytes + block_size - 1) / block_size;
        output->f_bfree = output->f_bavail = output->f_blocks > used_blocks
            ? output->f_blocks - used_blocks : 0;
        output->f_files = capacity_files;
        output->f_ffree = output->f_favail = capacity_files > live_files
            ? capacity_files - live_files : 0;
        output->f_namemax = 255;
        return 0;
    } catch (...) {
        TakeBytes(&quota);
        return -EIO;
    }
}

int PathInode(FuseState *state, const char *path, int64_t *inode) {
    try {
        std::string json;
        const int result = StatPath(state, path, &json);
        if (result != 0) return result;
        *inode = JsonInteger(json, "inode");
        return 0;
    } catch (...) {
        return -EIO;
    }
}

int SetXattr(const char *path, const char *name, const char *value, size_t size, int flags) {
    FuseState *state = State();
    if (state == nullptr) return -EIO;
    std::lock_guard<std::recursive_mutex> guard(state->mutex);
    int64_t inode = 0;
    const int stat_result = PathInode(state, path, &inode);
    if (stat_result != 0) return stat_result;
    int policy = VEXFS_MOUNT_XATTR_ALWAYS_SET;
    if ((flags & XATTR_CREATE) != 0) policy = VEXFS_MOUNT_XATTR_MUST_CREATE;
    else if ((flags & XATTR_REPLACE) != 0) policy = VEXFS_MOUNT_XATTR_MUST_REPLACE;
    vexfs_mount_error error{};
    return Result(vexfs_mount_xattr_set(state->session, inode, name, value, size,
                                        policy, &error));
}

int GetXattr(const char *path, const char *name, char *value, size_t size) {
    FuseState *state = State();
    if (state == nullptr) return -EIO;
    std::lock_guard<std::recursive_mutex> guard(state->mutex);
    int64_t inode = 0;
    const int stat_result = PathInode(state, path, &inode);
    if (stat_result != 0) return stat_result;
    vexfs_mount_bytes bytes{};
    vexfs_mount_error error{};
    const auto status = vexfs_mount_xattr_get(state->session, inode, name, &bytes, &error);
    if (status != VEXFS_MOUNT_OK) return -Errno(status);
    const std::string result = TakeBytes(&bytes);
    if (size == 0) return static_cast<int>(result.size());
    if (size < result.size()) return -ERANGE;
    std::memcpy(value, result.data(), result.size());
    return static_cast<int>(result.size());
}

int ListXattr(const char *path, char *list, size_t size) {
    FuseState *state = State();
    if (state == nullptr) return -EIO;
    std::lock_guard<std::recursive_mutex> guard(state->mutex);
    int64_t inode = 0;
    const int stat_result = PathInode(state, path, &inode);
    if (stat_result != 0) return stat_result;
    vexfs_mount_bytes json{};
    vexfs_mount_error error{};
    const auto status = vexfs_mount_xattr_list(state->session, inode, &json, &error);
    if (status != VEXFS_MOUNT_OK) return -Errno(status);
    std::string packed;
    for (const std::string &name : JsonStrings(TakeBytes(&json))) {
        packed.append(name);
        packed.push_back('\0');
    }
    if (size == 0) return static_cast<int>(packed.size());
    if (size < packed.size()) return -ERANGE;
    std::memcpy(list, packed.data(), packed.size());
    return static_cast<int>(packed.size());
}

int RemoveXattr(const char *path, const char *name) {
    FuseState *state = State();
    if (state == nullptr) return -EIO;
    std::lock_guard<std::recursive_mutex> guard(state->mutex);
    int64_t inode = 0;
    const int stat_result = PathInode(state, path, &inode);
    if (stat_result != 0) return stat_result;
    vexfs_mount_error error{};
    return Result(vexfs_mount_xattr_set(state->session, inode, name, nullptr, 0,
                                        VEXFS_MOUNT_XATTR_DELETE, &error));
}

int SyncDirectory(const char *, int, fuse_file_info *) {
    FuseState *state = State();
    if (state == nullptr) return -EIO;
    std::lock_guard<std::recursive_mutex> guard(state->mutex);
    int64_t published = 0;
    vexfs_mount_error error{};
    const std::string request = RequestId(state, "fsyncdir");
    return Result(vexfs_mount_synchronize(state->session, request.c_str(), &published, &error));
}

int SetTimes(const char *path, const struct timespec times[2], fuse_file_info *) {
    FuseState *state = State();
    if (state == nullptr || times == nullptr) return -EIO;
    std::lock_guard<std::recursive_mutex> guard(state->mutex);
    try {
        std::string json;
        const int stat_result = StatPath(state, path, &json);
        if (stat_result != 0) return stat_result;
        const int64_t inode = JsonInteger(json, "inode");
        int64_t accessed_at = JsonInteger(json, "accessed_at");
        int64_t modified_at = JsonInteger(json, "updated_at");
        uint32_t mask = 0;
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        auto convert = [&](const struct timespec &value, int64_t *target,
                           uint32_t field) -> int {
            if (value.tv_nsec == UTIME_OMIT) return 0;
            if (value.tv_nsec == UTIME_NOW) {
                *target = now;
            } else {
                if (value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1000000000L)
                    return -EINVAL;
                *target = static_cast<int64_t>(value.tv_sec) * 1000 + value.tv_nsec / 1000000;
            }
            mask |= field;
            return 0;
        };
        int result = convert(times[0], &accessed_at, VEXFS_MOUNT_TIME_ACCESS);
        if (result != 0) return result;
        result = convert(times[1], &modified_at, VEXFS_MOUNT_TIME_MODIFY);
        if (result != 0) return result;
        if (mask == 0) return 0;
        vexfs_mount_error error{};
        return Result(vexfs_mount_set_times(state->session, inode, accessed_at, modified_at,
                                            mask, &error));
    } catch (...) {
        return -EIO;
    }
}

void *Initialize(fuse_conn_info *, fuse_config *config) {
    config->kernel_cache = 0;
    config->use_ino = 1;
    config->entry_timeout = 0;
    config->attr_timeout = 0;
    config->negative_timeout = 0;
    return State();
}

fuse_operations Operations() {
    fuse_operations operations{};
    operations.getattr = GetAttr;
    operations.readlink = ReadLink;
    operations.mknod = MakeNode;
    operations.mkdir = MakeDirectory;
    operations.unlink = RemovePath;
    operations.rmdir = RemovePath;
    operations.symlink = CreateSymlink;
    operations.rename = RenamePath;
    operations.link = LinkPath;
    operations.chmod = ChmodPath;
    operations.chown = ChownPath;
    operations.utimens = SetTimes;
    operations.truncate = TruncatePath;
    operations.open = OpenPath;
    operations.read = ReadPath;
    operations.write = WritePath;
    operations.statfs = StatFs;
    operations.flush = FlushPath;
    operations.release = ReleasePath;
    operations.fsync = FsyncPath;
    operations.setxattr = SetXattr;
    operations.getxattr = GetXattr;
    operations.listxattr = ListXattr;
    operations.removexattr = RemoveXattr;
    operations.readdir = ReadDirectory;
    operations.fsyncdir = SyncDirectory;
    operations.create = CreatePath;
    operations.init = Initialize;
    return operations;
}

struct Options {
    std::string backend = VEXFS_RUNTIME_BACKEND_SQLITE;
    std::string connection;
    std::string workspace = "default";
    std::string mount_point;
    bool foreground = false;
    bool self_test = false;
};

Options ParseOptions(int argc, char **argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--backend" && index + 1 < argc) {
            options.backend = argv[++index];
            if (options.backend == "pg" || options.backend == "postgres")
                options.backend = VEXFS_RUNTIME_BACKEND_POSTGRESQL;
        }
        else if (argument == "--db" && index + 1 < argc) options.connection = argv[++index];
        else if (argument == "--dsn" && index + 1 < argc) options.connection = argv[++index];
        else if (argument == "--workspace" && index + 1 < argc) options.workspace = argv[++index];
        else if (argument == "--foreground" || argument == "-f") options.foreground = true;
        else if (argument == "--self-test") options.self_test = true;
        else if (!argument.empty() && argument.front() == '-')
            throw std::runtime_error("unknown option: " + argument);
        else if (options.mount_point.empty()) options.mount_point = argument;
        else throw std::runtime_error("only one mount point is allowed");
    }
    if (options.backend != VEXFS_RUNTIME_BACKEND_SQLITE &&
        options.backend != VEXFS_RUNTIME_BACKEND_POSTGRESQL)
        throw std::runtime_error("--backend must be sqlite or postgresql");
    if (options.connection.empty())
        throw std::runtime_error(options.backend == VEXFS_RUNTIME_BACKEND_POSTGRESQL
            ? "--dsn DSN is required" : "--db DATABASE is required");
    if (!options.self_test && options.mount_point.empty())
        throw std::runtime_error("MOUNT_POINT is required");
    return options;
}

int SelfTest(FuseState *state) {
    vexfs_mount_error error{};
    const char *path = "/.vexfs-fuse-self-test";
    const char content[] = "linux-fuse";
    vexfs_mount_remove(state->session, path, 0, &error);
    int64_t version = 0;
    auto status = vexfs_mount_write_file(state->session, path, content, sizeof(content) - 1,
                                         &version, &error);
    if (status != VEXFS_MOUNT_OK) return -Errno(status);
    vexfs_mount_bytes value{};
    status = vexfs_mount_read_file(state->session, path, &value, &error);
    if (status != VEXFS_MOUNT_OK) return -Errno(status);
    const bool matches = TakeBytes(&value) == content;
    status = vexfs_mount_remove(state->session, path, 0, &error);
    if (status != VEXFS_MOUNT_OK) return -Errno(status);
    if (!matches) return -EIO;
    std::cout << "VEXFS FUSE SELF TEST: PASS\n";
    return 0;
}

void AdoptDefaultRootOwnership(FuseState *state) {
    std::string json;
    if (StatPath(state, "/", &json) != 0) return;
    if (JsonInteger(json, "uid") != 0 || JsonInteger(json, "gid") != 0) return;
    const uid_t uid = getuid();
    const gid_t gid = getgid();
    if (uid == 0 && gid == 0) return;
    vexfs_mount_error error{};
    const auto status = vexfs_mount_chown(state->session, JsonInteger(json, "inode"),
                                          static_cast<int64_t>(uid),
                                          static_cast<int64_t>(gid), &error);
    if (status != VEXFS_MOUNT_OK) throw std::runtime_error(error.message);
}

}  // namespace

int main(int argc, char **argv) {
    try {
        const Options options = ParseOptions(argc, argv);
        FuseState state;
        state.backend = options.backend;
        state.connection = options.connection;
        state.request_prefix = NewRequestPrefix();
        vexfs_mount_config config{};
        config.abi_version = VEXFS_RUNTIME_ABI_VERSION;
        config.backend = options.backend.c_str();
        config.connection = options.connection.c_str();
        config.workspace = options.workspace.c_str();
        config.principal = options.backend == VEXFS_RUNTIME_BACKEND_SQLITE ? "local" : nullptr;
        config.operation_timeout_ms = 5000;
        config.flags = options.self_test ? 0 : VEXFS_RUNTIME_EXCLUSIVE_GATEWAY;
        vexfs_mount_error error{};
        const auto status = vexfs_mount_session_open(&config, &state.session, &error);
        if (status != VEXFS_MOUNT_OK) throw std::runtime_error(error.message);
        AdoptDefaultRootOwnership(&state);

        int result = 0;
        if (options.self_test) {
            result = SelfTest(&state);
        } else {
            std::vector<std::string> arguments = {argv[0], "-s", "-o",
                "default_permissions,fsname=vexfs,subtype=vexfs"};
            if (options.foreground) arguments.push_back("-f");
            arguments.push_back(options.mount_point);
            std::vector<char *> values;
            for (std::string &argument : arguments) values.push_back(argument.data());
            values.push_back(nullptr);
            const fuse_operations operations = Operations();
            result = fuse_main(static_cast<int>(arguments.size()), values.data(),
                               &operations, &state);
        }
        vexfs_mount_session_close(state.session);
        return result < 0 ? 1 : result;
    } catch (const std::exception &error) {
        std::cerr << "vexfs-fuse: " << error.what() << '\n';
        return 1;
    }
}
