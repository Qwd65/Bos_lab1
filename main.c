//Дополнительные функции для работы с файлами, процессами и памятью
#define _GNU_SOURCE
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>   // Для clone
#include <unistd.h>  // Для sleep, usleep
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>  // Для shared memory
#include <linux/futex.h>
#include <sys/syscall.h>//базовые системные вызовы
#include <sys/time.h>
#include <errno.h>

// Размер стека для потоков
#define STACK_SIZE 1024*1024*4

// Узел для AVL дерева
typedef struct Node {
    int key;
    struct Node* left;
    struct Node* right;
    int height;
} Node;

// Глобальные переменные, которые будут разделяться между потоками
int *current_size;
int *added_elements;
int stop_flag = 0;

//Переменные с информацией
int added_count = 0; // Счетчик добавленных чисел
int deleted_count = 0; // Счетчик удаленных чисел


// Переменные, значения которых задаются из командной строки
int add_threads_count;
int del_threads_count;
int max_size;
int min_value;
int max_value;

// Простая функция для блокировки с использованием futex
int futex_wait(volatile int *addr, int val) {
    return syscall(SYS_futex, addr, FUTEX_WAIT, val, NULL, NULL, 0);// вызываем системный вызов futex, ожидаем, что значение по адресу addr станет равным val. Если текущее значение addr не равно val, поток блокируется до тех пор, пока другой поток не вызовет futex_wake
}

// Разблокировка futex
int futex_wake(volatile int *addr, int val) {
    return syscall(SYS_futex, addr, FUTEX_WAKE, val, NULL, NULL, 0);//разблокирует val потоков, ожидающих на адресе addr
}//SYS_futex -  указывающая, что мы вызываем системный вызов futex FUTEX_WAKE - код операции, указывающий, что мы хотим разблокировать потоки

volatile int mutex = 0;  // Простой мьютекс на основе futex

void lock(volatile int *mutex) {//Эта функция пытается захватить мьютекс. __sync_lock_test_and_set(mutex, 1) устанавливает значение mutex в 1 и возвращает предыдущее значение. Если возвращаемое значение не равно 0, это значит, что мьютекс уже захвачен другим потоком.
    while (__sync_lock_test_and_set(mutex, 1)) {//
        futex_wait(mutex, 1);
    }
}

void unlock(volatile int *mutex) {//__sync_lock_release(mutex) сбрасывает значение mutex в 0, указывая, что мьютекс освобождён. volatile — ключевое слово , указывает компилятору,  не нужно оптимизировать доступ к ней(кешировать)
    __sync_lock_release(mutex);
    futex_wake(mutex, 1);
}

Node* tree = NULL; // Глобальное дерево

// Функции для работы с деревом
int max(int a, int b) {
    return (a > b) ? a : b;
}
//Возаращает высоту
int height(Node* node) {
    if (node == NULL)
        return 0;
    return node->height;
}

// Функция для проверки бинарного свойства дерева
int check_binary_tree(Node* node, int min_key, int max_key) {
    if (node == NULL)
        return 1;  // Пустое поддерево корректно

    // Проверяем, что ключ узла находится в допустимых пределах
    if (node->key < min_key || node->key > max_key)
        return 0;  // Нарушено бинарное свойство

    // Рекурсивно проверяем левое и правое поддеревья
    return check_binary_tree(node->left, min_key, node->key - 1) &&
           check_binary_tree(node->right, node->key + 1, max_key);
}

// Функция для проверки сбалансированности AVL-дерева
int check_balanced_tree(Node* node) {
    if (node == NULL)
        return 1;  // Пустое поддерево сбалансировано

    int left_height = height(node->left);
    int right_height = height(node->right);

    // Проверяем разницу в высотах
    if (abs(left_height - right_height) > 1)
        return 0;  // Дерево несбалансировано

    // Рекурсивно проверяем сбалансированность поддеревьев
    return check_balanced_tree(node->left) && check_balanced_tree(node->right);
}

// Функция для проверки целостности дерева
int check_tree_integrity(Node* root) {
    // Проверяем бинарное свойство
    if (!check_binary_tree(root, INT_MIN, INT_MAX)) {
        printf("Ошибка: Нарушено бинарное свойство дерева!\n");
        return 0;
    }

    // Проверяем сбалансированность дерева
    if (!check_balanced_tree(root)) {
        printf("Ошибка: Дерево несбалансировано!\n");
        return 0;
    }

    printf("Целостность дерева проверена успешно: Дерево бинарное и сбалансировано.\n");
    return 1;
}


Node* create_node(int key) {
    Node* node = (Node*)malloc(sizeof(Node));
    if (node == NULL) {
        perror("malloc failed");
        exit(EXIT_FAILURE);
    }
    node->key = key;
    node->left = node->right = NULL;
    node->height = 1;
    return node;
}

//Далее идут функции для балансировкм дерева
Node* right_rotate(Node* y) {
    Node* x = y->left;
    Node* T2 = x->right;

    // Выполняем вращение
    x->right = y;
    y->left = T2;

    // Обновляем высоты
    y->height = max(height(y->left), height(y->right)) + 1;
    x->height = max(height(x->left), height(x->right)) + 1;

    return x;
}

// Функция для вращения узла влево
Node* left_rotate(Node* x) {
    Node* y = x->right;
    Node* T2 = y->left;

    // Выполняем вращение
    y->left = x;
    x->right = T2;

    // Обновляем высоты
    x->height = max(height(x->left), height(x->right)) + 1;
    y->height = max(height(y->left), height(y->right)) + 1;

    return y;
}

// Получаем баланс узла
int get_balance(Node* node) {
    if (node == NULL)
        return 0;
    return height(node->left) - height(node->right);
}

Node* insert(Node* node, int key) {
    // Обычная вставка
    if (node == NULL) {
        (*current_size)++;
        return create_node(key);
    }

    if (key < node->key) {
        node->left = insert(node->left, key);//рекурсия
    } else if (key > node->key) {
        node->right = insert(node->right, key);
    } else {
        // Дубликаты не допускаются
        //printf("Элемент с ключом %d уже существует, пропуск вставки.\n", key);
        return node;
    }

    // Обновляем высоту узла
    node->height = 1 + max(height(node->left), height(node->right));

    // Проверяем баланс узла
    int balance = get_balance(node);

    // Если узел стал несбалансированным, выполняем повороты
    if (balance > 1 && key < node->left->key) {
        return right_rotate(node); // Левый Левый случай
    }
    if (balance < -1 && key > node->right->key) {
        return left_rotate(node); // Правый Правый случай
    }
    if (balance > 1 && key > node->left->key) {
        node->left = left_rotate(node->left); // Левый Правый случай
        return right_rotate(node);
    }
    if (balance < -1 && key < node->right->key) {
        node->right = right_rotate(node->right); // Правый Левый случай
        return left_rotate(node);
    }

    return node; // Возвращаем указатель на узел
}

Node* delete_node(Node* root, int key) {
    if (root == NULL)
        return root;

    if (key < root->key)
        root->left = delete_node(root->left, key);//рекурсия
    else if (key > root->key)
        root->right = delete_node(root->right, key);
    else {
        if ((root->left == NULL) || (root->right == NULL)) {
            Node* temp = root->left ? root->left : root->right;
            if (temp == NULL) {
                temp = root;
                root = NULL;
            } else {
                *root = *temp; // Копируем значение узла
            }
            free(temp);
            (*current_size)--;
        } else {
            Node* temp = root->right; // Находим следующий по величине
            while (temp->left != NULL) {
                temp = temp->left;
            }
            root->key = temp->key; // Копируем значение
            root->right = delete_node(root->right, temp->key);
        }
    }

    if (root == NULL)
        return root;
    //копия балансировки из прошлой функции
    // Обновляем высоту узла
    root->height = 1 + max(height(root->left), height(root->right));

    // Проверяем баланс узла
    int balance = get_balance(root);

    // Если узел стал несбалансированным, выполняем повороты
    if (balance > 1 && get_balance(root->left) >= 0)
        return right_rotate(root); // Левый Левый случай
    if (balance < -1 && get_balance(root->right) <= 0)
        return left_rotate(root); // Правый Правый случай
    if (balance > 1 && get_balance(root->left) < 0) {
        root->left = left_rotate(root->left); // Левый Правый случай
        return right_rotate(root);
    }
    if (balance < -1 && get_balance(root->right) > 0) {
        root->right = right_rotate(root->right); // Правый Левый случай
        return left_rotate(root);
    }

    return root;
}


// Функция для добавляющих потоков
int add_thread_func(void *arg) {
    while (!stop_flag) {
        int random_value = rand() % (max_value - min_value + 1) + min_value;
        if (*current_size < max_size) {
            lock(&mutex);  // Захватываем мьютекс на время работы с деревом

            // Вставляем элемент в дерево
            tree = insert(tree, random_value);
            (*added_elements)++;
            //printf("Добавлено: %d\n", random_value);
            added_count++;
            unlock(&mutex);  // Освобождаем мьютекс
        }

        usleep(100000); // Задержка для имитации работы
    }
    return 0;
}

// Функция для удаляющих потоков
int del_thread_func(void *arg) {
    while (!stop_flag) {
        int random_value = rand() % (max_value - min_value + 1) + min_value;

        lock(&mutex);

        // Удаляем элемент из дерева
        tree = delete_node(tree, random_value);
        //printf("Удалено: %d\n", random_value);
        //printf("%02d\t",deleted_count);
        deleted_count++;
        unlock(&mutex);

        usleep(150000);
    }
    return 0;
}

// Функция для мониторинга размера дерева
int monitor_thread_func(void *arg) {
    while (!stop_flag) {
        lock(&mutex); // Блокируем только на чтение размера
        int size = __sync_fetch_and_add(current_size, 0);
        int added = __sync_fetch_and_add(added_elements, 0);
        //int deleted = deleted_count; // Получаем общее количество удаленных элементов
        //printf(deleted_count);
        printf("Размер дерева: %d, Добавлено за последнюю секунду: %d\tОбщее добавлено: %02d\tОбщее удалено: %02d\n",size, added, added_count, deleted_count);
        __sync_fetch_and_and(added_elements, 0); // Сброс счетчика добавлений

        unlock(&mutex); // Освобождаем мьютекс
        sleep(1); // Пауза на 1 секунду
    }
    return 0;
}


int main(int argc, char* argv[]) {
    if (argc != 6) {
        fprintf(stderr, "Использование: %s <add_threads> <del_threads> <max_size> <min_value> <max_value>\n", argv[0]);
        return 1;
    }

    // Парсинг аргументов командной строки
    add_threads_count = atoi(argv[1]);
    del_threads_count = atoi(argv[2]);
    max_size = atoi(argv[3]);
    min_value = atoi(argv[4]);
    max_value = atoi(argv[5]);

    // Проверка корректности введенных значений
    if (add_threads_count <= 0 || del_threads_count < 0 || max_size <= 0 || min_value > max_value) {
        fprintf(stderr, "Некорректные значения аргументов командной строки.\n");
        return 1;
    }

    printf("Количество добавляющих потоков: %d\n", add_threads_count);
    printf("Количество удаляющих потоков: %d\n", del_threads_count);
    printf("Максимальный размер дерева: %d\n", max_size);
    printf("Диапазон случайных чисел: от %d до %d\n", min_value, max_value);

    srand(time(NULL));

    // Инициализация общей памяти
    current_size = mmap(NULL, sizeof *current_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);//Первый параметр указывает на начальный адрес отображения(NULL - система выберет адрес сама);*sizeof current_size - размер выделяемой памяти (равен current_size); PROT_READ | PROT_WRITE - указывают, что память будет доступна для чтения и записи;MAP_SHARED позволяет отображённой памяти быть доступной для других процессов;MAP_ANONYMOUS указывает, что память не связана с каким-либо файлом, и её содержимое инициализируется в ноль; 5 - указывает на файл; последний - смещение в файле
    if (current_size == MAP_FAILED) {
        perror("mmap не удалось выполнить current_size");
        exit(EXIT_FAILURE);
    }

    added_elements = mmap(NULL, sizeof *added_elements, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (added_elements == MAP_FAILED) {
        perror("mmap не удалось выполнить added_elements");
        exit(EXIT_FAILURE);
    }

    //иниализируем нулем
    *current_size = 0;
    *added_elements = 0;

    // Динамическое создание стеков для потоков
    char **add_thread_stacks = malloc(add_threads_count * sizeof(char*));//это указатель на указатель char, который будет использоваться для хранения адресов стеков потоков
    pid_t *add_thread_pids = malloc(add_threads_count * sizeof(pid_t)); //идентификаторов процессов (PID) для добавляемых потоков

    char **del_thread_stacks = malloc(del_threads_count * sizeof(char*));
    pid_t *del_thread_pids = malloc(del_threads_count * sizeof(pid_t));

    if (!add_thread_stacks || !add_thread_pids || !del_thread_stacks || !del_thread_pids) {
        perror("malloc не удалось выполнить thread stacks or pids");
        exit(EXIT_FAILURE);
    }

    // Создание стеков для потоков
    for (int i = 0; i < add_threads_count; i++) {
        add_thread_stacks[i] = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (add_thread_stacks[i] == MAP_FAILED) {
            perror("mmap не удалось выполнить add_thread_stack");
            exit(EXIT_FAILURE);
        }
    }

    for (int i = 0; i < del_threads_count; i++) {
        del_thread_stacks[i] = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (del_thread_stacks[i] == MAP_FAILED) {
            perror("mmap не удалось выполнить del_thread_stack");
            exit(EXIT_FAILURE);
        }
    }

    // Стек для потока мониторинга
    char *monitor_thread_stack = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (monitor_thread_stack == MAP_FAILED) {
        perror("mmap не удалось выполнить monitor_thread_stack");
        exit(EXIT_FAILURE);
    }

    // Флаги для clone()
    int clone_flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | SIGCHLD;//CLONE_VM - указывает, что новый поток будет разделять с родительским процессом одно и то же адресное пространство памяти. Это значит, что изменения, сделанные в памяти одним потоком, будут видны другим потокам; CLONE_FS - позволяет новому потоку разделять информацию о файловой системе с родительским процессом. Это включает в себя рабочую директорию и корневую файловую систему;CLONE_FILES - указывает, что новый процесс будет разделять таблицу открытых файлов с родительским процессом. Это означает, что если один из потоков закроет файл, он будет закрыт и для других потоков; CLONE_SIGHAND - позволяет новому потоку разделять обработчики сигналов с родительским процессом. Это значит, что изменения в обработчиках сигналов будут видны для обоих; CLONE_THREAD - указывает, что новый процесс следует рассматривать как поток в рамках одного процесса. Это полезно для создания легковесных потоков, которые могут быть использованы в многопоточных приложениях; SIGCHLD - этот флаг указывает, что родительский процесс получит сигнал SIGCHLD, когда дочерний процесс завершит выполнение. Это используется для обработки завершения дочерних процессов.

    // Запуск добавляющих потоков
    for (int i = 0; i < add_threads_count; i++) {
        add_thread_pids[i] = clone(add_thread_func, add_thread_stacks[i] + STACK_SIZE, clone_flags, NULL);//функция, которая будет выполняться в потоке; указатель на вершину стека, который был выделен ранее для этого потока. Так как стекирастут вниз,передаём адрес конца стека, чтобы корректно использовать его, затем флаги и аргументы для функции
        if (add_thread_pids[i] == -1) {
            perror("clone не удалось выполнить add_thread");
            exit(EXIT_FAILURE);
        }
    }

    // Запуск удаляющих потоков
    for (int i = 0; i < del_threads_count; i++) {
        del_thread_pids[i] = clone(del_thread_func, del_thread_stacks[i] + STACK_SIZE, clone_flags, NULL);
        if (del_thread_pids[i] == -1) {
            perror("clone failed for del_thread");
            exit(EXIT_FAILURE);
        }
    }

    // Запуск потока мониторинга
    pid_t monitor_thread_pid = clone(monitor_thread_func, monitor_thread_stack + STACK_SIZE, clone_flags, NULL);
    if (monitor_thread_pid == -1) {
        perror("clone failed for monitor_thread");
        exit(EXIT_FAILURE);
    }

    // Ожидание ввода пользователя
    getchar();//ждет введения любого символа
    stop_flag = 1;

    // Ожидание завершения потоков
    for (int i = 0; i < add_threads_count; i++) {
        waitpid(add_thread_pids[i], NULL, __WCLONE);//функция waitpid ожидает завершения дочерних потоков, которые были созданы с помощью clone; NULL - указывает, что мы не хотите получать информацию о статусе завершения; __WCLONE - этот флаг указывает, что мы хотите ожидать завершения дочернего потока, созданного с использованием clone с флагом CLONE_THREAD. Это необходимо для корректного ожидания потоков, поскольку они разделяют адресное пространство с родительским процессом
    }
    for (int i = 0; i < del_threads_count; i++) {
        waitpid(del_thread_pids[i], NULL, __WCLONE);
    }
    waitpid(monitor_thread_pid, NULL, __WCLONE);

     // Проверка целостности дерева перед завершением программы
    if (check_tree_integrity(tree)) {
        printf("Программа завершена успешно.\n");
    } else {
        printf("Программа завершена с ошибками в дереве.\n");
    }

    printf("Программа завершена.\n");

    // Освобождение памяти
    for (int i = 0; i < add_threads_count; i++) {
        munmap(add_thread_stacks[i], STACK_SIZE);
    }
    for (int i = 0; i < del_threads_count; i++) {
        munmap(del_thread_stacks[i], STACK_SIZE);//начало и размер
    }
    munmap(monitor_thread_stack, STACK_SIZE);
    //конечное освобождение памяти
    free(add_thread_stacks);
    free(add_thread_pids);
    free(del_thread_stacks);
    free(del_thread_pids);

    munmap(current_size, sizeof *current_size);
    munmap(added_elements, sizeof *added_elements);

    return 0;
}
