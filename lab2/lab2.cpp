#include <iostream>
#include <unordered_map>
#include <cstring>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <cstdlib>
#include <atomic>
#include <pthread.h>
#include <vector>
#include <unistd.h>
#include <cerrno>

constexpr size_t GLOBAL_CACHE_SIZE = 16 * 16 * 50;   // Count of cache pages (50 MB)
constexpr size_t PAGE_SIZE = 4096;                   // Size of single page (4 KB)
const char *SHARED_MEMORY_NAME = "/globalCache_shm";
constexpr int ABSOLUTE_PATH_LENGTH = 512;            // Max length of absolute path

struct CachePage {
    char path[ABSOLUTE_PATH_LENGTH];    // File path
    off_t offset;                       // Offset of this page in the file
    char data[PAGE_SIZE];               // Data buffer of the page in shared memory
    bool used;                          // Used flag for clock policy
    bool dirty;                         // Dirty flag
};

struct FileDescriptor {
    int fd;                  // File descriptor
    off_t cursor;            // Current cursor position in the file
    std::string path;        // File path
};

struct SharedMemory {
    std::atomic<int> refCount;      // Count of active processes
    std::atomic<size_t> clockHand;  // Clock hand for cache replacement
    pthread_mutex_t mutex;          // Mutex
    CachePage cache[GLOBAL_CACHE_SIZE]; // Cache page structure
};

std::unordered_map<int, FileDescriptor> fileDescriptors;
SharedMemory *sharedMemory = nullptr;

constexpr size_t SHARED_MEMORY_SIZE = sizeof(SharedMemory);

extern "C" {

bool found_file_descriptor(int fd) {
    if (fileDescriptors.find(fd) == fileDescriptors.end()) {
        std::cerr << "File descriptor not found" << std::endl;
        return false;
    }
    return true;
}

void attach_shared_memory() {
    int shm_fd = shm_open(SHARED_MEMORY_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("Failed to open shared memory");
        exit(1);
    }
    if (ftruncate(shm_fd, SHARED_MEMORY_SIZE) == -1) {
        perror("Failed to set shared memory size");
        exit(1);
    }
    void *ptr = mmap(nullptr, SHARED_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (ptr == MAP_FAILED) {
        perror("Failed to map shared memory");
        exit(1);
    }
    close(shm_fd);
    sharedMemory = static_cast<SharedMemory *>(ptr);
    if (sharedMemory->refCount.fetch_add(1) == 0) {
        sharedMemory->clockHand = 0;
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(&sharedMemory->mutex, &attr);
        pthread_mutexattr_destroy(&attr);
        for (CachePage &page: sharedMemory->cache) {
            std::memset(page.path, 0, sizeof(page.path));
            page.offset = 0;
            page.used = false;
            page.dirty = false;
        }
    }
}

void detach_shared_memory() {
    if (sharedMemory) {
        sharedMemory->refCount--;
        if (sharedMemory->refCount == 0) {
            pthread_mutex_destroy(&sharedMemory->mutex);
            shm_unlink(SHARED_MEMORY_NAME);
        }
    }
}

void initialize_library() {
    attach_shared_memory();
    atexit(detach_shared_memory);
}

void clear_cache_for_file(const std::string &filePath) {
    for (CachePage &page: sharedMemory->cache) {
        if (std::strncmp(page.path, filePath.c_str(), sizeof(page.path)) == 0) {
            std::memset(page.path, 0, sizeof(page.path));
            page.used = false;
            page.dirty = false;
        }
    }
}

int lab2_open(const char *path, int flags) {
    char absolute_path[ABSOLUTE_PATH_LENGTH];
    int realFd = open(path, flags | O_DIRECT, 0666);
    if (realFd != -1) {
        if (!realpath(path, absolute_path)) {
            perror("Failed to resolve absolute path");
            return -1;
        }
        std::string filePath(absolute_path);
        pthread_mutex_lock(&sharedMemory->mutex);
        clear_cache_for_file(filePath);
        pthread_mutex_unlock(&sharedMemory->mutex);
        fileDescriptors[realFd] = {realFd, 0, filePath};
    }
    return realFd;
}

ssize_t lab2_read(int fd, void *buf, size_t count) {
    if (!found_file_descriptor(fd)) return -1;
    FileDescriptor &fileDesc = fileDescriptors[fd];
    size_t bytes_read_total = 0;
    std::vector<std::pair<char *, size_t>> temp_chunks;
    pthread_mutex_lock(&sharedMemory->mutex);
    while (bytes_read_total < count) {
        off_t page_aligned_offset = fileDesc.cursor / PAGE_SIZE * PAGE_SIZE;
        size_t page_offset = fileDesc.cursor % PAGE_SIZE;
        size_t bytes_to_read = std::min(PAGE_SIZE - page_offset, count - bytes_read_total);
        std::cerr << "Reading page_aligned_offset: " << page_aligned_offset
                  << ", page_offset: " << page_offset
                  << ", bytes_to_read: " << bytes_to_read
                  << ", cursor: " << fileDesc.cursor << "\n";

        char *chunk;
        posix_memalign(reinterpret_cast<void **>(&chunk), PAGE_SIZE, PAGE_SIZE);
        bool page_found = false;
        for (size_t i = 0; i < GLOBAL_CACHE_SIZE; i++) {
            CachePage &page = sharedMemory->cache[i];
            if (page.used && page.offset == page_aligned_offset &&
                strcmp(page.path, fileDesc.path.c_str()) == 0) {
                std::memcpy(chunk, page.data + page_offset, bytes_to_read);
                page_found = true;
                std::cerr << "Cache hit at offset " << page_aligned_offset
                          << " for bytes to read " << bytes_to_read
                          << ", last byte in cache: " << static_cast<int>(page.data[PAGE_SIZE - 1]) << "\n";
                break;
            }
        }
        if (!page_found) {
            lseek(fd, fileDesc.cursor, SEEK_SET);
            ssize_t bytes_from_file = read(fd, chunk, bytes_to_read);
            if (bytes_from_file <= 0) {
                std::cerr << "Read error or EOF reached at offset " << fileDesc.cursor << "\n";
                free(chunk);
                break;
            }
            std::cerr << "Reading from file at offset " << fileDesc.cursor
                      << ", bytes read " << bytes_from_file << "\n";

            if (bytes_from_file < bytes_to_read) {
                std::memset(chunk + bytes_from_file, 0, bytes_to_read - bytes_from_file);
            }
            size_t start_hand = sharedMemory->clockHand;
            CachePage *page = nullptr;
            while (true) {
                CachePage &current_page = sharedMemory->cache[sharedMemory->clockHand];
                if (!current_page.used || !current_page.dirty) {
                    if (current_page.used && current_page.dirty) {
                        int file_fd = open(current_page.path, O_WRONLY | O_DIRECT);
                        if (file_fd != -1) {
                            char *aligned_buffer;
                            posix_memalign(reinterpret_cast<void **>(&aligned_buffer), PAGE_SIZE, PAGE_SIZE);
                            std::memcpy(aligned_buffer, current_page.data, PAGE_SIZE);
                            lseek(file_fd, current_page.offset, SEEK_SET);
                            write(file_fd, aligned_buffer, PAGE_SIZE);
                            close(file_fd);
                            free(aligned_buffer);
                            std::cerr << "Flushed dirty page at offset " << current_page.offset << " to file.\n";
                        }
                        current_page.dirty = false;
                    }
                    current_page.used = true;
                    current_page.dirty = false;
                    strncpy(current_page.path, fileDesc.path.c_str(), ABSOLUTE_PATH_LENGTH);
                    current_page.offset = page_aligned_offset;
                    std::memcpy(current_page.data, chunk, bytes_to_read);
                    std::cerr << "Loaded page into cache at offset " << page_aligned_offset
                              << ", with last byte in chunk: " << static_cast<int>(chunk[bytes_to_read - 1]) << "\n";
                    break;
                }
                sharedMemory->clockHand = (sharedMemory->clockHand + 1) % GLOBAL_CACHE_SIZE;
                if (sharedMemory->clockHand == start_hand) {
                    CachePage &forced_page = sharedMemory->cache[sharedMemory->clockHand];
                    forced_page.used = true;
                    forced_page.dirty = false;
                    strncpy(forced_page.path, fileDesc.path.c_str(), ABSOLUTE_PATH_LENGTH);
                    forced_page.offset = page_aligned_offset;
                    std::memcpy(forced_page.data, chunk, bytes_to_read);
                    std::cerr << "Evicted and loaded new page into cache at offset " << page_aligned_offset
                              << ", with last byte in chunk: " << static_cast<int>(chunk[bytes_to_read - 1]) << "\n";
                    break;
                }
            }
        }
        temp_chunks.push_back({chunk, bytes_to_read});
        bytes_read_total += bytes_to_read;
        fileDesc.cursor += bytes_to_read;
        std::cerr << "Total bytes read so far: " << bytes_read_total
                  << ", updated cursor: " << fileDesc.cursor << "\n";
    }
    pthread_mutex_unlock(&sharedMemory->mutex);
    size_t assembled_offset = 0;
    for (auto &[temp_chunk, chunk_size]: temp_chunks) {
        std::memcpy(static_cast<char *>(buf) + assembled_offset, temp_chunk, chunk_size);
        assembled_offset += chunk_size;
        free(temp_chunk);
    }
    std::cerr << "Completed read with total bytes read: " << bytes_read_total
              << ", last byte in buffer: " << static_cast<int>(static_cast<char *>(buf)[bytes_read_total - 1]) << "\n";
    return bytes_read_total;
}


ssize_t lab2_write(int fd, const char *buffer, size_t size) {
    if (!found_file_descriptor(fd)) return -1;
    FileDescriptor &fileDesc = fileDescriptors[fd];
    size_t bytes_written = 0;
    pthread_mutex_lock(&sharedMemory->mutex);
    while (bytes_written < size) {
        off_t offset = fileDesc.cursor / PAGE_SIZE * PAGE_SIZE;
        size_t page_offset = fileDesc.cursor % PAGE_SIZE;
        size_t bytes_to_write = std::min(PAGE_SIZE - page_offset, size - bytes_written);
        bool page_found = false;
        for (size_t i = 0; i < GLOBAL_CACHE_SIZE; i++) {
            CachePage &page = sharedMemory->cache[i];
            if (page.used && page.offset == offset && strcmp(page.path, fileDesc.path.c_str()) == 0) {
                memcpy(page.data + page_offset, buffer + bytes_written, bytes_to_write);
                page.dirty = true;
                page_found = true;
                break;
            }
        }
        if (!page_found) {
            size_t start_hand = sharedMemory->clockHand;
            while (true) {
                CachePage &page = sharedMemory->cache[sharedMemory->clockHand];
                if (!page.used || !page.dirty) {
                    if (page.used && page.dirty) {
                        int file_fd = open(page.path, O_WRONLY | O_DIRECT);
                        if (file_fd != -1) {
                            lseek(file_fd, page.offset, SEEK_SET);
                            write(file_fd, page.data, PAGE_SIZE);
                            close(file_fd);
                        }
                    }
                    page.used = true;
                    page.dirty = true;
                    strncpy(page.path, fileDesc.path.c_str(), ABSOLUTE_PATH_LENGTH);
                    page.offset = offset;
                    memcpy(page.data, buffer + bytes_written, bytes_to_write);
                    break;
                }
                sharedMemory->clockHand = (sharedMemory->clockHand + 1) % GLOBAL_CACHE_SIZE;
                if (sharedMemory->clockHand == start_hand) {
                    CachePage &forced_page = sharedMemory->cache[sharedMemory->clockHand];
                    if (forced_page.dirty) {
                        int file_fd = open(forced_page.path, O_WRONLY);
                        if (file_fd != -1) {
                            lseek(file_fd, forced_page.offset, SEEK_SET);
                            write(file_fd, forced_page.data, PAGE_SIZE);
                            close(file_fd);
                        }
                    }
                    forced_page.used = true;
                    forced_page.dirty = true;
                    strncpy(forced_page.path, fileDesc.path.c_str(), ABSOLUTE_PATH_LENGTH);
                    forced_page.offset = offset;
                    memcpy(forced_page.data, buffer + bytes_written, bytes_to_write);
                    break;
                }
            }
        }
        fileDesc.cursor += bytes_to_write;
        bytes_written += bytes_to_write;
    }
    pthread_mutex_unlock(&sharedMemory->mutex);
    return bytes_written;
}


off_t lab2_lseek(int fd, off_t offset, int whence) {
    if (!found_file_descriptor(fd)) {
        return -1;
    }
    off_t &cursor = fileDescriptors[fd].cursor;
    switch (whence) {
        case SEEK_SET:
            cursor = offset;
            break;
        case SEEK_CUR:
            cursor += offset;
            break;
        case SEEK_END: {
            off_t fileSize = lseek(fd, 0, SEEK_END);
            if (fileSize == -1) return -1;
            cursor = fileSize + offset;
            break;
        }
        default:
            return -1;
    }
    return cursor;
}

int lab2_close(int fd) {
    if (!found_file_descriptor(fd)) {
        return -1;
    }
    std::string filePath = fileDescriptors[fd].path;
    pthread_mutex_lock(&sharedMemory->mutex);
    for (CachePage* it = sharedMemory->cache; it != sharedMemory->cache + GLOBAL_CACHE_SIZE; ++it) {
        if (std::strncmp(it->path, filePath.c_str(), sizeof(it->path)) == 0) {
            if (it->dirty) {
                pwrite(fd, it->data, PAGE_SIZE, it->offset);
                it->dirty = false;
            }
            std::memset(it->path, 0, sizeof(it->path));
            it->used = false;
        }
    }
    pthread_mutex_unlock(&sharedMemory->mutex);
    fileDescriptors.erase(fd);
    fsync(fd);
    return close(fd);
}

int lab2_fsync(int fd) {
    if (!found_file_descriptor(fd)) return -1;
    FileDescriptor &fileDesc = fileDescriptors[fd];
    pthread_mutex_lock(&sharedMemory->mutex);
    for (size_t i = 0; i < GLOBAL_CACHE_SIZE; i++) {
        CachePage &page = sharedMemory->cache[i];
        if (page.dirty && strcmp(page.path, fileDesc.path.c_str()) == 0) {
            int file_fd = open(page.path, O_WRONLY);
            if (file_fd == -1) {
                std::cerr << "Error: failed to open file for syncing at offset " << page.offset << std::endl;
                pthread_mutex_unlock(&sharedMemory->mutex);
                return -1;
            }
            if (lseek(file_fd, page.offset, SEEK_SET) == -1 ||
                write(file_fd, page.data, PAGE_SIZE) != PAGE_SIZE) {
                std::cerr << "Error: failed to write dirty page to disk at offset " << page.offset << std::endl;
                close(file_fd);
                pthread_mutex_unlock(&sharedMemory->mutex);
                return -1;
            }
            close(file_fd);
            page.dirty = false;
        }
    }
    pthread_mutex_unlock(&sharedMemory->mutex);
    return 0;
}
}
