#include <iostream>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <random>
#include <cstring>
#include <cstdlib>


extern "C" {
int lab2_open(const char *path, int flags);
int lab2_close(int fd);
ssize_t lab2_read(int fd, void *buf, size_t count);
ssize_t lab2_write(int fd, const void *buf, size_t count);
off_t lab2_lseek(int fd, off_t offset, int whence);
int lab2_fsync(int fd);
void initialize_library();
}

constexpr size_t PAGE_SIZE = 4096;

int main() {
    size_t buf_size = PAGE_SIZE * 16 * 16; // Плюс минус 1 мегабайт

    initialize_library();

    auto start = std::chrono::high_resolution_clock::now();

    int fd = lab2_open("testfile.txt", O_RDWR | O_CREAT);
    if (fd < 0) {
        std::cerr << "Failed to open file.\n";
        return 1;
    }

    int control_fd = open("controlfile.txt", O_RDWR | O_CREAT | O_DIRECT, 0666);
    if (control_fd < 0) {
        std::cerr << "Failed to open control file.\n";
        return 1;
    }

    char *buffer;
    if (posix_memalign(reinterpret_cast<void **>(&buffer), PAGE_SIZE, buf_size) != 0) {
        std::cerr << "Error allocating aligned buffer.\n";
        lab2_close(fd);
        close(control_fd);
        return 1;
    }
    std::cout << "Allocated buffer at address: " << static_cast<void *>(buffer) << "\n";

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(32, 126);

    for (size_t i = 0; i < buf_size; i++) {
        buffer[i] = static_cast<char>(dis(gen));
    }

    ssize_t bytes_written = lab2_write(fd, buffer, buf_size);
    if (bytes_written != buf_size) {
        std::cerr << "Error writing file. Written: " << bytes_written << " Expected: " << buf_size << "\n";
        free(buffer);
        lab2_close(fd);
        close(control_fd);
        return 1;
    }

    ssize_t control_bytes_written = pwrite(control_fd, buffer, buf_size, 0);
    if (control_bytes_written != buf_size) {
        std::cerr << "Error writing control file. Written: " << control_bytes_written << " Expected: " << buf_size
                  << "\n";
        free(buffer);
        lab2_close(fd);
        close(control_fd);
        return 1;
    }


    if (lab2_fsync(fd) < 0) {
        std::cerr << "Error syncing file with lab2_fsync.\n";
        free(buffer);
        lab2_close(fd);
        close(control_fd);
        return 1;
    }

    char *new_buffer, *new_control_buffer;
    if (posix_memalign(reinterpret_cast<void **>(&new_buffer), PAGE_SIZE, buf_size) != 0) {
        std::cerr << "Error allocating aligned new_buffer.\n";
        free(buffer);
        lab2_close(fd);
        close(control_fd);
        return 1;
    }
    std::cout << "Allocated new_buffer at address: " << static_cast<void *>(new_buffer) << "\n";

    if (posix_memalign(reinterpret_cast<void **>(&new_control_buffer), PAGE_SIZE, buf_size) != 0) {
        std::cerr << "Error allocating aligned new_control_buffer.\n";
        free(new_buffer);
        free(buffer);
        lab2_close(fd);
        close(control_fd);
        return 1;
    }
    std::cout << "Allocated new_control_buffer at address: " << static_cast<void *>(new_control_buffer) << "\n";

    for (int i = 0; i < 10; i++) {
        lab2_lseek(fd, 0, SEEK_SET);

        ssize_t bytes_read = lab2_read(fd, new_buffer, buf_size);
        if (bytes_read != buf_size) {
            std::cerr << "Error reading file. Bytes read: " << bytes_read << " Expected: " << buf_size << "\n";
            break;
        }

        ssize_t control_bytes_read = pread(control_fd, new_control_buffer, buf_size, 0);
        if (control_bytes_read != buf_size) {
            std::cerr << "Error reading control file. Bytes read: " << control_bytes_read << " Expected: " << buf_size
                      << "\n";
            break;
        }

        if (memcmp(new_buffer, new_control_buffer, buf_size) != 0) {
            std::cerr << "Buffers are not equal at iteration " << i << ".\n";

            for (size_t j = 0; j < buf_size; j++) {
                if (new_buffer[j] != new_control_buffer[j]) {
                    std::cerr << "Difference at byte " << j
                              << ": new_buffer[" << j << "] = " << static_cast<int>(new_buffer[j])
                              << ", new_control_buffer[" << j << "] = " << static_cast<int>(new_control_buffer[j])
                              << "\n";
                    break;
                }
            }
            break;
        }

        for (size_t j = 0; j < buf_size; j++) {
            char random_char = static_cast<char>(dis(gen));
            new_buffer[j] = random_char;
            new_control_buffer[j] = random_char;
        }

        lab2_lseek(fd, 0, SEEK_SET);
        ssize_t new_bytes_written = lab2_write(fd, new_buffer, buf_size);
        if (new_bytes_written != buf_size) {
            std::cerr << "Error writing file at iteration " << i << ". Written: " << new_bytes_written << " Expected: "
                      << buf_size << "\n";
            break;
        }

        pwrite(control_fd, new_control_buffer, buf_size, 0);

        std::cout << "Iteration " << i << " completed successfully.\n";
    }

    std::cout << "Freeing buffers...\n";
    free(new_buffer);
    free(new_control_buffer);
    free(buffer);
    std::cout << "Buffers freed.\n";

    lab2_close(fd);
    close(control_fd);

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> execution_time = end - start;
    std::cout << "Test completed after " << execution_time.count() << " ms.\n";

    return 0;
}
