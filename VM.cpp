#define NOMINMAX
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <windows.h>

using namespace std;

// ==========================================
// Структури за паметта и процесите
// ==========================================

const int32_t MEM_SIZE = 2048; // Лимит на потребителската памет в думи
const int32_t MAX_EXT = 32;    // Максимален брой външни имена
const int32_t USER_RING = 400; // Кръгов буфер за потребителската конзола
const int32_t SYS_RING = 20;   // Буфер за системната конзола
const int32_t SYS_DEFAULT = 3; // Видими системни редове по подразбиране

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
	int32_t pid = 1;
	string module_name;
	ProcessState state = PS_NEW;

	// Съхранен контекст
	int32_t pc = 0, acc = 0, base = 0, limit = 0;

	// Информация за изпълнимия модул
	int32_t image_size = 0, entry = 0;

	// За планировчика
	int32_t priority = 1;
	int32_t est_time = 1000000;
	int32_t instr_count = 0;

	vector<Xlat> xlat;
	vector<int32_t> image; // Временно пази образеца преди заемане на памет

	string out_buf = ""; // Буфер за незавършен ред
};

// Глобална памет (модел на реалната памет)
vector<int32_t> Memory(MEM_SIZE, 0);

// =================================================================
// Управление на двете конзоли (системна + потребителска)
// =================================================================

class Console {
public:
	static HANDLE hOut;
	static HANDLE hIn;
	static WORD def_attr;
	static int org_y;
	static bool is_console;

	static int scr_w;
	static int scr_h;
	static int sys_rows;
	static int user_bottom;

	static vector<string> u_ring;
	static int u_head;
	static vector<string> sys_lines;
	static string line_buf;
	static bool paused;

	// Инициализация и проверка какво поддържа средата
	static void Init() {
		hOut = GetStdHandle(STD_OUTPUT_HANDLE);
		hIn = GetStdHandle(STD_INPUT_HANDLE);
		def_attr = 7;
		org_y = 0;

		CONSOLE_SCREEN_BUFFER_INFO bi;
		// Проверка дали средата поддържа конзолен екран
		if (GetConsoleScreenBufferInfo(hOut, &bi)) {
			is_console = true;
			def_attr = bi.wAttributes;

			// Изравняваме размера на буфера с прозореца, за да съвпадат координатите
			COORD sz;
			sz.X = bi.srWindow.Right - bi.srWindow.Left + 1;
			sz.Y = bi.srWindow.Bottom - bi.srWindow.Top + 1;
			SetConsoleScreenBufferSize(hOut, sz);

			if (GetConsoleScreenBufferInfo(hOut, &bi)) {
				org_y = bi.srWindow.Top;
			}
		}
		else {
			is_console = false;
		}

		QueryScreenSize();
		user_bottom = scr_h - sys_rows - 2;
		if (user_bottom < 3) user_bottom = 3;

		u_head = 0;
		for (int i = 0; i < SYS_RING; ++i) sys_lines[i] = "";

		ScrClearAll();
		RepaintUser();
		DrawSeparator();
		DrawSysArea();
		DrawPrompt();
	}

	static void Done() {
		if (is_console) {
			ScrGoto(0, scr_h - 1);
		}
		cout << "\n";
	}

	static void QueryScreenSize() {
		scr_w = 80;
		scr_h = 25;
		if (!is_console) return;

		CONSOLE_SCREEN_BUFFER_INFO bi;
		if (GetConsoleScreenBufferInfo(hOut, &bi)) {
			scr_w = bi.srWindow.Right - bi.srWindow.Left + 1;
			scr_h = bi.srWindow.Bottom - bi.srWindow.Top + 1;
			org_y = bi.srWindow.Top;
		}
	}

	static void ScrGoto(int x, int y) {
		if (!is_console) return;
		COORD c = { static_cast<SHORT>(x), static_cast<SHORT>(org_y + y) };
		SetConsoleCursorPosition(hOut, c);
	}

	static void ScrClearLine(int y) {
		if (!is_console) return;
		COORD c = { 0, static_cast<SHORT>(org_y + y) };
		DWORD n;
		FillConsoleOutputCharacterA(hOut, ' ', scr_w, c, &n);
		FillConsoleOutputAttribute(hOut, def_attr, scr_w, c, &n);
	}

	static void ScrClearAll() {
		if (!is_console) return;
		COORD c = { 0, static_cast<SHORT>(org_y) };
		DWORD n;
		FillConsoleOutputCharacterA(hOut, ' ', scr_w * scr_h, c, &n);
		FillConsoleOutputAttribute(hOut, def_attr, scr_w * scr_h, c, &n);
	}

	static void ScrPut(int x, int y, const string& s) {
		if (!is_console) return;
		if (x < 0 || x >= scr_w || y < 0 || y >= scr_h) return;
		ScrGoto(x, y);
		int max_len = scr_w - x - 1;
		if (max_len > 0) {
			cout << s.substr(0, max_len);
			cout.flush();
		}
	}

	static void DrawPrompt() {
		if (!is_console) return;
		ScrClearLine(scr_h - 1);
		ScrPut(0, scr_h - 1, "> " + line_buf);
	}

	static void DrawSysArea() {
		if (!is_console) return;
		for (int i = 0; i < sys_rows; ++i) {
			ScrClearLine(user_bottom + 1 + i);
			ScrPut(0, user_bottom + 1 + i, sys_lines[SYS_RING - sys_rows + i]);
		}
	}

	static void DrawSeparator() {
		if (!is_console) return;
		string bar;
		if (paused && scr_w > 24) {
			bar = "--[ ПАУЗА ]" + string(scr_w - 12, '-');
		}
		else {
			bar = string(scr_w - 1, '-');
		}
		ScrClearLine(user_bottom);
		ScrPut(0, user_bottom, bar);
	}

	static void RepaintUser() {
		if (!is_console) return;
		int first = (u_head > user_bottom) ? (u_head - user_bottom) : 0;
		for (int i = 0; i < user_bottom; ++i) {
			ScrClearLine(i);
			if (first + i < u_head) {
				ScrPut(0, i, u_ring[(first + i) % USER_RING]);
			}
		}
	}

	// Извежда ред в потребителската конзола
	static void UserLine(const string& s) {
		if (!is_console) {
			cout << s << "\n";
			return;
		}
		u_ring[u_head % USER_RING] = s;
		u_head++;
		RepaintUser();
		DrawPrompt();
	}

	// Извежда съобщение в системната конзола с префикс [VM]
	static void SysMsg(const string& s) {
		if (!is_console) {
			cout << "[VM] " << s << "\n";
			return;
		}
		for (int i = 0; i <= SYS_RING - 2; ++i) {
			sys_lines[i] = sys_lines[i + 1];
		}
		sys_lines[SYS_RING - 1] = "[VM] " + s;
		DrawSysArea();
		DrawPrompt();
	}

	// Неблокиращо четене на символ от клавиатурата
	static bool ReadCharNB(char& c) {
		c = 0;
		if (!is_console) return false;
		INPUT_RECORD ir;
		DWORD n = 0;
		while (PeekConsoleInputA(hIn, &ir, 1, &n) && n > 0) {
			if (!ReadConsoleInputA(hIn, &ir, 1, &n)) return false;
			if (ir.EventType == KEY_EVENT && ir.Event.KeyEvent.bKeyDown) {
				char ch = ir.Event.KeyEvent.uChar.AsciiChar;
				if (ch != 0) {
					c = ch;
					return true;
				}
			}
		}
		return false;
	}

	// Четене на цял ред от командния промпт без нарушаване на екрана
	static string ReadLineBlocking() {
		if (!is_console) {
			string s;
			getline(cin, s);
			return s;
		}
		line_buf = "";
		DrawPrompt();
		while (true) {
			char c;
			while (ReadCharNB(c)) {
				if (c == '\r' || c == '\n') {
					string res = line_buf;
					line_buf = "";
					DrawPrompt();
					return res;
				}
				else if (c == '\b' || c == 127) {
					if (!line_buf.empty()) line_buf.pop_back();
					DrawPrompt();
				}
				else if (c == 27) { // ESC изчиства реда
					line_buf = "";
					DrawPrompt();
				}
				else if (static_cast<unsigned char>(c) >= 32) {
					line_buf += c;
					DrawPrompt();
				}
			}
			Sleep(10);
		}
	}
};

// Дефиниция на статичните променливи на класа Console
HANDLE Console::hOut = INVALID_HANDLE_VALUE;
HANDLE Console::hIn = INVALID_HANDLE_VALUE;
WORD Console::def_attr = 7;
int Console::org_y = 0;
bool Console::is_console = false;
int Console::scr_w = 80;
int Console::scr_h = 25;
int Console::sys_rows = SYS_DEFAULT;
int Console::user_bottom = 20;
vector<string> Console::u_ring(USER_RING, "");
int Console::u_head = 0;
vector<string> Console::sys_lines(SYS_RING, "");
string Console::line_buf = "";
bool Console::paused = false;

// ==========================================
// Loader и зареждане в паметта
// ==========================================

class Loader {
public:
	// Функция за четене на .vmx модул и валидация
	static bool LoadModule(const string& filename, PCB& pcb) {
		ifstream file(filename, ios::binary);
		if (!file.is_open()) {
			Console::SysMsg("Грешка: Файлът " + filename + " не може да бъде отворен!");
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
			Console::SysMsg("Грешка: Непълен или повреден хедър.");
			return false;
		}

		// 2. Проверки по т. 4.2
		int32_t expected_magic = 'V' | ('M' << 8) | ('X' << 16) | ('1' << 24);
		if (magic != expected_magic) {
			Console::SysMsg("Грешка: Невалидна сигнатура (очаква се VMX1).");
			return false;
		}

		if (version != 1) {
			Console::SysMsg("Грешка: Неподдържана версия.");
			return false;
		}

		if (image_size <= 0 || image_size > MEM_SIZE) {
			Console::SysMsg("Грешка: Невалиден общ размер на модула: " + to_string(image_size) + " думи.");
			return false;
		}

		if (entry < 0 || entry >= image_size) {
			Console::SysMsg("Грешка: Невалидна входна точка (entry).");
			return false;
		}

		if (ext_count < 0 || ext_count > MAX_EXT) {
			Console::SysMsg("Грешка: Невалиден брой външни имена: " + to_string(ext_count));
			return false;
		}

		// 3. Копиране на образеца на модула (image)
		pcb.image.resize(image_size);
		for (int32_t i = 0; i < image_size; ++i) {
			if (!readWord(pcb.image[i])) {
				Console::SysMsg("Грешка: Файлът е твърде къс (липсват данни за образеца).");
				return false;
			}
		}

		// 4. Четене на таблицата с външни имена
		pcb.xlat.clear();
		for (int32_t k = 0; k < ext_count; ++k) {
			int32_t kind, len;
			if (!readWord(kind) || !readWord(len)) {
				Console::SysMsg("Грешка: Отрязана таблица с външни имена.");
				return false;
			}

			string ext_name = "";
			for (int32_t i = 0; i < len; ++i) {
				int32_t char_word;
				if (!readWord(char_word)) {
					Console::SysMsg("Грешка: Отрязано име в таблицата.");
					return false;
				}
				ext_name += static_cast<char>(char_word & 0xFF);
			}

			// Забележка: В Седмица 12 тук ще се прави реална връзка със системните таблици.
			pcb.xlat.push_back({ (kind == 1) ? EK_SHARED : EK_SEM, 0 });
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
			Console::SysMsg("[" + to_string(pcb.pid) + "] " + pcb.module_name + ": недостатъчна памет - чака");
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

		Console::SysMsg("[" + to_string(pcb.pid) + "] " + pcb.module_name + " стартиран, " +
			to_string(pcb.image_size) + " думи, PC = " + to_string(pcb.pc));

		// Освобождаване на временния буфер
		pcb.image.clear();

		return true;
	}
};

// ==========================================================
// Цикъл извличане-декодиране-изпълнение на инструкция
// ==========================================================
bool trace_on = false; // Глобален флаг за режима на трасиране

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

	// Връща текстовото име на инструкцията
	static string MnemOf(int32_t op) {
		switch (op) {
		case 0x00: return "HALT";   case 0x11: return "LDA";    case 0x12: return "STA";
		case 0x13: return "LDS";    case 0x14: return "STS";    case 0x20: return "ADD";
		case 0x21: return "SUB";    case 0x22: return "MUL";    case 0x23: return "DIV";
		case 0x24: return "MOD";    case 0x25: return "NEG";    case 0x30: return "EQ";
		case 0x31: return "NE";     case 0x32: return "LT";     case 0x33: return "LE";
		case 0x34: return "GT";     case 0x35: return "GE";     case 0x36: return "AND";
		case 0x37: return "OR";     case 0x38: return "NOT";    case 0x40: return "JMP";
		case 0x41: return "JZ";     case 0x42: return "JNZ";    case 0x50: return "IN";
		case 0x51: return "OUT";    case 0x52: return "OUTC";   case 0x60: return "SEMINIT";
		case 0x62: return "WAIT";   case 0x63: return "SIGNAL";
		default: return "???";
		}
	}

	// Функция за обработка на грешки по време на изпълнение
	static void RuntimeError(PCB& pcb, const string& msg, int32_t pc_error) {
		int32_t offset = pc_error - pcb.base;
		if (!pcb.out_buf.empty()) {
			Console::UserLine("[" + to_string(pcb.pid) + "] " + pcb.out_buf);
			pcb.out_buf = "";
		}
		Console::SysMsg("[" + to_string(pcb.pid) + "] Грешка при изпълнение: " + msg + " (Отместване: " + to_string(offset) + ")");

		pcb.state = PS_DONE;
	}

	// Изпълнение на една единствена инструкция
	static bool ExecOne(PCB& pcb) {
		// 1. Извличане (Fetch): Проверка дали PC е в рамките на процеса
		if (pcb.pc < pcb.base || pcb.pc > pcb.limit) {
			RuntimeError(pcb, "PC е извън блока на процеса!", pcb.pc);
			return false;
		}

		// Запомняме адреса на инструкцията за трасирането
		int32_t orig_pc = pcb.pc;
		int32_t op = Memory[pcb.pc++];
		pcb.instr_count++;
		int32_t opd = 0;

		// 2. Декодиране (Decode): Извличане на операнд, ако е необходим
		if (HasOperand(op)) {
			if (pcb.pc > pcb.limit) {
				RuntimeError(pcb, "Липсващ операнд в края на блока!", orig_pc);
				return false;
			}
			opd = Memory[pcb.pc++];
		}

		// Режим на трасиране
		if (trace_on) {
			stringstream ss;
			ss << "[" << pcb.pid << "] trace "
				<< right << setw(4) << (orig_pc - pcb.base) << ": "
				<< left << setw(7) << MnemOf(op) << " "
				<< right << setw(5) << opd
				<< "   ACC=" << pcb.acc;
			Console::UserLine(ss.str());
		}

		// Помощна функция за проверка дали адресът е извън блока на процеса
		auto getValidAddress = [&](int32_t offset) -> int32_t {
			int32_t abs_addr = pcb.base + offset;
			if (abs_addr < pcb.base || abs_addr > pcb.limit) {
				RuntimeError(pcb, "Адресът е извън блока на процеса!", orig_pc);
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

			// Контрол на външните имена (LDS, STS, SEMINIT, WAIT, SIGNAL)
		case 0x13: case 0x14: case 0x60: case 0x62: case 0x63:
			// Проверка: Невалиден индекс на външно име
			if (opd < 0 || opd >= static_cast<int32_t>(pcb.xlat.size())) {
				RuntimeError(pcb, "Невалиден индекс на външно име!", orig_pc);
				return false;
			}

			// Проверка на типовете: LDS и STS са само за общи променливи
			if ((op == 0x13 || op == 0x14) && pcb.xlat[opd].kind != EK_SHARED) {
				RuntimeError(pcb, (op == 0x13) ? "LDS върху семафор!" : "STS върху семафор!", orig_pc);
				return false;
			}

			// Проверка на типовете: SEMINIT, WAIT и SIGNAL са само за семафори
			if ((op == 0x60 || op == 0x62 || op == 0x63) && pcb.xlat[opd].kind != EK_SEM) {
				string m;
				if (op == 0x60) m = "SEMINIT върху променлива!";
				else if (op == 0x62) m = "WAIT върху променлива!";
				else m = "SIGNAL върху променлива!";
				RuntimeError(pcb, m, orig_pc);
				return false;
			}
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
			// Проверка: Деление на нула
			if (val == 0) {
				RuntimeError(pcb, "Деление на нула!", orig_pc);
				return false;
			}
			pcb.acc /= val;
			break;
		case 0x24: // MOD
			if ((addr = getValidAddress(opd)) == -1) return false;
			val = Memory[addr];
			// Проверка: Остатък при делител нула
			if (val == 0) {
				RuntimeError(pcb, "Остатък при делител нула!", orig_pc);
				return false;
			}
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
		case 0x36: // AND
			if ((addr = getValidAddress(opd)) == -1) return false;
			pcb.acc = ((pcb.acc != 0) && (Memory[addr] != 0)) ? 1 : 0;
			break;
		case 0x37: // OR
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
		case 0x50: { // IN
			// Ако имаме текст в буфера (например подкана "Въведи N: "), го извеждаме
			if (!pcb.out_buf.empty()) {
				Console::UserLine("[" + to_string(pcb.pid) + "] " + pcb.out_buf);
				pcb.out_buf = "";
			}
			Console::UserLine("[" + to_string(pcb.pid) + "] Въведете стойност: ");
			Console::SysMsg("[" + to_string(pcb.pid) + "] блокиран по вход");

			string in_str = Console::ReadLineBlocking();
			try {
				pcb.acc = stoi(in_str);
			}
			catch (...) {
				pcb.acc = 0;
			}

			Console::UserLine("[" + to_string(pcb.pid) + "] вход: " + to_string(pcb.acc));
			Console::SysMsg("[" + to_string(pcb.pid) + "] деблокиран след вход");

			if (trace_on) {
				Console::UserLine("      -> Прочетена стойност: " + to_string(pcb.acc));
			}
			break;
		}
		case 0x51: // OUT (Извежда десетично число)
			pcb.out_buf += to_string(pcb.acc);
			break;
		case 0x52: // OUTC (Извежда ASCII символ)
			if (pcb.acc == 10 || pcb.acc == 13) {
				// Код 10 (или 13) затваря реда и го извежда
				Console::UserLine("[" + to_string(pcb.pid) + "] " + pcb.out_buf);
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
			// Проверка: Невалиден код на операция
			RuntimeError(pcb, "Невалиден код на операция: 0x" + [](int32_t code) {
				stringstream stream;
				stream << hex << code;
				return stream.str();
				}(op), orig_pc);
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
			Console::UserLine("[" + to_string(pcb.pid) + "] " + pcb.out_buf);
			pcb.out_buf = "";
		}

		Console::SysMsg("[" + to_string(pcb.pid) + "] завършен, изпълнени " + to_string(pcb.instr_count) + " инструкции.");
	}
};

int main(int argc, char* argv[]) {
	system("chcp 1251 > nul");

	if (argc < 2) {
		cerr << "Употреба: " << argv[0] << " <module.vmx> [-t]\n";
		cerr << "  -t : Включване на режим на трасиране\n";
		return 1;
	}

	string filename = argv[1];

	// Проверка за наличие на флаг -t
	if (argc >= 3) {
		string flag = argv[2];
		if (flag == "-t") {
			trace_on = true;
		}
	}

	Console::Init();
	Console::UserLine("Виртуална машина VM(L) – потребителска конзола");
	Console::UserLine("Изходът на процесите се появява тук, маркиран с техния идентификатор.");
	Console::UserLine("");

	PCB process1;
	process1.pid = 1;
	process1.instr_count = 0; // Инициализация

	if (Loader::LoadModule(filename, process1)) {
		if (Loader::AllocateAndStart(process1)) {
			// Стартираме главния цикъл за изпълнение
			CPU::RunProcess(process1);
		}
	}

	Console::SysMsg("Натиснете Enter за изход...");
	Console::ReadLineBlocking();
	Console::Done();

	return 0;
}