#include <iostream>
#include <unordered_map>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <cstdlib>
#include <atomic>
#include <pthread.h>

const size_t GLOBAL_CACHE_SIZE = 16 * 16 * 50;   // Count of cache pages (50 MB)
const size_t PAGE_SIZE = 4096;                   // Size of single page (4 KB)
const char* SHARED_MEMORY_NAME = "/globalCache_shm";

struct CachePage {
    char path[256];          // File path
    off_t offset;            // Offset of this page in the file
    char data[PAGE_SIZE];    // Data buffer of the page in shared memory
    bool used;               // Used flag for clock policy
    bool dirty;              // Dirty flag
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
SharedMemory* sharedMemory = nullptr;

const size_t SHARED_MEMORY_SIZE = sizeof(std::atomic<int>) + sizeof(std::atomic<size_t>) + sizeof(pthread_mutex_t) + sizeof(CachePage) * GLOBAL_CACHE_SIZE;

extern "C" {

void manage_cache(int fd, off_t offset, const char *data);

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

    void* ptr = mmap(nullptr, SHARED_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (ptr == MAP_FAILED) {
        perror("Failed to map shared memory");
        exit(1);
    }
    close(shm_fd);

    sharedMemory = static_cast<SharedMemory*>(ptr);

    if (sharedMemory->refCount == 0) {
        sharedMemory->refCount = 1;
        sharedMemory->clockHand = 0;

        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(&sharedMemory->mutex, &attr);
        pthread_mutexattr_destroy(&attr);

        for (auto &page : sharedMemory->cache) {
            std::memset(page.path, 0, sizeof(page.path));
            page.offset = 0;
            page.used = false;
            page.dirty = false;
        }
    } else {
        sharedMemory->refCount++;
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

int lab2_open(const char *path, int flags) {
    std::string filePath(path);
    pthread_mutex_lock(&sharedMemory->mutex);
    for (auto &page : sharedMemory->cache) {
        if (std::strncmp(page.path, filePath.c_str(), sizeof(page.path)) == 0) {
            std::memset(page.path, 0, sizeof(page.path));
            page.used = false;
            page.dirty = false;
        }
    }
    pthread_mutex_unlock(&sharedMemory->mutex);

    int realFd = open(path, flags | O_DIRECT, 0666);
    if (realFd != -1) {
        fileDescriptors[realFd] = {realFd, 0, filePath};
    }
    return realFd;
}

ssize_t lab2_read(int fd, void *buf, size_t count) {
    if (fileDescriptors.find(fd) == fileDescriptors.end()) {
        std::cerr << "File descriptor not found in lab2_read" << std::endl;
        return -1;
    }

    off_t &cursor = fileDescriptors[fd].cursor;
    size_t bytesRead = 0;
    char *buf_ptr = static_cast<char *>(buf);

    while (bytesRead < count) {
        size_t bytesToRead = count - bytesRead;
        size_t current_page_offset = (cursor / PAGE_SIZE) * PAGE_SIZE;
        size_t pageStartOffset = cursor % PAGE_SIZE;

        bool inCache = false;
        CachePage *page_ptr = nullptr;

        pthread_mutex_lock(&sharedMemory->mutex);
        for (auto &page : sharedMemory->cache) {
            if (page.offset == current_page_offset) {
                page_ptr = &page;
                inCache = true;
                page.used = true;
                break;
            }
        }
        pthread_mutex_unlock(&sharedMemory->mutex);

        if (!inCache) {
            void *aligned_buf;
            if (posix_memalign(&aligned_buf, 4096, PAGE_SIZE) != 0) {
                std::cerr << "Failed to allocate aligned memory in lab2_read" << std::endl;
                return -1;
            }

            ssize_t result = pread(fd, aligned_buf, PAGE_SIZE, current_page_offset);
            if (result <= 0) {
                free(aligned_buf);
                return bytesRead > 0 ? bytesRead : result;
            }

            pthread_mutex_lock(&sharedMemory->mutex);
            for (auto &page : sharedMemory->cache) {
                if (!page.used) {
                    std::memcpy(page.data, aligned_buf, PAGE_SIZE);
                    page.offset = current_page_offset;
                    page.used = true;
                    page.dirty = false;
                    page_ptr = &page;
                    break;
                }
            }
            pthread_mutex_unlock(&sharedMemory->mutex);

            free(aligned_buf);
        }

        if (page_ptr) {
            size_t bytesFromCache = std::min(PAGE_SIZE - pageStartOffset, bytesToRead);
            std::memcpy(buf_ptr + bytesRead, page_ptr->data + pageStartOffset, bytesFromCache);
            cursor += bytesFromCache;
            bytesRead += bytesFromCache;
        }
    }
    return bytesRead;
}


ssize_t lab2_write(int fd, const void *buf, size_t count) {
    if (fileDescriptors.find(fd) == fileDescriptors.end()) {
        std::cerr << "File descriptor not found in lab2_write" << std::endl;
        return -1;
    }

    off_t &cursor = fileDescriptors[fd].cursor;
    size_t bytesWritten = 0;
    const char *buf_ptr = static_cast<const char *>(buf);

    while (bytesWritten < count) {
        size_t bytesToWrite = count - bytesWritten;
        size_t current_page_offset = (cursor / PAGE_SIZE) * PAGE_SIZE;
        size_t pageStartOffset = cursor % PAGE_SIZE;

        bool inCache = false;
        CachePage *page_ptr = nullptr;

        pthread_mutex_lock(&sharedMemory->mutex);
        for (auto &page : sharedMemory->cache) {
            if (page.offset == current_page_offset) {
                page_ptr = &page;
                inCache = true;
                page.used = true;
                break;
            }
        }
        pthread_mutex_unlock(&sharedMemory->mutex);

        if (!inCache) {
            void *aligned_buf;
            if (posix_memalign(&aligned_buf, 4096, PAGE_SIZE) != 0) {
                std::cerr << "Failed to allocate aligned memory in lab2_write" << std::endl;
                return -1;
            }

            ssize_t result = pread(fd, aligned_buf, PAGE_SIZE, current_page_offset);
            if (result < 0) {
                free(aligned_buf);
                return -1;
            }

            pthread_mutex_lock(&sharedMemory->mutex);
            for (auto &page : sharedMemory->cache) {
                if (!page.used) {
                    std::memcpy(page.data, aligned_buf, PAGE_SIZE);
                    page.offset = current_page_offset;
                    page.used = true;
                    page.dirty = false;
                    page_ptr = &page;
                    break;
                }
            }
            pthread_mutex_unlock(&sharedMemory->mutex);

            free(aligned_buf);
        }

        if (page_ptr) {
            size_t bytesToCache = std::min(PAGE_SIZE - pageStartOffset, bytesToWrite);
            std::memcpy(page_ptr->data + pageStartOffset, buf_ptr + bytesWritten, bytesToCache);
            page_ptr->dirty = true;
            page_ptr->used = true;
            cursor += bytesToCache;
            bytesWritten += bytesToCache;
        }
    }
    return bytesWritten;
}

off_t lab2_lseek(int fd, off_t offset, int whence) {
    if (fileDescriptors.find(fd) == fileDescriptors.end()) return -1;

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
    if (fileDescriptors.find(fd) == fileDescriptors.end()) return -1;

    std::string filePath = fileDescriptors[fd].path;

    pthread_mutex_lock(&sharedMemory->mutex);
    for (auto it = sharedMemory->cache; it != sharedMemory->cache + GLOBAL_CACHE_SIZE; ++it) {
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

void manage_cache(int fd, off_t offset, const char *data) {
    if (fileDescriptors.find(fd) == fileDescriptors.end()) {
        return;
    }

    std::string filePath = fileDescriptors[fd].path;

    bool foundEvictablePage = false;
    size_t startClockHand = sharedMemory->clockHand;

    pthread_mutex_lock(&sharedMemory->mutex);
    while (!foundEvictablePage) {
        if (sharedMemory->clockHand >= GLOBAL_CACHE_SIZE) {
            sharedMemory->clockHand = 0;
        }

        CachePage &page = sharedMemory->cache[sharedMemory->clockHand];

        if (!page.used) {
            int cacheFd = -1;
            for (const auto &entry : fileDescriptors) {
                if (std::strncmp(entry.second.path.c_str(), page.path, sizeof(page.path)) == 0) {
                    cacheFd = entry.first;
                    break;
                }
            }

            if (page.dirty && cacheFd != -1) {
                pwrite(cacheFd, page.data, PAGE_SIZE, page.offset);
            }

            std::strncpy(page.path, filePath.c_str(), sizeof(page.path) - 1);
            page.path[sizeof(page.path) - 1] = '\0'; // Null-terminate to avoid overflow
            page.offset = offset;
            std::memcpy(page.data, data, PAGE_SIZE);
            page.used = true;
            page.dirty = false;
            foundEvictablePage = true;
        } else {
            page.used = false;
            sharedMemory->clockHand = (sharedMemory->clockHand + 1) % GLOBAL_CACHE_SIZE;
        }

        if (sharedMemory->clockHand == startClockHand) {
            for (auto &p : sharedMemory->cache) {
                p.used = false;
            }
        }
    }
    pthread_mutex_unlock(&sharedMemory->mutex);
}
int lab2_fsync(int fd) {
    if (fileDescriptors.find(fd) == fileDescriptors.end()) {
        std::cerr << "Error: file descriptor not found.\n";
        return -1;
    }
    std::string filePath = fileDescriptors[fd].path;

    pthread_mutex_lock(&sharedMemory->mutex);

    for (size_t i = 0; i < GLOBAL_CACHE_SIZE; ++i) {
        CachePage& page = sharedMemory->cache[i];

        if (page.used && page.dirty && filePath == page.path) {
            ssize_t bytes_written = pwrite(fd, page.data, PAGE_SIZE, page.offset);
            if (bytes_written < 0) {
                std::cerr << "Error: failed to write dirty page to disk at offset " << page.offset << ".\n";
                pthread_mutex_unlock(&sharedMemory->mutex);
                return -1;
            }

            page.dirty = false;
        }
    }

    pthread_mutex_unlock(&sharedMemory->mutex);

    if (fsync(fd) < 0) {
        std::cerr << "Error: fsync failed on file descriptor.\n";
        return -1;
    }

    return 0;
}
}
