#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
using namespace std;

// ==========================================
// Структури за паметта и процесите
// ==========================================

const int32_t MEM_SIZE = 2048; // Лимит на потребителската памет в думи
const int32_t MAX_EXT = 32;    // Максимален брой външни имена

// Състояния на процеса
enum ProcessState {
    PS_NEW,
    PS_WAIT_MEM,
    PS_READY,
    PS_RUNNING,
    PS_BLOCKED_SEM,
    PS_BLOCKED_IN,
    PS_DONE
};

// Видове външни имена
enum ExtKind {
    EK_SHARED = 1,
    EK_SEM = 2
};

// Вектор за превод на външните имена (xlat)
struct Xlat {
    ExtKind kind;
    int32_t idx; // Индекс в системната таблица
};

// Блок за управление на процес (PCB)
struct PCB {
    int32_t pid;
    string module_name;
    ProcessState state;

    // Съхранен контекст
    int32_t pc;
    int32_t acc;
    int32_t base;
    int32_t limit;

    // Информация за изпълнимия модул
    int32_t image_size;
    int32_t entry;

    // За планировчика
    int32_t priority;
    int32_t est_time;
    int32_t instr_count;

    vector<Xlat> xlat;
    vector<int32_t> image; // Временно пази образеца преди заемане на памет
};

// Глобална памет (модел на реалната памет)
vector<int32_t> Memory(MEM_SIZE, 0);

// ==========================================
// Loader и зареждане в паметта
// ==========================================

class Loader {
public:
    // Функция за четене на .vmx модул и валидация
    static bool LoadModule(const string& filename, PCB& pcb) {
        ifstream file(filename, ios::binary);
        if (!file.is_open()) {
            cerr << "Грешка: Файлът не може да бъде отворен: " << filename << "\n";
            return false;
        }

        // Помощна ламбда функция за четене на една 32-битова дума (little-endian)
        auto readWord = [&file](int32_t& word) -> bool {
            return file.read(reinterpret_cast<char*>(&word), sizeof(int32_t)).good();
            };

        int32_t magic, version, flags, image_size, entry, ext_count;

        // 1. Четене на заглавието (думи 0-5)
        if (!readWord(magic) || !readWord(version) || !readWord(flags) ||
            !readWord(image_size) || !readWord(entry) || !readWord(ext_count)) {
            cerr << "Грешка: Непълен или повреден хедър.\n";
            return false;
        }

        // 2. Проверки по т. 4.2
        int32_t expected_magic = 'V' | ('M' << 8) | ('X' << 16) | ('1' << 24);
        if (magic != expected_magic) {
            cerr << "Грешка: Невалидна сигнатура (очаква се VMX1).\n";
            return false;
        }

        if (version != 1) {
            cerr << "Грешка: Неподдържана версия.\n";
            return false;
        }

        if (image_size <= 0 || image_size > MEM_SIZE) {
            cerr << "Грешка: Невалиден общ размер на модула: " << image_size << " думи.\n";
            return false;
        }

        if (entry < 0 || entry >= image_size) {
            cerr << "Грешка: Невалидна входна точка (entry).\n";
            return false;
        }

        if (ext_count < 0 || ext_count > MAX_EXT) {
            cerr << "Грешка: Невалиден брой външни имена: " << ext_count << "\n";
            return false;
        }

        // 3. Копиране на образеца на модула (image)
        pcb.image.resize(image_size);
        for (int32_t i = 0; i < image_size; ++i) {
            if (!readWord(pcb.image[i])) {
                cerr << "Грешка: Файлът е твърде къс (липсват данни за образеца).\n";
                return false;
            }
        }

        // 4. Четене на таблицата с външни имена
        for (int32_t k = 0; k < ext_count; ++k) {
            int32_t kind, len;
            if (!readWord(kind) || !readWord(len)) {
                cerr << "Грешка: Отрязана таблица с външни имена.\n";
                return false;
            }

            string ext_name = "";
            for (int32_t i = 0; i < len; ++i) {
                int32_t char_word;
                if (!readWord(char_word)) {
                    cerr << "Грешка: Отрязано име в таблицата.\n";
                    return false;
                }
                ext_name += static_cast<char>(char_word & 0xFF);
            }

            // Забележка: В Седмица 12 тук ще се попълва векторът xlat 
            // и ще се прави връзка със системните таблици.
        }

        pcb.image_size = image_size;
        pcb.entry = entry;
        pcb.module_name = filename;

        return true;
    }

    // Симулация на заемане на памет за 1 процес (Ниво А / Седмица 2)
    static bool AllocateAndStart(PCB& pcb) {
        // В Седмица 2 разпределянето на паметта е най-опростено - зареждаме на адрес 0
        int32_t allocated_base = 0;

        if (allocated_base + pcb.image_size > MEM_SIZE) {
            pcb.state = PS_WAIT_MEM;
            return false;
        }

        // Копиране на образеца в реалната памет
        for (int32_t i = 0; i < pcb.image_size; ++i) {
            Memory[allocated_base + i] = pcb.image[i];
        }

        // Инициализиране на регистрите
        pcb.base = allocated_base;
        pcb.limit = allocated_base + pcb.image_size - 1;
        pcb.pc = pcb.base + pcb.entry;
        pcb.acc = 0;
        pcb.state = PS_READY;

        cout << "[VM] " << pcb.module_name << " стартиран, "
            << pcb.image_size << " думи, PC = " << pcb.pc << "\n";

        // Освобождаване на временния буфер
        pcb.image.clear();

        return true;
    }
};

int main(int argc, char* argv[]) {
    system("chcp 1251 > nul");

    // Проверка дали е подаден файл
    if (argc < 2) {
        cerr << "Употреба: " << argv[0] << " <module.vmx>\n";
        return 1;
    }

    string filename = argv[1];
    PCB process1;
    process1.pid = 1;

    cout << "Опит за зареждане на " << filename << "...\n";

    // Тестване на Loader-а
    if (Loader::LoadModule(filename, process1)) {
        cout << "Модулът е прочетен успешно (Размер: " << process1.image_size
            << " думи, Entry: " << process1.entry << ")!\n";

        if (Loader::AllocateAndStart(process1)) {
            cout << "Успех! Модулът е зареден в реалната памет.\n";
            // В Седмица 3 тук ще се извика: CPU::RunSlice(process1);
        }
        else {
            cerr << "Грешка: Недостатъчна памет за стартиране.\n";
        }
    }
    else {
        cerr << "Грешка: Зареждането на файла се провали.\n";
    }

    return 0;
}