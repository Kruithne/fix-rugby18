#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <conio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define PROCESS_NAME L"Rugby18.exe"
#define LAUNCH_URL L"steam://rungameid/627450"

#define PRINT_SIZE 2048
#define POLL_INTERVAL_MS 500
#define LAUNCH_TIMEOUT_MS 120000
#define DECRYPT_TIMEOUT_MS 60000
#define MAX_SECTIONS 96

#define PATCH_OFFSET 15

static const uint8_t ORIGINAL_CODE[] = {
	0x48, 0x8B, 0xC6, 0x44, 0x3B, 0x84, 0xB2, 0xD4, 0x17, 0x17, 0x00, 0x7F, 0x12,
	0x8D, 0x46, 0x01, 0x48, 0x8B, 0x74, 0x24, 0x38, 0x3B, 0xC1, 0x0F, 0x94, 0xC0
};

static const uint8_t PATCHED_CODE[] = {
	0x48, 0x8B, 0xC6, 0x44, 0x3B, 0x84, 0xB2, 0xD4, 0x17, 0x17, 0x00, 0x7F, 0x12,
	0x8D, 0x46, 0x02, 0x48, 0x8B, 0x74, 0x24, 0x38, 0x3B, 0xC1, 0x0F, 0x94, 0xC0
};

typedef enum {
	SCAN_NOT_FOUND,
	SCAN_ORIGINAL,
	SCAN_PATCHED,
	SCAN_ERROR
} ScanResult;

static void print(const wchar_t *format, ...)
{
	wchar_t text[PRINT_SIZE];
	va_list args;

	va_start(args, format);
	int length = vswprintf(text, PRINT_SIZE, format, args);
	va_end(args);

	if (length < 0)
		return;

	HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
	DWORD written;

	if (WriteConsoleW(output, text, (DWORD)length, &written, NULL))
		return;

	char bytes[PRINT_SIZE * 3];
	int size = WideCharToMultiByte(CP_UTF8, 0, text, length, bytes, sizeof(bytes), NULL, NULL);
	WriteFile(output, bytes, (DWORD)size, &written, NULL);
}

static DWORD find_process(void)
{
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE)
		return 0;

	PROCESSENTRY32W entry = { .dwSize = sizeof(entry) };
	DWORD pid = 0;

	for (BOOL more = Process32FirstW(snapshot, &entry); more && !pid; more = Process32NextW(snapshot, &entry)) {
		if (_wcsicmp(entry.szExeFile, PROCESS_NAME) == 0)
			pid = entry.th32ProcessID;
	}

	CloseHandle(snapshot);
	return pid;
}

static DWORD wait_for_process(DWORD timeout)
{
	for (DWORD elapsed = 0; elapsed < timeout; elapsed += POLL_INTERVAL_MS) {
		DWORD pid = find_process();
		if (pid)
			return pid;

		Sleep(POLL_INTERVAL_MS);
	}

	return 0;
}

static uintptr_t find_module_base(DWORD pid)
{
	for (DWORD elapsed = 0; elapsed < DECRYPT_TIMEOUT_MS; elapsed += POLL_INTERVAL_MS) {
		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);

		if (snapshot != INVALID_HANDLE_VALUE) {
			MODULEENTRY32W entry = { .dwSize = sizeof(entry) };
			uintptr_t base = 0;

			if (Module32FirstW(snapshot, &entry))
				base = (uintptr_t)entry.modBaseAddr;

			CloseHandle(snapshot);

			if (base)
				return base;
		}

		Sleep(POLL_INTERVAL_MS);
	}

	return 0;
}

static bool read_memory(HANDLE process, uintptr_t address, void *buffer, size_t size)
{
	SIZE_T read = 0;
	return ReadProcessMemory(process, (LPCVOID)address, buffer, size, &read) && read == size;
}

static bool find_code_section(HANDLE process, uintptr_t base, uintptr_t *start, DWORD *size)
{
	IMAGE_DOS_HEADER dos;
	if (!read_memory(process, base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE)
		return false;

	IMAGE_NT_HEADERS64 nt;
	uintptr_t nt_address = base + dos.e_lfanew;
	if (!read_memory(process, nt_address, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE)
		return false;

	WORD count = nt.FileHeader.NumberOfSections;
	if (count == 0 || count > MAX_SECTIONS)
		return false;

	IMAGE_SECTION_HEADER sections[MAX_SECTIONS];
	uintptr_t sections_address = nt_address + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
	if (!read_memory(process, sections_address, sections, count * sizeof(IMAGE_SECTION_HEADER)))
		return false;

	for (WORD i = 0; i < count; i++) {
		if (memcmp(sections[i].Name, ".text", 6) != 0)
			continue;

		*start = base + sections[i].VirtualAddress;
		*size = sections[i].Misc.VirtualSize;
		return true;
	}

	return false;
}

static ScanResult scan(HANDLE process, uintptr_t start, DWORD size, uint8_t *buffer, uintptr_t *match)
{
	if (!read_memory(process, start, buffer, size))
		return SCAN_NOT_FOUND;

	DWORD pattern_size = sizeof(ORIGINAL_CODE);
	ScanResult result = SCAN_NOT_FOUND;
	int matches = 0;

	for (DWORD pos = 0; pos + pattern_size <= size; pos++) {
		if (buffer[pos] != ORIGINAL_CODE[0])
			continue;

		if (memcmp(buffer + pos, ORIGINAL_CODE, pattern_size) == 0)
			result = SCAN_ORIGINAL;
		else if (memcmp(buffer + pos, PATCHED_CODE, pattern_size) == 0)
			result = SCAN_PATCHED;
		else
			continue;

		*match = start + pos;
		matches++;
	}

	if (matches > 1)
		return SCAN_ERROR;

	return result;
}

static ScanResult wait_for_code(HANDLE process, uintptr_t start, DWORD size, uintptr_t *match)
{
	uint8_t *buffer = malloc(size);
	if (!buffer)
		return SCAN_ERROR;

	ScanResult previous = SCAN_NOT_FOUND;
	ScanResult result = SCAN_NOT_FOUND;

	for (DWORD elapsed = 0; elapsed < DECRYPT_TIMEOUT_MS; elapsed += POLL_INTERVAL_MS) {
		if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
			result = SCAN_NOT_FOUND;
			break;
		}

		result = scan(process, start, size, buffer, match);
		if (result == SCAN_ERROR || (result != SCAN_NOT_FOUND && result == previous))
			break;

		previous = result;
		Sleep(POLL_INTERVAL_MS);
	}

	free(buffer);
	return result;
}

static bool apply_patch(HANDLE process, uintptr_t match)
{
	uintptr_t address = match + PATCH_OFFSET;
	uint8_t value = PATCHED_CODE[PATCH_OFFSET];
	DWORD old_protect;

	if (!VirtualProtectEx(process, (LPVOID)address, 1, PAGE_EXECUTE_READWRITE, &old_protect))
		return false;

	SIZE_T written = 0;
	bool success = WriteProcessMemory(process, (LPVOID)address, &value, 1, &written) && written == 1;

	VirtualProtectEx(process, (LPVOID)address, 1, old_protect, &old_protect);
	FlushInstructionCache(process, (LPCVOID)address, 1);

	uint8_t check = 0;
	return success && read_memory(process, address, &check, 1) && check == value;
}

static int finish(int code)
{
	print(L"\nPress a key to close.\n");
	_getwch();
	return code;
}

int wmain(void)
{
	print(L"fix-rugby18: fixes the \"We've Made It\" achievement in the running Rugby 18 game.\n\n");

	DWORD pid = find_process();
	if (!pid) {
		print(L"The game is not running. Starting it through Steam...\n");
		ShellExecuteW(NULL, L"open", LAUNCH_URL, NULL, NULL, SW_SHOWNORMAL);

		pid = wait_for_process(LAUNCH_TIMEOUT_MS);
		if (!pid) {
			print(L"The game did not start. Start the game, then run this program again.\n");
			return finish(1);
		}
	}

	HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | SYNCHRONIZE, FALSE, pid);
	if (!process) {
		print(L"Could not open the game process (error %lu).\n", GetLastError());
		return finish(1);
	}

	uintptr_t base = find_module_base(pid);
	uintptr_t code_start;
	DWORD code_size;

	if (!base || !find_code_section(process, base, &code_start, &code_size)) {
		print(L"Could not read the game process.\n");
		CloseHandle(process);
		return finish(1);
	}

	print(L"Waiting for the game code...\n");

	uintptr_t match = 0;
	ScanResult result = wait_for_code(process, code_start, code_size, &match);
	int code = 1;

	if (result == SCAN_PATCHED) {
		print(L"The game is already fixed for this session.\n");
		code = 0;
	} else if (result == SCAN_ORIGINAL && apply_patch(process, match)) {
		print(L"Fixed at %ls+0x%llX. Keep the game open and play your career.\n", PROCESS_NAME, (unsigned long long)(match + PATCH_OFFSET - base));
		code = 0;
	} else if (result == SCAN_ORIGINAL) {
		print(L"Could not write to the game process (error %lu).\n", GetLastError());
	} else {
		print(L"Could not find the achievement code. The game version is possibly different.\n");
	}

	CloseHandle(process);
	return finish(code);
}
