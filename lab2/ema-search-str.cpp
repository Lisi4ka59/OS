#include <iostream>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>

#define BUFFER_SIZE 32768 // 32 KiB

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

void search_substring(const char *filename, const char *substring, int repetitions) {
    // Open the file using lab2_open
    int fd = lab2_open(filename, O_RDONLY);
    if (fd == -1) {
        std::cerr << "Error opening file" << std::endl;
        exit(EXIT_FAILURE);
    }

    size_t substring_len = std::strlen(substring);
    char buffer[BUFFER_SIZE + substring_len - 1]; // Buffer with extra space for overlap
    ssize_t bytes_read;
    ssize_t overlap = substring_len - 1;

    for (int rep = 0; rep < repetitions; rep++) {
        if (lab2_lseek(fd, 0, SEEK_SET) == -1) {
            std::cerr << "Error seeking in file" << std::endl;
            lab2_close(fd);
            exit(EXIT_FAILURE);
        }
        ssize_t total_bytes_read = 0;

        while ((bytes_read = lab2_read(fd, buffer + total_bytes_read, BUFFER_SIZE)) > 0) {
            total_bytes_read += bytes_read;

            for (ssize_t i = 0; i <= total_bytes_read - substring_len; i++) {
                if (std::strncmp(&buffer[i], substring, substring_len) == 0) {
//                    std::cout << "Found substring at position "
//                              << i + lab2_lseek(fd, 0, SEEK_CUR) - total_bytes_read
//                              << " in repetition " << rep << std::endl;
                }
            }

            // Move last `overlap` bytes to the start of the buffer
            std::memmove(buffer, buffer + total_bytes_read - overlap, overlap);
            total_bytes_read = overlap;
        }
    }

    lab2_close(fd);
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <filename> <substring> <repetitions>" << std::endl;
        return EXIT_FAILURE;
    }
    initialize_library();

    const char *filename = argv[1];
    const char *substring = argv[2];
    int repetitions = std::atoi(argv[3]);

    search_substring(filename, substring, repetitions);
    return 0;
}
