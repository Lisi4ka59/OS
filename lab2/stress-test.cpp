#include <iostream>
#include <chrono>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <random>
#include <fstream>


// Declare functions from the library
extern "C" {
int lab2_open(const char *path, int flags);
int lab2_close(int fd);
ssize_t lab2_read(int fd, void *buf, size_t count);
ssize_t lab2_write(int fd, const void *buf, size_t count);
off_t lab2_lseek(int fd, off_t offset, int whence);
int lab2_fsync(int fd);
void initialize_library();
}

int main() {
    size_t buf_size = 1000000;
    if (initialize_library() != 0) {
        std::cerr << "Failed to initialize library.\n";
        return 1;
    }

    auto start = std::chrono::high_resolution_clock::now();

    int fd = lab2_open("testfile.txt", O_RDWR | O_CREAT);
    if (fd < 0) {
        std::cerr << "Failed to open file.\n";
        return 1;
    }

    int control_fd = open("controlfile.txt", O_RDWR | O_CREAT, 0666);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(32, 126);
    char buffer[buf_size];

    for (size_t i = 0; i < buf_size; i++) {
        char random_char = static_cast<char>(dis(gen));
        buffer[i] = random_char;
    }

    ssize_t bytes_written = lab2_write(fd, buffer, buf_size);
    if (bytes_written != buf_size) {
        std::cerr << "Error writing file.\n";
        return 1;
    }

    pwrite(control_fd, buffer, buf_size, 0);


    if (lab2_fsync(fd) < 0) {
        std::cerr << "Error syncing file with lab2_fsync.\n";
        return 1;
    }

    for (int i = 0; i < 50; i++) {

        char new_buffer[buf_size];
        char new_control_buffer[buf_size];
        ssize_t bytes_read = lab2_read(fd, new_buffer, buf_size);
        if (bytes_read != buf_size) {
            std::cerr << "Error reading file.\n";
            break;
        }

        ssize_t control_bytes_read = pread(fd, new_control_buffer, buf_size, 0);

        if (memcmp(new_buffer, new_control_buffer, buf_size) != 0) {
            std::cerr << "Buffers are not equal.\n";
        }


        for (size_t j = 0; j < 10000; i++) {
            char random_char = static_cast<char>(dis(gen));
            new_buffer[j] = random_char;
            new_control_buffer[j] = random_char;
        }

        for (size_t j = 0; j < 100; i++) {
            char random_char = static_cast<char>(dis(gen));
            new_buffer[buf_size + j] = random_char;
            new_control_buffer[buf_size + j] = random_char;
        }
        buf_size += 100;

        ssize_t new_bytes_written = lab2_write(fd, new_buffer, buf_size);
        if (new_bytes_written != buf_size) {
            std::cerr << "Error writing file.\n";
            return 1;
        }

        pwrite(control_fd, new_control_buffer, buf_size, 0);

        std::cout << "Iteration " << i << "completed.";
    }


    if (lab2_close(fd) < 0) {
        std::cerr << "Failed to close file.\n";
        return 1;
    }
    if (close(control_fd) < 0) {
        std::cerr << "Failed to close control file.\n";
        return 1;
    }
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> execution_time = end - start;
    std::cout << "Test completed after " << execution_time.count() << " ms.\n" << std::endl;
    return 0;
}
