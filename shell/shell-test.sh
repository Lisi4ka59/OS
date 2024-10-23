#!/bin/bash

# Компиляция программы
gcc -o main main.c

# Создание тестовых файлов
echo "This is a test file with the word test in it." > testfile.txt

# Функция для выполнения теста
run_test() {
local command=$1
local expected_output=$2

# Запуск программы и захват вывода
output=$(./main <<EOF
$command
exit
EOF
)

# Проверка вывода
if [[ "$output" == *"$expected_output"* ]]; then
echo "Test passed"
else
echo "Test failed"
echo "Expected: $expected_output"
echo "Got: $output"
exit 1
fi
}

# Тест 1: Проверка корректного выполнения команды ls
run_test "ls" "testfile.txt"

# Тест 2: Проверка корректного выполнения команды echo
run_test "echo Hello, World!" "Hello, World!"

# Тест 3: Проверка корректного выполнения команды cat
run_test "cat testfile.txt" "This is a test file with the word test in it."
