// Точка входа: подготовка процесса, разбор аргументов командной строки,
// создание ядра и окна. Портативный exe без зависимостей — как и в Python-версии,
// только вместо интерпретатора здесь статически слинкованный код.
#include <windows.h>

#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <exception>
#include <memory>
#include <string>

#include "app_win32.h"
#include "common.h"
#include "player_core.h"

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 (reinterpret_cast<HANDLE>(-4))
#endif

namespace {

typedef BOOL(WINAPI* SetProcessDpiAwarenessContextProc)(HANDLE);
typedef HRESULT(WINAPI* SetProcessDpiAwarenessProc)(int);

// Не размывать интерфейс на экранах с масштабированием.
// Порядок как в Windows-документации: сначала самое новое API.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
void setup_dpi_awareness() {
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        auto set_context = reinterpret_cast<SetProcessDpiAwarenessContextProc>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (set_context != nullptr &&
            set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE) {
            return;
        }
    }
    const HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore != nullptr) {
        auto set_awareness = reinterpret_cast<SetProcessDpiAwarenessProc>(
            GetProcAddress(shcore, "SetProcessDpiAwareness"));
        if (set_awareness != nullptr && set_awareness(2 /* PROCESS_PER_MONITOR_DPI_AWARE */) == S_OK) {
            return;
        }
    }
    SetProcessDPIAware();
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

void fatal(const wchar_t* title, const std::wstring& text) {
    MessageBoxW(nullptr, text.c_str(), title, MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    // Своя группа в панели задач (иначе там «AudioPlayer.exe» без значка).
    SetCurrentProcessExplicitAppUserModelID(L"simple.audioplayer.2");
    setup_dpi_awareness();
    // Диалог выбора папки (IFileOpenDialog) требует COM.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::string path_from_arguments;
    int argument_count = 0;
    if (LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count)) {
        if (argument_count > 1 && arguments[1] != nullptr) {
            path_from_arguments = app::wide_to_utf8(arguments[1]);
        }
        LocalFree(arguments);
    }

    std::unique_ptr<PlayerCore> core;
    try {
        core = std::make_unique<PlayerCore>();
    } catch (const std::exception& error) {
        fatal(L"Нет доступа к звуковому устройству",
              std::wstring(L"Не удалось инициализировать звук:\n") +
                  app::utf8_to_wide(error.what()) +
                  L"\n\nПроверьте, что в системе есть работающее устройство вывода звука.");
        if (SUCCEEDED(com)) {
            CoUninitialize();
        }
        return 1;
    }

    PlayerApp player(*core);
    if (!player.create(instance)) {
        fatal(L"Не удалось создать окно",
              L"RegisterClassEx/CreateWindow завершились ошибкой.");
        if (SUCCEEDED(com)) {
            CoUninitialize();
        }
        return 1;
    }
    player.show();
    if (!path_from_arguments.empty()) {
        player.open_from_command_line(path_from_arguments);
    }
    player.run();

    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
    return 0;
}
