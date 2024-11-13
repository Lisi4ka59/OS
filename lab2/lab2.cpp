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
SharedMemory *sharedMemory = nullptr;

constexpr size_t SHARED_MEMORY_SIZE = sizeof(std::atomic<int>) + sizeof(std::atomic<size_t>) + sizeof(pthread_mutex_t) +
                                      sizeof(CachePage) * GLOBAL_CACHE_SIZE;

extern "C" {

void manage_cache(int fd, off_t offset, const char *data);

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

        for (auto &page: sharedMemory->cache) {
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

int lab2_open(const char *path, int flags) {
    std::string filePath(path);
    pthread_mutex_lock(&sharedMemory->mutex);
    for (auto &page: sharedMemory->cache) {
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
    if (!found_file_descriptor(fd)) return -1;

    FileDescriptor &fileDesc = fileDescriptors[fd];
    size_t bytes_read_total = 0;
    std::vector<std::pair<char*, size_t>> temp_chunks;

    pthread_mutex_lock(&sharedMemory->mutex);

    while (bytes_read_total < count) {
        off_t page_aligned_offset = fileDesc.cursor / PAGE_SIZE * PAGE_SIZE;
        size_t page_offset = fileDesc.cursor % PAGE_SIZE;
        size_t bytes_to_read = std::min(PAGE_SIZE - page_offset, count - bytes_read_total);

        char* chunk;
        posix_memalign(reinterpret_cast<void**>(&chunk), PAGE_SIZE, PAGE_SIZE);

        bool page_found = false;
        for (size_t i = 0; i < GLOBAL_CACHE_SIZE; i++) {
            CachePage &page = sharedMemory->cache[i];
            if (page.used && page.offset == page_aligned_offset &&
                strcmp(page.path, fileDesc.path.c_str()) == 0) {
                std::memcpy(chunk, page.data + page_offset, bytes_to_read);
                page_found = true;
                break;
            }
        }

        if (!page_found) {
            lseek(fd, fileDesc.cursor, SEEK_SET);
            ssize_t bytes_from_file = read(fd, chunk, bytes_to_read);


            if (bytes_from_file <= 0) {
                // std::cerr << "Read error or end of file reached; exiting read loop.\n"<<" bytes_read_total "<<bytes_read_total<<"\n";
                // std::cerr << "fileDesc.cursor " <<fileDesc.cursor<<"\n";
                // std::cerr << "fd " <<fd<<"\n";
                // std::cerr << "Error: " << strerror(errno) << " (" << errno << ")\n";
                delete[] chunk;
                break;
            }

            if (bytes_from_file < bytes_to_read) {
                std::memset(chunk + bytes_from_file, 0, bytes_to_read - bytes_from_file);
            }

            size_t start_hand = sharedMemory->clockHand;
            CachePage *page = nullptr;

            while (true) {
                CachePage &current_page = sharedMemory->cache[sharedMemory->clockHand];
                if (!current_page.used || !current_page.dirty) {
                    if (current_page.used && current_page.dirty) {
                        int file_fd = open(current_page.path, O_WRONLY);
                        if (file_fd != -1) {
                            lseek(file_fd, current_page.offset, SEEK_SET);
                            write(file_fd, current_page.data, PAGE_SIZE);
                            close(file_fd);
                        }
                    }
                    current_page.used = true;
                    current_page.dirty = false;
                    strcpy(current_page.path, fileDesc.path.c_str());
                    current_page.offset = page_aligned_offset;

                    std::memcpy(current_page.data, chunk, bytes_to_read);
                    page = &current_page;
                    break;
                }

                sharedMemory->clockHand = (sharedMemory->clockHand + 1) % GLOBAL_CACHE_SIZE;
                if (sharedMemory->clockHand == start_hand) {
                    CachePage &forced_page = sharedMemory->cache[sharedMemory->clockHand];
                    forced_page.used = true;
                    forced_page.dirty = false;
                    strcpy(forced_page.path, fileDesc.path.c_str());
                    forced_page.offset = page_aligned_offset;

                    std::memcpy(forced_page.data, chunk, bytes_to_read);
                    page = &forced_page;
                    break;
                }
            }

            if (!page) {
                pthread_mutex_unlock(&sharedMemory->mutex);
                for (auto& [temp_chunk, size] : temp_chunks) {
                    delete[] temp_chunk;
                }
                return -1;
            }
        }


        temp_chunks.push_back({chunk, bytes_to_read});


        bytes_read_total += bytes_to_read;
        fileDesc.cursor += bytes_to_read;
    }

    pthread_mutex_unlock(&sharedMemory->mutex);


    size_t assembled_offset = 0;
    for (auto& [temp_chunk, chunk_size] : temp_chunks) {
        std::memcpy(static_cast<char*>(buf) + assembled_offset, temp_chunk, chunk_size);
        assembled_offset += chunk_size;
        delete[] temp_chunk;
    }

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

                        int file_fd = open(page.path, O_WRONLY);
                        if (file_fd != -1) {
                            lseek(file_fd, page.offset, SEEK_SET);
                            write(file_fd, page.data, PAGE_SIZE);
                            close(file_fd);
                        }
                    }

                    page.used = true;
                    page.dirty = true;
                    strcpy(page.path, fileDesc.path.c_str());
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
                    strcpy(forced_page.path, fileDesc.path.c_str());
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


void manage_cache(int fd, off_t offset, const char *data) {
    if (!found_file_descriptor(fd)) {
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
            for (const auto &entry: fileDescriptors) {
                if (std::strncmp(entry.second.path.c_str(), page.path, sizeof(page.path)) == 0) {
                    cacheFd = entry.first;
                    break;
                }
            }

            if (page.dirty && cacheFd != -1) {
                pwrite(cacheFd, page.data, PAGE_SIZE, page.offset);
            }

            std::strncpy(page.path, filePath.c_str(), sizeof(page.path) - 1);
            page.path[sizeof(page.path) - 1] = '\0';
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
            for (auto &p: sharedMemory->cache) {
                p.used = false;
            }
        }
    }
    pthread_mutex_unlock(&sharedMemory->mutex);
}
}
