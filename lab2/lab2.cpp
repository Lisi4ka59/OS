#include <iostream>
#include <unordered_map>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <cstdlib>  // For posix_memalign, exit
#include <atomic>

const size_t GLOBAL_CACHE_SIZE = 4;   // Global limit on cache pages
const size_t PAGE_SIZE = 4096;        // Example page size, aligned with typical file system block size
const char *SHARED_MEMORY_NAME = "/globalCache_shm";

struct CachePage {
    char path[256];          // File path (fixed-size character array)
    off_t offset;            // Offset of this page in the file
    char *data;              // Data buffer of the page, aligned for direct I/O
    bool used;               // Used flag for clock policy
    bool dirty;              // Dirty flag to indicate if data is modified
};

struct FileDescriptor {
    int fd;                  // File descriptor for the open file
    off_t cursor;            // Current cursor position in the file
    std::string path;        // File path associated with this descriptor
};

struct SharedMemory {
    std::atomic<int> refCount;      // Reference counter for active processes
    CachePage cache[GLOBAL_CACHE_SIZE]; // Shared cache structure
};

std::unordered_map<int, FileDescriptor> fileDescriptors; // Map of descriptors to FileDescriptor info
SharedMemory *sharedMemory = nullptr;                    // Pointer to shared memory structure
size_t clockHand = 0;                                    // Global clock pointer for cache replacement

const size_t SHARED_MEMORY_SIZE = sizeof(std::atomic<int>) + sizeof(CachePage) * GLOBAL_CACHE_SIZE;

extern "C" {

// Forward declaration of manage_cache function
void manage_cache(int fd, off_t offset, const char *data);

// Initialize and attach shared memory
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

    // Initialize shared cache and reference count if this is the first process
    if (sharedMemory->refCount == 0) {
        sharedMemory->refCount = 1;
        for (auto &page: sharedMemory->cache) {
            std::memset(page.path, 0, sizeof(page.path));  // Initialize path to an empty string
            page.offset = 0;
            page.data = nullptr;    // Set data pointer to nullptr
            page.used = false;
            page.dirty = false;
        }
    } else {
        sharedMemory->refCount++;
    }
}

// Detach shared memory and remove if last process
void detach_shared_memory() {
    if (sharedMemory) {
        sharedMemory->refCount--;

        for (size_t i = 0; i < GLOBAL_CACHE_SIZE; i++) {
            if (sharedMemory->cache[i].used && sharedMemory->cache[i].dirty) {
                sharedMemory->cache[i] = {};  // Reset page for this process
            }
        }

        if (sharedMemory->refCount == 0) {
            shm_unlink(SHARED_MEMORY_NAME);
        }

        munmap(sharedMemory, SHARED_MEMORY_SIZE);
    }
}

// Cleanup function to detach shared memory on exit
void cleanup() {
    detach_shared_memory();
}

void initialize_library() {
    attach_shared_memory();
    atexit(cleanup);  // Register cleanup on process exit
}

// Library functions mimicking system calls

int lab2_open(const char *path, int flags) {
    std::string filePath(path);
    for (const auto &entry: fileDescriptors) {
        if (entry.second.path == filePath) {
            return entry.first; // Return existing fd if the file is already open
        }
    }

    int realFd = open(path, flags | O_DIRECT, 0666); // Add O_DIRECT to bypass page cache
    if (realFd != -1) {
        fileDescriptors[realFd] = {realFd, 0, filePath}; // Initialize cursor and store path
    }
    return realFd;
}

ssize_t lab2_read(int fd, void *buf, size_t count) {
    attach_shared_memory();

    if (!sharedMemory) {
        std::cerr << "Error: sharedMemory is not mapped in lab2_read." << std::endl;
        return -1;
    }


    if (fileDescriptors.find(fd) == fileDescriptors.end()) {
        return -1;
    }

    off_t &cursor = fileDescriptors[fd].cursor;
    std::string filePath = fileDescriptors[fd].path;

    struct stat st;
    if (fstat(fd, &st) == -1) {
        return -1;
    }
    off_t fileSize = st.st_size;

    size_t bytesRead = 0;

    if (count > fileSize - cursor) {
        count = fileSize - cursor;
    }

    while (bytesRead < count && cursor < fileSize) {
        off_t pageOffset = (cursor / PAGE_SIZE) * PAGE_SIZE;
        size_t pageStart = cursor % PAGE_SIZE;
        size_t bytesToRead = std::min(PAGE_SIZE - pageStart, count - bytesRead);

        if (cursor + bytesToRead > fileSize) {
            bytesToRead = fileSize - cursor;
        }

        bool inCache = false;

        for (auto &page: sharedMemory->cache) {
            // Check that page.path is non-empty before comparing

            if (std::strncmp(page.path, filePath.c_str(), sizeof(page.path)) == 0 && page.offset == pageOffset) {
                inCache = true;
                std::memcpy((char *) buf + bytesRead, page.data + pageStart, bytesToRead);
                page.used = true;
                break;
            }
        }

        if (!inCache) {
            char *diskData;
            posix_memalign((void **) &diskData, PAGE_SIZE, PAGE_SIZE);
            ssize_t ret = pread(fd, diskData, PAGE_SIZE, pageOffset);
            if (ret == -1) {
                free(diskData);
                return -1;
            }

            manage_cache(fd, pageOffset, diskData);
            std::memcpy((char *) buf + bytesRead, diskData + pageStart, bytesToRead);
            free(diskData);
        }

        bytesRead += bytesToRead;
        cursor += bytesToRead;
    }

    return bytesRead;
}

ssize_t lab2_write(int fd, const void *buf, size_t count) {
    if (fileDescriptors.find(fd) == fileDescriptors.end()) return -1;

    off_t &cursor = fileDescriptors[fd].cursor;
    std::string filePath = fileDescriptors[fd].path;

    size_t bytesWritten = 0;

    while (bytesWritten < count) {
        off_t pageOffset = cursor / PAGE_SIZE * PAGE_SIZE;
        size_t pageStart = cursor % PAGE_SIZE;
        size_t bytesToWrite = std::min(PAGE_SIZE - pageStart, count - bytesWritten);

        bool inCache = false;
        for (auto &page: sharedMemory->cache) {
            if (std::strncmp(page.path, filePath.c_str(), sizeof(page.path)) == 0 && page.offset == pageOffset) {
                inCache = true;
                std::memcpy(page.data + pageStart, (const char *) buf + bytesWritten, bytesToWrite);
                page.dirty = true;
                page.used = true;
                break;
            }
        }

        if (!inCache) {
            char *pageData;
            posix_memalign((void **) &pageData, PAGE_SIZE, PAGE_SIZE);
            std::memcpy(pageData + pageStart, (const char *) buf + bytesWritten, bytesToWrite);
            manage_cache(fd, pageOffset, pageData);
        }

        bytesWritten += bytesToWrite;
        cursor += bytesToWrite;
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

    for (auto it = sharedMemory->cache; it != sharedMemory->cache + GLOBAL_CACHE_SIZE; ++it) {
        if (std::strncmp(it->path, filePath.c_str(), sizeof(it->path)) == 0) {
            if (it->dirty) {
                pwrite(fd, it->data, PAGE_SIZE, it->offset);
            }
            free(it->data);
            *it = {};  // Reset page
        }
    }

    fileDescriptors.erase(fd);
    return close(fd);
}

void manage_cache(int fd, off_t offset, const char *data) {
    if (fileDescriptors.find(fd) == fileDescriptors.end()) {
        return;
    }

    std::string filePath = fileDescriptors[fd].path;

    if (sharedMemory->refCount >= GLOBAL_CACHE_SIZE) {
        bool foundEvictablePage = false;
        size_t startClockHand = clockHand;

        while (!foundEvictablePage) {
            if (clockHand >= GLOBAL_CACHE_SIZE) {
                clockHand = 0;
            }

            CachePage &page = sharedMemory->cache[clockHand];

            if (!page.used) {
                int cacheFd = -1;
                for (const auto &entry: fileDescriptors) {
                    if (std::strncmp(entry.second.path.c_str(), page.path, sizeof(page.path)) == 0) {
                        cacheFd = entry.first;
                        break;
                    }
                }

                if (page.dirty && cacheFd != -1) {
                    pwrite(cacheFd, page.data, PAGE_SIZE, page.offset);
                }

                free(page.data);
                std::strncpy(page.path, filePath.c_str(), sizeof(page.path) - 1);
                page.path[sizeof(page.path) - 1] = '\0'; // Null-terminate to avoid overflow
                page.offset = offset;
                posix_memalign((void **) &page.data, PAGE_SIZE, PAGE_SIZE);
                std::memcpy(page.data, data, PAGE_SIZE);
                page.used = true;
                page.dirty = false;
                foundEvictablePage = true;
            } else {
                page.used = false;
                clockHand = (clockHand + 1) % GLOBAL_CACHE_SIZE;
            }

            if (clockHand == startClockHand) {
                for (auto &p: sharedMemory->cache) {
                    p.used = false;
                }
            }
        }
    } else {
        CachePage newPage;
        std::strncpy(newPage.path, filePath.c_str(), sizeof(newPage.path) - 1);
        newPage.path[sizeof(newPage.path) - 1] = '\0'; // Null-terminate to avoid overflow
        newPage.offset = offset;
        posix_memalign((void **) &newPage.data, PAGE_SIZE, PAGE_SIZE);
        std::memcpy(newPage.data, data, PAGE_SIZE);
        newPage.used = true;
        newPage.dirty = false;
        sharedMemory->cache[clockHand] = newPage;
    }
}

} // End of extern "C"
