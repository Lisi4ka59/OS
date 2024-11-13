#include <iostream>
#include <chrono>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <random>
#include <fstream>
#include <cstring>
#include <sys/stat.h>
#include <malloc.h>

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

// Function to compare two files byte-by-byte
bool compare_files(const char *file1, const char *file2) {
    int fd1 = open(file1, O_RDONLY);
    int fd2 = open(file2, O_RDONLY);
    if (fd1 < 0 || fd2 < 0) {
        std::cerr << "Error: failed to open one or both files for comparison.\n";
        return false;
    }

    struct stat stat1, stat2;
    fstat(fd1, &stat1);
    fstat(fd2, &stat2);

    // Check if files are of different sizes
    if (stat1.st_size != stat2.st_size) {
        std::cerr << "Files have different sizes.\n";
        close(fd1);
        close(fd2);
        return false;
    }

    const size_t buffer_size = 4096;
    char buffer1[buffer_size], buffer2[buffer_size];

    ssize_t bytes_read1, bytes_read2;
    bool identical = true;
    while ((bytes_read1 = read(fd1, buffer1, buffer_size)) > 0 &&
           (bytes_read2 = read(fd2, buffer2, buffer_size)) > 0) {
        if (bytes_read1 != bytes_read2 || memcmp(buffer1, buffer2, bytes_read1) != 0) {
            identical = false;
            std::cerr << "Files differ.\n";
            break;
        }
    }

    close(fd1);
    close(fd2);
    return identical;
}

int main() {
    // Remove files at the beginning to ensure a clean test environment
    remove("testfile.txt");
    remove("controlfile.txt");

    size_t buf_size = 4096 * 2;  // Adjusted to the nearest multiple of 4096
    const size_t alignment = 4096;  // Align buffers to 4096 bytes for O_DIRECT
    initialize_library();

    auto start = std::chrono::high_resolution_clock::now();

    int fd = lab2_open("testfile.txt", O_RDWR | O_CREAT);
    if (fd < 0) {
        std::cerr << "Failed to open file.\n";
        return 1;
    }

    int control_fd = open("controlfile.txt", O_RDWR | O_CREAT | O_DIRECT, 0666);
    if (control_fd < 0) {
        std::cerr << "Failed to open control file with O_DIRECT.\n";
        return 1;
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(32, 126);

    // Allocate aligned buffers for O_DIRECT
    char *buffer;
    if (posix_memalign((void **) &buffer, alignment, buf_size) != 0) {
        std::cerr << "Failed to allocate aligned memory for buffer.\n";
        return 1;
    }
    for (size_t i = 0; i < buf_size; i++) {
        buffer[i] = static_cast<char>(dis(gen));
    }

    ssize_t bytes_written = lab2_write(fd, buffer, buf_size);
    if (bytes_written != buf_size) {
        std::cerr << "Error writing file. Asked " << buf_size << " did " << bytes_written << "\n";
        free(buffer);
        return 1;
    }

    // Use pwrite for O_DIRECT and aligned buffer
    ssize_t control_bytes_written = pwrite(control_fd, buffer, buf_size, 0);
    if (control_bytes_written != buf_size) {
        std::cerr << "Error writing to control file with O_DIRECT. Asked " << buf_size << " did "
                  << control_bytes_written << "\n";
        free(buffer);
        return 1;
    }
    fsync(control_fd);

    if (lab2_fsync(fd) < 0) {
        std::cerr << "Error syncing file with lab2_fsync.\n";
        free(buffer);
        return 1;
    }

    // Close files before comparing them
    lab2_close(fd);
    close(control_fd);

    // Compare files before starting the main loop
    if (!compare_files("testfile.txt", "controlfile.txt")) {
        std::cerr << "Files are NOT identical before the test loop starts.\n";
        free(buffer);
        return 1;  // Exit early if files are already different
    } else {
        std::cout << "Files are identical before the test loop starts.\n";
    }

    // Reopen files for the main test loop
    fd = lab2_open("testfile.txt", O_RDWR);
    control_fd = open("controlfile.txt", O_RDWR | O_DIRECT);
    if (fd < 0 || control_fd < 0) {
        std::cerr << "Failed to reopen files for main loop.\n";
        free(buffer);
        return 1;
    }

    char *new_buffer;
    char *new_control_buffer;
    if (posix_memalign((void **) &new_buffer, alignment, buf_size) != 0 ||
        posix_memalign((void **) &new_control_buffer, alignment, buf_size) != 0) {
        std::cerr << "Failed to allocate aligned memory for read buffers.\n";
        free(buffer);
        return 1;
    }

    for (int i = 0; i < 50; i++) {
        ssize_t bytes_read = lab2_read(fd, new_buffer, buf_size);
        if (bytes_read != buf_size) {
            std::cerr << "Error reading file. Asked " << buf_size << " got " << bytes_read << "\n";
            break;
        }

        ssize_t control_bytes_read = pread(control_fd, new_control_buffer, buf_size, 0);
        if (control_bytes_read != buf_size) {
            std::cerr << "Error reading control file with O_DIRECT. Asked " << buf_size << " got " << control_bytes_read
                      << "\n";
            break;
        }

        // Compare buffers
        if (memcmp(new_buffer, new_control_buffer, buf_size) != 0) {
            std::cout << new_buffer << std::endl << std::endl;
            // std::cout <<new_control_buffer << std::endl<< std::endl;
            break;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end - start;
    std::cout << "Test completed after " << duration.count() << " ms.\n";

    lab2_close(fd);
    close(control_fd);

    // Free aligned buffers
    free(buffer);
    free(new_buffer);
    free(new_control_buffer);

    return 0;
}
