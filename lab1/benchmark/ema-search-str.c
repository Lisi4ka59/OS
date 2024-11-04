#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define BUFFER_SIZE 32768 // 32 KiB

void search_substring(const char *filename, const char *substring, int repetitions) {
    int fd = open(filename, O_RDONLY);
    if (fd == -1) {
        perror("Error opening file");
        exit(EXIT_FAILURE);
    }

    size_t substring_len = strlen(substring);
    char buffer[BUFFER_SIZE + substring_len - 1]; // Буфер с запасом для перекрытия
    ssize_t bytes_read;
    ssize_t overlap = substring_len - 1;

    for (int rep = 0; rep < repetitions; rep++) {
        // Сброс указателя файла в начало
        if (lseek(fd, 0, SEEK_SET) == -1) {
            perror("Error seeking in file");
            close(fd);
            exit(EXIT_FAILURE);
        }
        ssize_t total_bytes_read = 0;

        while ((bytes_read = read(fd, buffer + total_bytes_read, BUFFER_SIZE)) > 0) {
            total_bytes_read += bytes_read;

            for (ssize_t i = 0; i <= total_bytes_read - substring_len; i++) {
                if (strncmp(&buffer[i], substring, substring_len) == 0) {
                    //printf("Found substring at position %ld in repetition %d\n", i + lseek(fd, 0, SEEK_CUR) - total_bytes_read, rep);
                }
            }

// Переместить последние `overlap` байт в начало буфера
            memmove(buffer, buffer + total_bytes_read - overlap, overlap);
            total_bytes_read = overlap;
        }
    }

    close(fd);
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <filename> <substring> <repetitions>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *filename = argv[1];
    const char *substring = argv[2];
    int repetitions = atoi(argv[3]);

    search_substring(filename, substring, repetitions);

    return 0;
}
