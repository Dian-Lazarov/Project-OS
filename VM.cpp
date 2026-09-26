#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include <iomanip>

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

    string out_buf = ""; // Буфер за незавършен ред
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
            cerr << "Грешка: Файлът " << filename << " не може да бъде отворен!" << "\n";
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

// ==========================================================
// Цикъл извличане-декодиране-изпълнение на инструкция
// ==========================================================
class CPU {
public:
    // Функция за проверка дали инструкцията изисква операнд
    static bool HasOperand(int32_t op) {
        return (op >= 0x11 && op <= 0x14) || // LDA, STA, LDS, STS
            (op >= 0x20 && op <= 0x24) || // ADD, SUB, MUL, DIV, MOD
            (op >= 0x30 && op <= 0x37) || // EQ, NE, LT, LE, GT, GE, AND, OR
            (op >= 0x40 && op <= 0x42) || // JMP, JZ, JNZ
            (op == 0x60 || op == 0x62 || op == 0x63); // SEMINIT, WAIT, SIGNAL
    }

    // Изпълнение на една единствена инструкция
    static bool ExecOne(PCB& pcb) {
        // 1. Извличане (Fetch): Проверка дали PC е в рамките на процеса
        if (pcb.pc < pcb.base || pcb.pc > pcb.limit) {
            cerr << "[VM] Грешка при изпълнение: PC (" << pcb.pc << ") е извън блока на процеса!\n";
            pcb.state = PS_DONE;
            return false;
        }

        int32_t op = Memory[pcb.pc++];
        pcb.instr_count++;

        // 2. Декодиране (Decode): Извличане на операнд, ако е необходим
        int32_t opd = 0;
        if (HasOperand(op)) {
            if (pcb.pc > pcb.limit) {
                cerr << "[VM] Грешка при изпълнение: Липсващ операнд в края на блока!\n";
                pcb.state = PS_DONE;
                return false;
            }
            opd = Memory[pcb.pc++];
        }

        // Помощна функция за проверка на валиден адрес за данни
        auto getValidAddress = [&](int32_t offset) -> int32_t {
            int32_t abs_addr = pcb.base + offset;
            if (abs_addr < pcb.base || abs_addr > pcb.limit) {
                cerr << "[VM] Грешка при изпълнение: Адрес извън блока (offset " << offset << ")!\n";
                return -1;
            }
            return abs_addr;
            };

        // 3. Изпълнение (Execute)
        int32_t addr, val;

        switch (op) {
        case 0x00: // HALT
            pcb.state = PS_DONE;
            return false;

            // Работа с локална памет
        case 0x11: // LDA
            if ((addr = getValidAddress(opd)) == -1) return false;
            pcb.acc = Memory[addr];
            break;
        case 0x12: // STA
            if ((addr = getValidAddress(opd)) == -1) return false;
            Memory[addr] = pcb.acc;
            break;

            // Аритметични операции
        case 0x20: // ADD
            if ((addr = getValidAddress(opd)) == -1) return false;
            pcb.acc += Memory[addr];
            break;
        case 0x21: // SUB
            if ((addr = getValidAddress(opd)) == -1) return false;
            pcb.acc -= Memory[addr];
            break;
        case 0x22: // MUL
            if ((addr = getValidAddress(opd)) == -1) return false;
            pcb.acc *= Memory[addr];
            break;
        case 0x23: // DIV
            if ((addr = getValidAddress(opd)) == -1) return false;
            val = Memory[addr];
            if (val == 0) { cerr << "[VM] Грешка: Деление на нула!\n"; pcb.state = PS_DONE; return false; }
            pcb.acc /= val;
            break;
        case 0x24: // MOD
            if ((addr = getValidAddress(opd)) == -1) return false;
            val = Memory[addr];
            if (val == 0) { cerr << "[VM] Грешка: Остатък при делител нула!\n"; pcb.state = PS_DONE; return false; }
            pcb.acc %= val;
            break;
        case 0x25: // NEG
            pcb.acc = -pcb.acc;
            break;

            // Сравнения (Резултатът е 1 при истина и 0 при лъжа)
        case 0x30: // EQ
            if ((addr = getValidAddress(opd)) == -1) return false;
            pcb.acc = (pcb.acc == Memory[addr]) ? 1 : 0;
            break;
        case 0x31: // NE
            if ((addr = getValidAddress(opd)) == -1) return false;
            pcb.acc = (pcb.acc != Memory[addr]) ? 1 : 0;
            break;
        case 0x32: // LT
            if ((addr = getValidAddress(opd)) == -1) return false;
            pcb.acc = (pcb.acc < Memory[addr]) ? 1 : 0;
            break;
        case 0x33: // LE
            if ((addr = getValidAddress(opd)) == -1) return false;
            pcb.acc = (pcb.acc <= Memory[addr]) ? 1 : 0;
            break;
        case 0x34: // GT
            if ((addr = getValidAddress(opd)) == -1) return false;
            pcb.acc = (pcb.acc > Memory[addr]) ? 1 : 0;
            break;
        case 0x35: // GE
            if ((addr = getValidAddress(opd)) == -1) return false;
            pcb.acc = (pcb.acc >= Memory[addr]) ? 1 : 0;
            break;

            // Логически операции
        case 0x36: // AND (Логическо И, не побитово)
            if ((addr = getValidAddress(opd)) == -1) return false;
            pcb.acc = ((pcb.acc != 0) && (Memory[addr] != 0)) ? 1 : 0;
            break;
        case 0x37: // OR (Логическо ИЛИ, не побитово)
            if ((addr = getValidAddress(opd)) == -1) return false;
            pcb.acc = ((pcb.acc != 0) || (Memory[addr] != 0)) ? 1 : 0;
            break;
        case 0x38: // NOT
            pcb.acc = (pcb.acc == 0) ? 1 : 0;
            break;

            // Преходи (Операндът m е отместване спрямо началото на модула)
        case 0x40: // JMP
            pcb.pc = pcb.base + opd;
            break;
        case 0x41: // JZ (Преход, ако ACC == 0)
            if (pcb.acc == 0) pcb.pc = pcb.base + opd;
            break;
        case 0x42: // JNZ (Преход, ако ACC != 0)
            if (pcb.acc != 0) pcb.pc = pcb.base + opd;
            break;

            // Вход / Изход
        case 0x51: // OUT (Извежда десетично число)
            pcb.out_buf += to_string(pcb.acc);
            break;
        case 0x52: // OUTC (Извежда ASCII символ)
            if (pcb.acc == 10 || pcb.acc == 13) {
                // Код 10 (или 13) затваря реда и го извежда
                cout << "[" << pcb.pid << "] " << pcb.out_buf << "\n";
                pcb.out_buf = "";
            }
            else if (pcb.acc >= 32 && pcb.acc < 127) {
                // Печат на четими ASCII символи
                pcb.out_buf += static_cast<char>(pcb.acc);
            }
            else {
                // За нечетими символи
                pcb.out_buf += "?";
            }
            break;

        default:
            cerr << "[VM] Грешка при изпълнение: Невалиден код на операция (0x"
                << hex << op << dec << ").\n";
            pcb.state = PS_DONE;
            return false;
        }
        return true;
    }

    // Изпълнява инструкции, докато процесът не завърши (Ниво A)
    static void RunProcess(PCB& pcb) {
        pcb.state = PS_RUNNING;
        while (pcb.state == PS_RUNNING) {
            if (!ExecOne(pcb)) {
                break;
            }
        }

        // Ако има останал текст в буфера при завършване на програмата
        if (!pcb.out_buf.empty()) {
            cout << "[" << pcb.pid << "] " << pcb.out_buf << "\n";
            pcb.out_buf = "";
        }

        cout << "[VM] [" << pcb.pid << "] завършен, изпълнени " << pcb.instr_count << " инструкции.\n";
    }
};

int main(int argc, char* argv[]) {
    system("chcp 1251 > nul");

    if (argc < 2) {
        cerr << "Употреба: " << argv[0] << " <module.vmx>\n";
        return 1;
    }

    string filename = argv[1];
    PCB process1;
    process1.pid = 1;
    process1.instr_count = 0; // Инициализация

    if (Loader::LoadModule(filename, process1)) {
        if (Loader::AllocateAndStart(process1)) {
            // Стартираме главния цикъл за изпълнение
            CPU::RunProcess(process1);
        }
    }

    return 0;
}