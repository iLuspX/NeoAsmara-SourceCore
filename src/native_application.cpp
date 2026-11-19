#include "native/runtime.hpp"
#include <algorithm>
#include <commctrl.h>
#include <dbghelp.h>
#include <iostream>
#include <shellapi.h>

namespace rosh {
namespace {
fs::path dump_root;
LONG WINAPI crash_dump(EXCEPTION_POINTERS *exception) {
    try {
        fs::create_directories(dump_root / "Dumps");
        const auto file =
            dump_root / "Dumps" /
            (std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + ".dmp");
        auto handle = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), exception, FALSE};
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), handle, MiniDumpWithThreadInfo,
                              &info, nullptr, nullptr);
            CloseHandle(handle);
        }
    } catch (...) {
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
struct Window {
    std::unique_ptr<NativeService> service;
    HWND window{}, grid{}, logs{}, state{}, channel{}, announcement{}, operator_input{};
    HFONT font{};
    std::string role;
    fs::path root;
    int selected_channel{1};
    std::vector<HANDLE> children;
    void error(const std::exception &ex) {
        MessageBoxW(window, wide(ex.what()).c_str(), L"ROSH : Neo Asmara", MB_ICONERROR);
    }
    HWND control(const wchar_t *type, const wchar_t *text, DWORD style, int x, int y, int w, int h,
                 int id = 0) {
        auto item = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, window,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                    GetModuleHandleW(nullptr), nullptr);
        SendMessageW(item, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return item;
    }
    void initialize() {
        font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        control(L"BUTTON", L"Service control", BS_GROUPBOX, 12, 8, 948, 100);
        state = control(L"STATIC", L"Stopped", SS_LEFT, 26, 33, 340, 22);
        control(L"BUTTON", L"Start", BS_PUSHBUTTON, 370, 29, 90, 28, 101);
        control(L"BUTTON", L"Stop", BS_PUSHBUTTON, 468, 29, 90, 28, 102);
        control(L"BUTTON", L"SQLite checkpoint", BS_PUSHBUTTON, 566, 29, 146, 28, 103);
        control(L"BUTTON", L"Disconnect sessions", BS_PUSHBUTTON, 720, 29, 217, 28, 104);
        control(L"STATIC", wide(root.u8string()).c_str(), SS_PATHELLIPSIS, 26, 72, 700, 20);
        if (role == "WorldServer") {
            control(L"STATIC", L"Public channel:", 0, 736, 72, 95, 20);
            channel = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 834, 68, 105, 160, 105);
            for (int i = 1; i <= 4; i++)
                SendMessageW(channel, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(std::to_wstring(i).c_str()));
            SendMessageW(channel, CB_SETCURSEL, selected_channel - 1, 0);
        }
        control(L"BUTTON", L"Native services / channels", BS_GROUPBOX, 12, 118, 948, 235);
        grid = control(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | WS_BORDER, 25, 143, 922, 196, 106);
        ListView_SetExtendedListViewStyle(grid,
                                          LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        const wchar_t *names[] = {L"Instance",       L"Process",    L"State",        L"Authenticated",
                                  L"Public channel", L"Login port", L"Service ticks"};
        int widths[] = {220, 90, 110, 125, 110, 110, 130};
        for (int i = 0; i < 7; i++) {
            LVCOLUMNW c{};
            c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.pszText = const_cast<wchar_t *>(names[i]);
            c.cx = widths[i];
            ListView_InsertColumn(grid, i, &c);
        }
        if (role == "MainServer") {
            control(L"BUTTON", L"Start cluster (4 public channels)", BS_PUSHBUTTON, 25, 365, 270, 28, 107);
            control(L"BUTTON", L"Stop owned processes", BS_PUSHBUTTON, 305, 365, 200, 28, 108);
        }
        announcement = control(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 25, 407, 740, 26, 109);
        SendMessageW(announcement, EM_SETLIMITTEXT, 110, 0);
        control(L"BUTTON", L"Send notice", BS_PUSHBUTTON, 775, 406, 170, 28, 110);
        if(role=="MainServer"){
            control(L"STATIC",L"Console:",0,25,449,65,24);
            operator_input=control(L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL,95,446,670,26,112);
            SendMessageW(operator_input,EM_SETLIMITTEXT,220,0);
            control(L"BUTTON",L"Execute",BS_PUSHBUTTON,775,446,170,28,113);
        }
        control(L"BUTTON", L"Service log", BS_GROUPBOX, 12, 482, 948, 190);
        logs = control(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL, 25,
                       505, 922, 152, 111);
        control(L"STATIC", L"Native C++ / SQLite WAL / authenticated encrypted game data", 0, 25, 683, 900,
                20);
        SetTimer(window, 1, 1000, nullptr);
        refresh();
    }
    void refresh() {
        if (!service)
            return;
        auto status = service->status();
        auto text = role + std::string(status.value("running", false) ? " : RUNNING" : " : STOPPED") +
                    "    |    Sessions: " + std::to_string(status.value("users", 0));
        SetWindowTextW(state, wide(text).c_str());
        EnableWindow(GetDlgItem(window, 101), !service->running());
        EnableWindow(GetDlgItem(window, 102), service->running());
        if (channel)
            EnableWindow(channel, !service->running());
        ListView_DeleteAllItems(grid);
        int row = 0;
        for (const auto &item : service->cluster()) {
            std::vector<std::wstring> columns = {
                wide(item.value("instance", "")),
                std::to_wstring(item.value("pid", 0)),
                item.value("alive", false) && item.value("running", false) ? L"Running" : L"Stopped",
                std::to_wstring(item.value("users", 0)),
                std::to_wstring(item.value("channel", 0)),
                std::to_wstring(item.value("port", 0)),
                std::to_wstring(item.value("ticks", 0))};
            LVITEMW entry{};
            entry.mask = LVIF_TEXT;
            entry.iItem = row;
            entry.pszText = columns[0].data();
            ListView_InsertItem(grid, &entry);
            for (int c = 1; c < 7; c++)
                ListView_SetItemText(grid, row, c, columns[c].data());
            row++;
        }
        std::string lines;
        for (const auto &line : service->log_tail())
            lines += line + "\r\n";
        SetWindowTextW(logs, wide(lines).c_str());
        SendMessageW(logs, EM_SETSEL, -1, -1);
        SendMessageW(logs, EM_SCROLLCARET, 0, 0);
    }
    void start_cluster() {
        if (!children.empty())
            throw std::runtime_error("This MainServer already owns a cluster; stop it before restarting");
        service->start();
        const std::vector<std::pair<std::wstring, int>> roles = {{L"LobbyServer", 1}, {L"WorldServer", 1},
                                                                 {L"WorldServer", 2}, {L"WorldServer", 3},
                                                                 {L"WorldServer", 4}, {L"LoginServer", 1}};
        try {
            for (const auto &[name, index] : roles) {
                auto exe = root / "Bin" / (name + L".exe");
                auto command = L"\"" + exe.wstring() + L"\" --root \"" + root.wstring() + L"\" --channel " +
                               std::to_wstring(index) + L" --start";
                STARTUPINFOW startup{};
                startup.cb = sizeof(startup);
                startup.dwFlags = STARTF_USESHOWWINDOW;
                startup.wShowWindow = SW_SHOWMINNOACTIVE;
                PROCESS_INFORMATION process{};
                if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                    root.c_str(), &startup, &process))
                    throw std::runtime_error("Cannot launch " + utf8(name));
                CloseHandle(process.hThread);
                children.push_back(process.hProcess);
            }
        } catch (...) {
            stop_cluster();
            throw;
        }
    }
    void stop_cluster() {
        // A per-process named event requests an orderly socket shutdown and WAL checkpoint.
        for (auto process : children) {
            auto name = L"Local\\RoshNativeStop_" + std::to_wstring(GetProcessId(process));
            auto event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str());
            if (event) {
                SetEvent(event);
                CloseHandle(event);
            }
        }
        for (auto process : children) {
            WaitForSingleObject(process, 5000);
            CloseHandle(process);
        }
        children.clear();
    }
    void action(int id) {
        if (id == 101) {
            if (channel) {
                selected_channel = static_cast<int>(SendMessageW(channel, CB_GETCURSEL, 0, 0)) + 1;
                service = std::make_unique<NativeService>(root, role, selected_channel);
            }
            service->start();
        } else if (id == 102)
            service->stop();
        else if (id == 103)
            service->command("checkpoint");
        else if (id == 104)
            service->command("disconnect");
        else if (id == 107)
            start_cluster();
        else if (id == 108)
            stop_cluster();
        else if(id==113&&operator_input){wchar_t text[224]{};GetWindowTextW(operator_input,text,224);service->operator_console(utf8(text));SetWindowTextW(operator_input,L"");}
        else if (id == 110) {
            wchar_t text[112]{};
            GetWindowTextW(announcement, text, 112);
            if (!*text)
                return;
            if (role == "MainServer") {
                JobQueue jobs(root);
                for (const auto &item : jobs.services())
                    if (item.value("alive", false) && item.value("role", "") == "WorldServer")
                        jobs.enqueue(item.at("instance"), "announce", {{"text", utf8(text)}});
            } else
                service->command("announce", {{"text", utf8(text)}});
            SetWindowTextW(announcement, L"");
        }
        refresh();
    }
    ~Window() {
        stop_cluster();
        if (service)
            service->stop();
    }
};
LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    auto self = reinterpret_cast<Window *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<Window *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        self->window = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    try {
        if (self) {
            if (message == WM_CREATE) {
                self->initialize();
                return 0;
            }
            if (message == WM_COMMAND) {
                self->action(LOWORD(w));
                return 0;
            }
            if (message == WM_TIMER) {
                self->refresh();
                return 0;
            }
            if (message == WM_CLOSE) {
                DestroyWindow(hwnd);
                return 0;
            }
            if (message == WM_DESTROY) {
                PostQuitMessage(0);
                return 0;
            }
        }
    } catch (const std::exception &ex) {
        if (self)
            self->error(ex);
    }
    return DefWindowProcW(hwnd, message, w, l);
}
} // namespace
int server_application(HINSTANCE instance, int show, const std::string &role,
                       const std::wstring &command_line) {
    try {
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr, executable, 32768);
        fs::path root = fs::path(executable).parent_path().parent_path();
        int channel = 1, seconds = 0;
        DWORD after_process=0;
        bool headless = false, start = false, cluster = false;
        int argc{};
        auto args = CommandLineToArgvW((L"service " + command_line).c_str(), &argc);
        for (int i = 1; i < argc; i++) {
            std::wstring arg = args[i];
            if(arg==L"--after-process"&&i+1<argc){if(role!="MainServer")throw std::runtime_error("Restart helper requires MainServer");after_process=std::stoul(args[++i]);}
            else if (arg == L"--root" && i + 1 < argc)
                root = args[++i];
            else if (arg == L"--channel" && i + 1 < argc)
                channel = std::stoi(args[++i]);
            else if (arg == L"--duration" && i + 1 < argc)
                seconds = std::stoi(args[++i]);
            else if (arg == L"--headless")
                headless = true;
            else if (arg == L"--start")
                start = true;
            else if (arg == L"--cluster") {
                if(role != "MainServer") throw std::runtime_error("--cluster requires MainServer");
                cluster = true;
                start = true;
            }
            else
                throw std::runtime_error("Unknown or incomplete native service option");
        }
        LocalFree(args);
        root = fs::weakly_canonical(root);
        if(after_process){
            auto previous=OpenProcess(SYNCHRONIZE,FALSE,after_process);
            if(previous){auto waited=WaitForSingleObject(previous,60000);CloseHandle(previous);if(waited!=WAIT_OBJECT_0)throw std::runtime_error("Reinicio cancelado: processo anterior nao encerrou");}
        }
        dump_root = root;
        SetUnhandledExceptionFilter(crash_dump);
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        auto event_name = L"Local\\RoshNativeStop_" + std::to_wstring(GetCurrentProcessId());
        auto stop_event = CreateEventW(nullptr, TRUE, FALSE, event_name.c_str());
        if (!stop_event)
            throw std::runtime_error("Cannot create shutdown event");
        Window ui;
        ui.root = root;
        ui.role = role;
        ui.selected_channel = channel;
        ui.service = std::make_unique<NativeService>(root, role, channel);
        if (cluster)
            ui.start_cluster();
        else if (start)
            ui.service->start();
        int result = 0;
        if (headless) {
            if (!start)
                throw std::runtime_error("Headless mode requires --start");
            double until = seconds > 0 ? now() + seconds : 0;
            while (ui.service->running() && (!until || now() < until) &&
                   WaitForSingleObject(stop_event, 100) != WAIT_OBJECT_0) {
            }
            ui.service->stop();
        } else {
            INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
            InitCommonControlsEx(&controls);
            WNDCLASSW type{};
            type.lpfnWndProc = procedure;
            type.hInstance = instance;
            type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
            type.lpszClassName = L"RoshNativeServiceConsole";
            RegisterClassW(&type);
            auto title = wide(role + " - ROSH : Neo Asmara | Pre Alpha C++");
            auto hwnd =
                CreateWindowExW(0, type.lpszClassName, title.c_str(),
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT,
                                CW_USEDEFAULT, 990, 755, nullptr, nullptr, instance, &ui);
            if (!hwnd)
                throw std::runtime_error("Cannot create native management window");
            ShowWindow(hwnd, show);
            MSG message{};
            while (WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0) {
                if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    if (message.message == WM_QUIT)
                        break;
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                } else
                    MsgWaitForMultipleObjects(1, &stop_event, FALSE, 100, QS_ALLINPUT);
            }
            if (IsWindow(hwnd))
                DestroyWindow(hwnd);
            result = static_cast<int>(message.wParam);
        }
        CloseHandle(stop_event);
        return result;
    } catch (const std::exception &ex) {
        if (command_line.find(L"--headless") != std::wstring::npos) {
            std::cerr << role << ": " << ex.what() << "\n";
            try {
                RuntimeLog log(dump_root / "Logs" / (role + "-startup.jsonl"));
                log.write("error", ex.what());
            } catch (...) {
            }
        } else
            MessageBoxW(nullptr, wide(ex.what()).c_str(), wide(role + " - initialization failed").c_str(),
                        MB_ICONERROR);
        return 1;
    }
}
} // namespace rosh
extern "C" __declspec(dllexport) int RoshServerApplication(HINSTANCE instance, int show, const wchar_t *role,
                                                           const wchar_t *command) {
    return rosh::server_application(instance, show, rosh::utf8(role), command);
}
