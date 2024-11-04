#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <limits.h>

#define BUFFER_SIZE 32768 // 32 KiB
#define V 10 

int graph[V][V] = {
        {0, 10, 20, 0, 0, 0, 0, 0, 0, 0},
        {10, 0, 30, 5, 0, 0, 0, 0, 0, 0},
        {20, 30, 0, 15, 6, 0, 0, 0, 0, 0},
        {0, 5, 15, 0, 8, 0, 0, 0, 0, 0},
        {0, 0, 6, 8, 0, 12, 0, 0, 0, 0},
        {0, 0, 0, 0, 12, 0, 7, 0, 0, 0},
        {0, 0, 0, 0, 0, 7, 0, 9, 0, 0},
        {0, 0, 0, 0, 0, 0, 9, 0, 11, 0},
        {0, 0, 0, 0, 0, 0, 0, 11, 0, 13},
        {0, 0, 0, 0, 0, 0, 0, 0, 13, 0}
};

int minPath = INT_MAX;

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
        lseek(fd, 0, SEEK_SET); // Сброс указателя файла в начало
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

void findShortestPath(int current, int visited[], int pathLength, int start) {
    visited[current] = 1;

    int allVisited = 1;
    for (int i = 0; i < V; i++) {
        if (!visited[i] && graph[current][i] != 0) {
            allVisited = 0;
            findShortestPath(i, visited, pathLength + graph[current][i], start);
        }
    }

    if (allVisited) {
        pathLength += graph[current][start];
        if (pathLength < minPath) {
            minPath = pathLength;
        }
    }

    visited[current] = 0;
}

void find_shortest_path(int repetitions) {
    for (int rep = 0; rep < repetitions; rep++) {
        int visited[V] = {0};
        minPath = INT_MAX;
        findShortestPath(0, visited, 0, 0);
        //printf("Repetition %d: Shortest path length is %d\n", rep, minPath);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <filename> <substring>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    pid_t pids[12];

// Аргументы для поиска подстроки
    const char *filename = argv[1];
    const char *substring = argv[2];

// Создание процессов
    for (int i = 0; i < 12; i++) {
        if ((pids[i] = fork()) == 0) {
            if (i < 6) {
// Первые три процесса выполняют поиск подстроки
                search_substring(filename, substring, 1);
            } else {
// Последние три процесса выполняют поиск кратчайшего пути
                find_shortest_path(1045000);
            }
            exit(0);
        }
    }

// Ожидание завершения всех процессов
    for (int i = 0; i < 12; i++) {
        waitpid(pids[i], NULL, 0);
    }

    return 0;
}
