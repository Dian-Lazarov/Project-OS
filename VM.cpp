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

int main() {

    return 0;
}