# Имя выходного файла
TARGET = lb1

# Компилятор
CC = gcc

# Флаги компиляции
CFLAGS = -Wall -pthread

# Исходные файлы
SRCS = main.c

# Объектные файлы
OBJS = $(SRCS:.c=.o)

# Правило по умолчанию: сборка программы
all: $(TARGET)

# Правило сборки цели
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

# Правило для компиляции исходных файлов в объектные
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Очистка сгенерированных файлов
clean:
	rm -f $(OBJS) $(TARGET)

# Пересборка программы
rebuild: clean all
