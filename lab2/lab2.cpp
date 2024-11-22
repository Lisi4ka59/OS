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

constexpr size_t GLOBAL_CACHE_SIZE = 16 * 16 * 50;   // Count of cache pages (50 MB)
constexpr size_t PAGE_SIZE = 4096;                   // Size of single page (4 KB)
const char *SHARED_MEMORY_NAME = "/globalCache_shm";


struct CachePage {
    ino_t inode;                // Inode number of the file
    off_t offset;               // Offset of this page in the file
    char data[PAGE_SIZE];       // Data buffer of the page in shared memory
    bool used;                  // Used flag for clock policy
    bool dirty;                 // Dirty flag
};

struct CachePageIndex {
    ino_t inode;
    off_t offset;
    CachePage *page;

    bool operator<(const CachePageIndex &other) const {
        return std::tie(inode, offset) < std::tie(other.inode, other.offset);
    }
};


struct FileDescriptor {
    int fd;                     // File descriptor
    off_t cursor;               // Current cursor position in the file
    ino_t inode;                // Inode number for cache identification
};


struct SharedMemory {
    std::atomic<int> refCount;      // Count of active processes
    std::atomic<size_t> clockHand;  // Clock hand for cache replacement
    pthread_mutex_t mutex;          // Mutex
    CachePage cache[GLOBAL_CACHE_SIZE]; // Cache page structure
    CachePageIndex cacheIndex[GLOBAL_CACHE_SIZE];  // Array for cache index
    size_t cacheIndexCount = 0;                    // Number of active elements in cacheIndex

};


std::unordered_map<int, FileDescriptor> fileDescriptors;
SharedMemory *sharedMemory = nullptr;

extern "C" {

// Это надо чтобы общую память сделатт
void attach_shared_memory() {
    int shm_fd = shm_open(SHARED_MEMORY_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("Failed to open shared memory");
        exit(1);
    }
    if (ftruncate(shm_fd, sizeof(SharedMemory)) == -1) {
        perror("Failed to set shared memory size");
        close(shm_fd);
        exit(1);
    }

    sharedMemory = static_cast<SharedMemory *>(mmap(nullptr, sizeof(SharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED,
                                                    shm_fd, 0));
    if (sharedMemory == MAP_FAILED) {
        perror("Failed to map shared memory");
        close(shm_fd);
        exit(1);
    }
    close(shm_fd);

    if (sharedMemory->refCount.fetch_add(1) == 0) {
        sharedMemory->clockHand = 0;
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        pthread_mutex_init(&sharedMemory->mutex, &attr);
        pthread_mutexattr_destroy(&attr);

        // Инициализируем стронички
        for (CachePage &page: sharedMemory->cache) {
            page.used = false;
            page.dirty = false;
            page.inode = 0;
            page.offset = 0;
        }
    }
}

// Штука для того, чтобы потом эта библиотека завелась
void detach_shared_memory() {
    if (sharedMemory && sharedMemory->refCount.fetch_sub(1) == 1) {
        pthread_mutex_destroy(&sharedMemory->mutex);
        shm_unlink(SHARED_MEMORY_NAME);
    }
    munmap(sharedMemory, sizeof(SharedMemory));
}

// из названия и так понятно что это
void initialize_library() {
    attach_shared_memory();
    atexit(detach_shared_memory);
}


// валидируем дескриптор
void found_file_descriptor(int fd) {
    if (fileDescriptors.find(fd) == fileDescriptors.end()) {
        exit(1);
    }
}

// получаем инод файла из дескриптора
ino_t get_file_inode(int fd) {
    struct stat file_stat;
    if (fstat(fd, &file_stat) == -1) {
        perror("Failed to get file inode");
        close(fd);
        exit(1);
    }
    return file_stat.st_ino;
}

// открываем и сохраняем инод
int lab2_open(const char *path, int flags) {
    // ОДИРЕКТ надо чтобы без всех этих вашей пейдж кэшей работать с файлом
    flags |= O_DIRECT;
    int fd = open(path, flags);
    if (fd == -1) return -1;
    ino_t inode = get_file_inode(fd);
    fileDescriptors[fd] = {fd, 0, inode};
    return fd;
}

// Находим страницу в кэше по иноду и оффсету
CachePage *find_cache_page(ino_t inode, off_t offset) {
    CachePageIndex key = {inode, offset, nullptr};
    size_t left = 0, right = sharedMemory->cacheIndexCount;

    while (left < right) {
        size_t mid = left + (right - left) / 2;
        if (sharedMemory->cacheIndex[mid] < key) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    if (left < sharedMemory->cacheIndexCount &&
        sharedMemory->cacheIndex[left].inode == inode &&
        sharedMemory->cacheIndex[left].offset == offset) {
        return sharedMemory->cacheIndex[left].page;
    }
    return nullptr;
}

// обновляем индекс
void update_cache_page_index(ino_t inode, off_t offset, ino_t new_inode, off_t new_offset, CachePage *page) {
    size_t pos = 0;
    while (pos < sharedMemory->cacheIndexCount && !(sharedMemory->cacheIndex[pos].inode == inode && sharedMemory->cacheIndex[pos].offset == offset)) {
        pos++;
    }
    if (pos < sharedMemory->cacheIndexCount) {
        sharedMemory->cacheIndex[pos].inode = new_inode;
        sharedMemory->cacheIndex[pos].offset = new_offset;
    } else {
        CachePageIndex newIndex = {new_inode, new_offset, page};
        while (pos < sharedMemory->cacheIndexCount && sharedMemory->cacheIndex[pos] < newIndex) {
            pos++;
        }
        for (size_t i = sharedMemory->cacheIndexCount; i > pos; --i) {
            sharedMemory->cacheIndex[i] = sharedMemory->cacheIndex[i - 1];
        }

        sharedMemory->cacheIndex[pos] = newIndex;
        sharedMemory->cacheIndexCount++;
    }
}


// Читаем что нибудь из файла
ssize_t read_data_from_file(int fd, off_t offset, char *chunk, size_t bytes_to_read) {
    if (lseek(fd, offset, SEEK_SET) == -1) {
        perror("Failed to seek");
        return -1;
    }
    ssize_t bytes_from_file = read(fd, chunk, bytes_to_read);
    if (bytes_from_file < bytes_to_read && bytes_from_file > 0) {
        memset(chunk + bytes_from_file, 0, bytes_to_read - bytes_from_file);
    }
    return bytes_from_file;
}

// Записываем страницу в кэш
void update_cache_page(CachePage &current_page, ino_t inode, off_t offset, const char *data, size_t bytes_to_read) {
    update_cache_page_index(current_page.inode, current_page.offset, inode, offset, &current_page);
    current_page.used = true;
    current_page.dirty = false;
    current_page.inode = inode;
    current_page.offset = offset;
    memcpy(current_page.data, data, bytes_to_read);
}

// Проверяем, и если страница испачкана, то скидываем ее на диск
void flush_dirty_page(CachePage &page, int fd) {
    if (page.dirty) {
        if (lseek(fd, page.offset, SEEK_SET) != -1) {
            write(fd, page.data, PAGE_SIZE);
        }
        page.dirty = false;
    }
}

// Находим страницу которую можно выкинуть
CachePage *get_cache_page_to_replace(int fd) {
    while (true) {
        CachePage &page = sharedMemory->cache[sharedMemory->clockHand];
        if (!page.used) {
            flush_dirty_page(page, fd);
            sharedMemory->clockHand = (sharedMemory->clockHand + 1) % GLOBAL_CACHE_SIZE;
            return &page;
        }
        page.used = false;
        sharedMemory->clockHand = (sharedMemory->clockHand + 1) % GLOBAL_CACHE_SIZE;
    }
}


void *allocate_aligned_memory(size_t size) {
    void *ptr;
    posix_memalign(&ptr, PAGE_SIZE, size);
    return ptr;
}

ssize_t lab2_read(int fd, void *buf, size_t count) {
    if (count > GLOBAL_CACHE_SIZE * PAGE_SIZE) {
        return pread(fd, buf, count, 0);
    }
    found_file_descriptor(fd);
    FileDescriptor &fileDesc = fileDescriptors[fd];
    size_t bytes_read_total = 0;
    std::vector<std::pair<char *, size_t>> temp_chunks;
    pthread_mutex_lock(&sharedMemory->mutex);

    while (bytes_read_total < count) {
        off_t page_aligned_offset = fileDesc.cursor / PAGE_SIZE * PAGE_SIZE;
        size_t page_offset = fileDesc.cursor % PAGE_SIZE;
        size_t bytes_to_read = std::min(PAGE_SIZE - page_offset, count - bytes_read_total);

        char *chunk = static_cast<char *>(allocate_aligned_memory(PAGE_SIZE));

        CachePage *page = find_cache_page(fileDesc.inode, page_aligned_offset);
        if (!page) {
            ssize_t bytes_from_file = read_data_from_file(fd, fileDesc.cursor, chunk, bytes_to_read);
            if (bytes_from_file <= 0) {
                free(chunk);
                break;
            }

            CachePage &current_page = *get_cache_page_to_replace(fd);
            update_cache_page(current_page, fileDesc.inode, page_aligned_offset, chunk, bytes_to_read);
        } else {
            memcpy(chunk, page->data + page_offset, bytes_to_read);
        }

        temp_chunks.push_back({chunk, bytes_to_read});
        bytes_read_total += bytes_to_read;
        fileDesc.cursor += bytes_to_read;
    }

    pthread_mutex_unlock(&sharedMemory->mutex);

    size_t assembled_offset = 0;
    for (auto &[temp_chunk, chunk_size]: temp_chunks) {
        memcpy(static_cast<char *>(buf) + assembled_offset, temp_chunk, chunk_size);
        assembled_offset += chunk_size;
        free(temp_chunk);
    }

    return bytes_read_total;
}


ssize_t lab2_write(int fd, const char *buffer, size_t size) {
    if (size > GLOBAL_CACHE_SIZE * PAGE_SIZE) {
        return pwrite(fd, buffer, size, 0);
    }
    found_file_descriptor(fd);
    FileDescriptor &fileDesc = fileDescriptors[fd];
    size_t bytes_written = 0;
    pthread_mutex_lock(&sharedMemory->mutex);
    while (bytes_written < size) {
        off_t offset = fileDesc.cursor / PAGE_SIZE * PAGE_SIZE;
        size_t page_offset = fileDesc.cursor % PAGE_SIZE;
        size_t bytes_to_write = std::min(PAGE_SIZE - page_offset, size - bytes_written);
        CachePage *page = find_cache_page(fileDesc.inode, offset);

        if (page) {
            // если страница нашлась, то пишем туды и отмечаем ее как очень грязную
            memcpy(page->data + page_offset, buffer + bytes_written, bytes_to_write);
            page->dirty = true;
        } else {
            // если страница не нашлась, то подрубаем клок и вытесняем и загружаем нужную страницу
            CachePage &current_page = *get_cache_page_to_replace(fd);
            update_cache_page(current_page, fileDesc.inode, offset, buffer + bytes_written, bytes_to_write);
            current_page.dirty = true;
            }
        fileDesc.cursor += bytes_to_write;
        bytes_written += bytes_to_write;
    }
    pthread_mutex_unlock(&sharedMemory->mutex);
    return bytes_written;
}


int lab2_close(int fd) {
    found_file_descriptor(fd);
    ino_t inode = fileDescriptors[fd].inode;
    pthread_mutex_lock(&sharedMemory->mutex);
    for (size_t i = 0; i < GLOBAL_CACHE_SIZE; i++) {
        CachePage &page = sharedMemory->cache[i];
        if (page.inode == inode) {
            page.used = false;
            flush_dirty_page(page, fd);
        }
    }
    pthread_mutex_unlock(&sharedMemory->mutex);
    fileDescriptors.erase(fd);
    return close(fd);
}


off_t lab2_lseek(int fd, off_t offset, int whence) {
    found_file_descriptor(fd);

    FileDescriptor &fileDesc = fileDescriptors[fd];
    off_t new_offset;
    switch (whence) {
        case SEEK_SET:
            new_offset = offset;
            break;
        case SEEK_CUR:
            new_offset = fileDesc.cursor + offset;
            break;
        case SEEK_END: {
            struct stat st;
            if (fstat(fd, &st) == -1) {
                return -1;
            }
            new_offset = st.st_size + offset;
            break;
        }
        default:
            errno = EINVAL;
            return -1;
    }

    if (new_offset < 0) {
        errno = EINVAL;
        return -1;
    }
    fileDesc.cursor = new_offset;
    return new_offset;
}

// Синхронизация
int lab2_fsync(int fd) {
    found_file_descriptor(fd);
    FileDescriptor &fileDesc = fileDescriptors[fd];
    ino_t inode = fileDesc.inode;

    pthread_mutex_lock(&sharedMemory->mutex);
    for (size_t i = 0; i < GLOBAL_CACHE_SIZE; i++) {
        CachePage &page = sharedMemory->cache[i];
        if (page.dirty && page.inode == inode) {
            char *aligned_buffer;
            posix_memalign(reinterpret_cast<void **>(&aligned_buffer), PAGE_SIZE, PAGE_SIZE);
            memcpy(aligned_buffer, page.data, PAGE_SIZE);

            if (lseek(fd, page.offset, SEEK_SET) != -1) {
                if (write(fd, aligned_buffer, PAGE_SIZE) == -1) {
                    perror("Failed to write page to disk");
                    free(aligned_buffer);
                    pthread_mutex_unlock(&sharedMemory->mutex);
                    return -1;
                }
            } else {
                perror("Failed to seek in file");
                free(aligned_buffer);
                pthread_mutex_unlock(&sharedMemory->mutex);
                return -1;
            }

            free(aligned_buffer);
            page.dirty = false;
        }
    }
    pthread_mutex_unlock(&sharedMemory->mutex);

    return fsync(fd);
}

}
