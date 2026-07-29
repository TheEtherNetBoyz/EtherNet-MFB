#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include "dusk/multiplayer/invite_code.hpp"
#include "nlohmann/json.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr int kPublicHostId = 1001;
constexpr int kPortId = 1002;
constexpr int kStartId = 1003;
constexpr int kStopId = 1004;
constexpr int kCopyId = 1005;
constexpr int kCodeId = 1006;
constexpr int kStatusId = 1007;
constexpr int kOpenLogsId = 1008;
constexpr UINT_PTR kProcessTimerId = 1;

HWND gPublicHost = nullptr;
HWND gPort = nullptr;
HWND gStart = nullptr;
HWND gStop = nullptr;
HWND gCode = nullptr;
HWND gStatus = nullptr;
PROCESS_INFORMATION gRelayProcess{};
std::filesystem::path gLogPath;

std::filesystem::path sibling_relay_path();
std::wstring from_utf8(const std::string& value);

std::filesystem::path relay_data_directory() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetEnvironmentVariableW(
        L"APPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        return std::filesystem::path(std::wstring(buffer.data(), length)) /
               L"TwilitRealm" / L"Dusklight" / L"relay";
    }
    return sibling_relay_path().parent_path() / L"relay-data";
}

std::filesystem::path launcher_config_path() {
    return relay_data_directory() / L"launcher.json";
}

void save_launcher_config(const std::string& publicHost, int port) {
    std::error_code error;
    std::filesystem::create_directories(relay_data_directory(), error);
    if (error) {
        return;
    }
    std::ofstream file(launcher_config_path(), std::ios::binary | std::ios::trunc);
    if (!file) {
        return;
    }
    file << nlohmann::json{
        {"public_host", publicHost},
        {"port", port},
    }.dump(2);
}

void load_launcher_config() {
    std::ifstream file(launcher_config_path(), std::ios::binary);
    if (!file) {
        return;
    }
    try {
        const nlohmann::json config = nlohmann::json::parse(file);
        const std::wstring publicHost =
            from_utf8(config.value("public_host", std::string("127.0.0.1")));
        const int port = config.value("port", 34197);
        if (!publicHost.empty()) {
            SetWindowTextW(gPublicHost, publicHost.c_str());
        }
        if (port >= 1 && port <= 65535) {
            SetWindowTextW(gPort, std::to_wstring(port).c_str());
        }
    } catch (const nlohmann::json::exception&) {
    }
}

std::wstring get_text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(control, value.data(), length + 1);
    }
    value.resize(static_cast<size_t>(length));
    return value;
}

std::string to_utf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring from_utf8(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size);
    return result;
}

void set_running(bool running) {
    EnableWindow(gPublicHost, !running);
    EnableWindow(gPort, !running);
    EnableWindow(gStart, !running);
    EnableWindow(gStop, running);
}

void close_process_handles() {
    if (gRelayProcess.hThread != nullptr) {
        CloseHandle(gRelayProcess.hThread);
    }
    if (gRelayProcess.hProcess != nullptr) {
        CloseHandle(gRelayProcess.hProcess);
    }
    gRelayProcess = {};
}

void stop_relay() {
    if (gRelayProcess.hProcess != nullptr) {
        TerminateProcess(gRelayProcess.hProcess, 0);
        WaitForSingleObject(gRelayProcess.hProcess, 2000);
        close_process_handles();
    }
    set_running(false);
    SetWindowTextW(gStatus, L"Stopped");
}

bool parse_port(int& port) {
    const std::wstring text = get_text(gPort);
    wchar_t* end = nullptr;
    const long value = wcstol(text.c_str(), &end, 10);
    if (text.empty() || end == text.c_str() || *end != L'\0' ||
        value < 1 || value > 65535)
    {
        MessageBoxW(nullptr, L"Port must be between 1 and 65535.",
                    L"Invalid relay port", MB_OK | MB_ICONERROR);
        return false;
    }
    port = static_cast<int>(value);
    return true;
}

std::filesystem::path sibling_relay_path() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    std::filesystem::path path(std::wstring(buffer.data(), length));
    return path.parent_path() / L"tp_multiplayer_relay.exe";
}

void start_relay() {
    if (gRelayProcess.hProcess != nullptr) {
        return;
    }

    const std::wstring publicHostWide = get_text(gPublicHost);
    const std::string publicHost = to_utf8(publicHostWide);
    if (publicHost.empty()) {
        MessageBoxW(nullptr, L"Enter the public IP address or hostname supplied by the VPN.",
                    L"Missing public host", MB_OK | MB_ICONERROR);
        return;
    }
    if (publicHostWide.find_first_of(L"\" \t\r\n") != std::wstring::npos) {
        MessageBoxW(nullptr, L"The public host cannot contain spaces or quotes.",
                    L"Invalid public host", MB_OK | MB_ICONERROR);
        return;
    }

    int port = 0;
    if (!parse_port(port)) {
        return;
    }

    const std::filesystem::path relayPath = sibling_relay_path();
    if (!std::filesystem::exists(relayPath)) {
        MessageBoxW(nullptr, L"tp_multiplayer_relay.exe must be beside this launcher.",
                    L"Relay executable not found", MB_OK | MB_ICONERROR);
        return;
    }

    dusk::multiplayer::InviteCodePayload endpoint;
    endpoint.transport = "relay";
    endpoint.host = publicHost;
    endpoint.port = port;
    endpoint.room = "relay-endpoint";
    endpoint.sessionId = "relay";
    endpoint.sessionKey = "endpoint";
    const std::wstring relayCode =
        from_utf8(dusk::multiplayer::create_invite_code(endpoint));

    std::wstring command =
        L"\"" + relayPath.wstring() + L"\" --host 0.0.0.0 --port " +
        std::to_wstring(port) + L" --public-host \"" + publicHostWide +
        L"\" --public-port " + std::to_wstring(port) + L" --verbose";
    std::vector<wchar_t> commandBuffer(command.begin(), command.end());
    commandBuffer.push_back(L'\0');

    std::error_code directoryError;
    const std::filesystem::path dataDirectory = relay_data_directory();
    std::filesystem::create_directories(dataDirectory, directoryError);
    if (directoryError) {
        MessageBoxW(nullptr, L"Windows could not create the relay log folder.",
                    L"Relay start failed", MB_OK | MB_ICONERROR);
        return;
    }
    gLogPath = dataDirectory / L"relay.log";

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE logHandle = CreateFileW(
        gLogPath.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (logHandle == INVALID_HANDLE_VALUE) {
        MessageBoxW(nullptr, L"Windows could not open the relay log file.",
                    L"Relay start failed", MB_OK | MB_ICONERROR);
        return;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullptr;
    startup.hStdOutput = logHandle;
    startup.hStdError = logHandle;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(relayPath.c_str(), commandBuffer.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, relayPath.parent_path().c_str(),
                        &startup, &process))
    {
        CloseHandle(logHandle);
        MessageBoxW(nullptr, L"Windows could not start the relay process.",
                    L"Relay start failed", MB_OK | MB_ICONERROR);
        return;
    }
    CloseHandle(logHandle);

    gRelayProcess = process;
    save_launcher_config(publicHost, port);
    SetWindowTextW(gCode, relayCode.c_str());
    SetWindowTextW(gStatus, L"Running - give the relay code below to all players");
    set_running(true);
}

void copy_code(HWND window) {
    const std::wstring code = get_text(gCode);
    if (code.empty() || !OpenClipboard(window)) {
        return;
    }
    EmptyClipboard();
    const size_t bytes = (code.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory != nullptr) {
        void* destination = GlobalLock(memory);
        memcpy(destination, code.c_str(), bytes);
        GlobalUnlock(memory);
        if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}

void open_log_folder(HWND window) {
    const std::filesystem::path directory = relay_data_directory();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error ||
        reinterpret_cast<INT_PTR>(ShellExecuteW(
            window, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
    {
        MessageBoxW(window, L"Windows could not open the relay log folder.",
                    L"Open logs failed", MB_OK | MB_ICONERROR);
    }
}

HWND add_control(HWND parent, const wchar_t* className, const wchar_t* text,
                 DWORD style, int x, int y, int width, int height, int id) {
    return CreateWindowExW(
        0, className, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        add_control(window, L"STATIC", L"Public IP or hostname", 0,
                    18, 20, 150, 22, 0);
        gPublicHost = add_control(window, L"EDIT", L"127.0.0.1",
                                 WS_BORDER | ES_AUTOHSCROLL, 175, 17, 430, 24,
                                 kPublicHostId);
        add_control(window, L"STATIC", L"Forwarded port", 0,
                    18, 58, 150, 22, 0);
        gPort = add_control(window, L"EDIT", L"34197",
                           WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL,
                           175, 55, 120, 24, kPortId);
        gStart = add_control(window, L"BUTTON", L"Start Relay",
                             BS_PUSHBUTTON, 18, 96, 130, 30, kStartId);
        gStop = add_control(window, L"BUTTON", L"Stop Relay",
                            BS_PUSHBUTTON, 158, 96, 130, 30, kStopId);
        gStatus = add_control(window, L"STATIC", L"Stopped", 0,
                              310, 103, 295, 22, kStatusId);
        add_control(window, L"STATIC", L"Relay code", 0,
                    18, 146, 100, 22, 0);
        gCode = add_control(window, L"EDIT", L"",
                            WS_BORDER | ES_AUTOHSCROLL | ES_READONLY,
                            18, 170, 587, 26, kCodeId);
        add_control(window, L"BUTTON", L"Copy Relay Code",
                    BS_PUSHBUTTON, 18, 210, 150, 30, kCopyId);
        add_control(window, L"BUTTON", L"Open Log Folder",
                    BS_PUSHBUTTON, 180, 210, 150, 30, kOpenLogsId);
        add_control(window, L"STATIC",
                    L"Give the same relay code to lobby creators and joiners.",
                    0, 18, 255, 587, 35, 0);

        EnumChildWindows(window, [](HWND child, LPARAM fontValue) -> BOOL {
            SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(fontValue), TRUE);
            return TRUE;
        }, reinterpret_cast<LPARAM>(font));
        load_launcher_config();
        set_running(false);
        SetTimer(window, kProcessTimerId, 500, nullptr);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kStartId: start_relay(); return 0;
        case kStopId: stop_relay(); return 0;
        case kCopyId: copy_code(window); return 0;
        case kOpenLogsId: open_log_folder(window); return 0;
        default: break;
        }
        break;
    case WM_TIMER:
        if (wParam == kProcessTimerId && gRelayProcess.hProcess != nullptr &&
            WaitForSingleObject(gRelayProcess.hProcess, 0) == WAIT_OBJECT_0)
        {
            DWORD exitCode = 0;
            GetExitCodeProcess(gRelayProcess.hProcess, &exitCode);
            close_process_handles();
            set_running(false);
            const std::wstring status =
                L"Stopped (exit " + std::to_wstring(exitCode) +
                L") - open logs for details";
            SetWindowTextW(gStatus, status.c_str());
        }
        return 0;
    case WM_DESTROY:
        KillTimer(window, kProcessTimerId);
        stop_relay();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    constexpr const wchar_t* kWindowClass = L"DusklightRelayLauncher";
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = window_proc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kWindowClass;
    if (RegisterClassW(&windowClass) == 0) {
        return 1;
    }

    HWND window = CreateWindowExW(
        0, kWindowClass, L"Dusklight Relay", WS_OVERLAPPED | WS_CAPTION |
        WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 645, 340,
        nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        return 1;
    }

    ShowWindow(window, showCommand);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
