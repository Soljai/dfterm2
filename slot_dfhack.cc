/* DFHack slot type. Code initially copied from slot_dfglue.cc */

#ifndef _WIN32
/* For now, use dfhack only for windows. */
#error "dfhack in dfterm2 is only for Windows. For UNIX/Linux/BSD/Some other use pty instead."
#endif

#include "slot_dfhack.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <boost/thread/thread.hpp>
#include <boost/thread/recursive_mutex.hpp>
#include <iostream>
#include "nanoclock.hpp"
#include "interface_ncurses.hpp"
#include "cp437_to_unicode.hpp"
#include <algorithm>
#include <cstring>

#include "state.hpp"

using namespace dfterm;
using namespace boost;
using namespace std;
using namespace trankesbel;

static void PostMessage(const vector<HWND> &windows, UINT msg, WPARAM wparam, LPARAM lparam)
{
    vector<HWND>::const_iterator i1, windows_end = windows.end();
    for (i1 = windows.begin(); i1 != windows_end; ++i1)
        PostMessage(*i1, msg, wparam, lparam);
}

static void SendMessage(const vector<HWND> &windows, UINT msg, WPARAM wparam, LPARAM lparam)
{
    vector<HWND>::const_iterator i1, windows_end = windows.end();
    for (i1 = windows.begin(); i1 != windows_end; ++i1)
        SendMessage(*i1, msg, wparam, lparam);
}

/* A simple checksum function. */
static ui32 checksum(const char* buf, size_t buflen)
{
    assert(!buflen || (buflen && buf));

    ui32 a = 1, b = buflen;
    size_t i1;
    for (i1 = 0; i1 < buflen; ++i1)
    {
        a += buf[i1];
        b += (buflen - i1) * (ui32) buf[i1];
    }

    a %= 65521;
    b %= 65521;

    return a + b * 65536;
}

static ui32 checksum(string str)
{ return checksum(str.c_str(), str.size()); };

DFHackSlot::DFHackSlot() : Slot()
{
    df_terminal.setCursorVisibility(false);
    reported_memory_error = false;

    df_handle = INVALID_HANDLE_VALUE;
    close_thread = false;
    data_format = Unknown;

    dont_take_running_process = false;

    df_w = 80;
    df_h = 25;

    ticks_per_second = 30;

    alive = true;

    initVkeyMappings();

    df_context = (DFHack::Context*) 0;
    df_position_module = (DFHack::Position*) 0;
    df_contextmanager = new DFHack::ContextManager("Memory.xml");

    // Create a thread that will launch or grab the DF process.
    glue_thread = SP<thread>(new thread(static_thread_function, this));
    if (glue_thread->get_id() == thread::id())
        alive = false;
};

DFHackSlot::DFHackSlot(bool dummy) : Slot()
{
    df_terminal.setCursorVisibility(false);
    reported_memory_error = false;

    df_handle = INVALID_HANDLE_VALUE;
    close_thread = false;
    data_format = Unknown;

    dont_take_running_process = true;

    df_w = 80;
    df_h = 25;

    ticks_per_second = 20;

    alive = true;

    initVkeyMappings();

    df_context = (DFHack::Context*) 0;
    df_contextmanager = new DFHack::ContextManager("Memory.xml");

    // Create a thread that will launch or grab the DF process.
    glue_thread = SP<thread>(new thread(static_thread_function, this));
    if (glue_thread->get_id() == thread::id())
        alive = false;
}

DFHackSlot::~DFHackSlot()
{
    if (glue_thread)
        glue_thread->interrupt();

    unique_lock<recursive_mutex> lock(glue_mutex);
    close_thread = true;
    if (df_handle != INVALID_HANDLE_VALUE && dont_take_running_process) TerminateProcess(df_handle, 1);
    lock.unlock();

    if (glue_thread)
        glue_thread->join();

    if (df_handle != INVALID_HANDLE_VALUE) CloseHandle(df_handle);
    if (df_contextmanager) delete df_contextmanager;
}

bool DFHackSlot::isAlive()
{
    unique_lock<recursive_mutex> alive_try_lock(glue_mutex, try_to_lock_t());
    if (!alive_try_lock.owns_lock()) return true;

    bool alive_bool = alive;
    return alive_bool;
}

void DFHackSlot::static_thread_function(DFHackSlot* self)
{
    assert(self);
    self->thread_function();
}

void DFHackSlot::initVkeyMappings()
{
    fixed_mappings[(KeyCode) '0'] = VK_NUMPAD0;
    fixed_mappings[(KeyCode) '1'] = VK_NUMPAD1;
    fixed_mappings[(KeyCode) '2'] = VK_NUMPAD2;
    fixed_mappings[(KeyCode) '3'] = VK_NUMPAD3;
    fixed_mappings[(KeyCode) '4'] = VK_NUMPAD4;
    fixed_mappings[(KeyCode) '5'] = VK_NUMPAD5;
    fixed_mappings[(KeyCode) '6'] = VK_NUMPAD6;
    fixed_mappings[(KeyCode) '7'] = VK_NUMPAD7;
    fixed_mappings[(KeyCode) '8'] = VK_NUMPAD8;
    fixed_mappings[(KeyCode) '9'] = VK_NUMPAD9;
    fixed_mappings[(KeyCode) '/'] = VK_DIVIDE;
    fixed_mappings[(KeyCode) '*'] = VK_MULTIPLY;
    fixed_mappings[(KeyCode) '+'] = VK_ADD;
    fixed_mappings[(KeyCode) '-'] = VK_SUBTRACT;

    vkey_mappings[AUp] = VK_UP;
    vkey_mappings[ADown] = VK_DOWN;
    vkey_mappings[ALeft] = VK_LEFT;
    vkey_mappings[ARight] = VK_RIGHT;
    vkey_mappings[AltUp] = VK_UP;
    vkey_mappings[AltDown] = VK_DOWN;
    vkey_mappings[AltLeft] = VK_LEFT;
    vkey_mappings[AltRight] = VK_RIGHT;
    vkey_mappings[CtrlUp] = VK_UP;
    vkey_mappings[CtrlDown] = VK_DOWN;
    vkey_mappings[CtrlLeft] = VK_LEFT;
    vkey_mappings[CtrlRight] = VK_RIGHT;
    vkey_mappings[F1] = VK_F1;
    vkey_mappings[F2] = VK_F2;
    vkey_mappings[F3] = VK_F3;
    vkey_mappings[F4] = VK_F4;
    vkey_mappings[F5] = VK_F5;
    vkey_mappings[F6] = VK_F6;
    vkey_mappings[F7] = VK_F7;
    vkey_mappings[F8] = VK_F8;
    vkey_mappings[F9] = VK_F9;
    vkey_mappings[F10] = VK_F10;
    vkey_mappings[F11] = VK_F11;
    vkey_mappings[F12] = VK_F12;

    vkey_mappings[Home] = VK_HOME;
    vkey_mappings[End] = VK_END;
    vkey_mappings[PgUp] = VK_PRIOR;
    vkey_mappings[PgDown] = VK_NEXT;

    vkey_mappings[InsertChar] = VK_INSERT;
    vkey_mappings[DeleteChar] = VK_DELETE;

    vkey_mappings[Enter] = VK_RETURN;
}

extern WCHAR local_directory[60000];
extern size_t local_directory_size;

void DFHackSlot::thread_function()
{
    assert(!df_context);
    assert(df_contextmanager);

    // Handle and window for process goes here. 
    HANDLE df_process = INVALID_HANDLE_VALUE;
    vector<HWND> df_windows;

    if (!dont_take_running_process)
    {
        try
        {
            df_context = df_contextmanager->getSingleContext();
            df_context->Attach();
            df_position_module = df_context->getPosition();
        }
        catch (std::exception &e)
        {
            LOG(Error, "Cannot get a working context to DF with dfhack. \"" << e.what() << "\"");
            alive = false;
            return;
        }

        DFHack::Process* p = df_context->getProcess();
        assert(p);
        DWORD pid = p->getPID();
        HANDLE h_p = OpenProcess(PROCESS_CREATE_THREAD|PROCESS_VM_WRITE|PROCESS_VM_OPERATION|PROCESS_QUERY_INFORMATION|PROCESS_VM_READ, FALSE, pid);
        if (h_p == INVALID_HANDLE_VALUE)
        {
            LOG(Error, "DFHack attach succeeded but otherwise opening the handle failed.");
            alive = false;
            return;
        }

        findDFWindow(&df_windows, pid);

        df_process = h_p;
    }
    else
    // Launch a new DF process
    {
        // Oops. doesn't work yet
        alive = false;
        return;

        /*
        if (!launchDFProcess(&df_process, &df_windows))
        {
            alive = false;
            return;
        }
        */
    }

    assert(df_context->isAttached());

    unique_lock<recursive_mutex> alive_lock(glue_mutex);
    this->df_windows = df_windows;
    this->df_handle = df_process;

    string injection_glue_dll = TO_UTF8(local_directory, local_directory_size) + string("\\dfterm_injection_glue.dll");
    LOG(Note, "Injecting " << injection_glue_dll << " to process in 2 seconds.");
    injectDLL(injection_glue_dll);
    LOG(Note, "Waiting 2 seconds for injection to land.");
    df_context->ForceResume();
    

    try
    {
        this_thread::sleep(posix_time::time_duration(posix_time::microseconds(2000000LL)));
    }
    catch (const thread_interrupted &ti)
    {
        CloseHandle(df_process);
        this->df_handle = INVALID_HANDLE_VALUE;
        this->df_windows.clear();;
        alive = false;
        alive_lock.unlock();
        return;
    }

    alive_lock.unlock();

    try
    {
        while(!isDFClosed() && df_context)
        {
            this_thread::interruption_point();

            unique_lock<recursive_mutex> update_mutex(glue_mutex);

            if (close_thread) break;
            updateWindowSizeFromDFMemory();
        
            bool esc_down = false;
            while(input_queue.size() > 0)
            {
                bool esc_down_now = esc_down;
                bool ctrl_down_now = false;
                bool shift_down_now = false;
                esc_down = false;

                DWORD vkey;
                ui32 keycode = input_queue.front().getKeyCode();
                bool special_key = input_queue.front().isSpecialKey();
                if (input_queue.front().isAltDown()) esc_down_now = true;
                if (input_queue.front().isShiftDown()) shift_down_now = true;
                if (input_queue.front().isCtrlDown()) ctrl_down_now = true;

                input_queue.pop_front();

                if (!special_key)
                {
                    if (keycode == 27 && input_queue.size() > 0)
                    {
                        esc_down = true;
                        continue;
                    }
                    else if (keycode == 27)
                        vkey = VK_ESCAPE;
                    else if (keycode == '\n')
                        vkey = VK_RETURN;
                    else
                    {
                        map<KeyCode, DWORD>::iterator i1 = fixed_mappings.find((KeyCode) keycode);
                        if (i1 != fixed_mappings.end())
                            vkey = i1->second;
                        else
                        {
                            SHORT keyscan = VkKeyScanW((WCHAR) keycode);
                            vkey = (DWORD) (keyscan & 0xFF);
                            if ((keyscan & 0x00ff) != -1 && (keyscan & 0xff00))
                                shift_down_now = true;
                            if (keycode == 127) // backspace
                                vkey = VK_BACK;
                        }
                    }
                }
                else if (special_key)
                {
                    map<KeyCode, DWORD>::iterator i1 = vkey_mappings.find((KeyCode) keycode);
                    if (i1 == vkey_mappings.end()) continue;

                    vkey = i1->second;
                
                    KeyCode kc = (KeyCode) keycode;
                    if (kc == CtrlUp || kc == CtrlDown || kc == CtrlRight || kc == CtrlLeft)
                        ctrl_down_now = true;
                }

                if (shift_down_now)
                    PostMessage(df_windows, WM_USER, 1, 0);
                if (ctrl_down_now)
                    PostMessage(df_windows, WM_USER, 1, 1);
                if (esc_down_now)
                    PostMessage(df_windows, WM_USER, 1, 2);
                PostMessage(df_windows, WM_USER, vkey, 3 + (keycode << 16));
                if (shift_down_now)
                    PostMessage(df_windows, WM_USER, 0, 0);
                if (ctrl_down_now)
                    PostMessage(df_windows, WM_USER, 0, 1);
                if (esc_down_now)
                    PostMessage(df_windows, WM_USER, 0, 2);
            }

            update_mutex.unlock();

            updateDFWindowTerminal();

            {
            SP<State> s = state.lock();
            SP<Slot> self_sp = self.lock();
            if (s && self_sp)
                s->signalSlotData(self_sp);
            }

            this_thread::sleep(posix_time::time_duration(posix_time::microseconds(1000000LL / ticks_per_second)));
        }
    }
    catch (const thread_interrupted &ti)
    {
        
    }
    alive = false;
}

void DFHackSlot::buildColorFromFloats(float32 r, float32 g, float32 b, Color* color, bool* bold)
{
    assert(color && bold);

    (*color) = Black;
    (*bold) = false;

    Color result = Black;
    bool bold_result = false;

    if (r > 0.1) result = Red;
    if (g > 0.1)
    {
        if (result == Red) result = Yellow;
        else result = Green;
    }
    if (b > 0.1)
    {
        if (result == Red) result = Magenta;
        else if (result == Green) result = Cyan;
        else if (result == Yellow) result = White;
        else result = Blue;
    }
    if (r >= 0.9 || g >= 0.9 || b >= 0.9)
        bold_result = true;
    if (result == White && r < 0.75)
    {
        result = Black;
        bold_result = true;
    }

    (*color) = result;
    (*bold) = bold_result;
}

void DFHackSlot::updateDFWindowTerminal()
{
    lock_guard<recursive_mutex> alive_mutex_lock(glue_mutex);

    assert(df_context);

    if (df_terminal.getWidth() != df_w || df_terminal.getHeight() != df_h)
        df_terminal.resize(df_w, df_h);

    if (df_w > 256 || df_h > 256)
        return; /* I'd like to log this message but it would most likely spam the log
                   when this happens. */

    DFHack::t_screen screendata[256*256];

    assert(df_position_module);
    try
    {
        df_context->Suspend();
        if (!df_position_module->getScreenTiles(df_w, df_h, screendata))
        {
            LOG(Error, "Cannot read screen data with DFHack from DF for some reason. (missing screen_tiles_pointer?)");
            return;
        }
        df_context->ForceResume();
    }
    catch(std::exception &e)
    {
        LOG(Error, "Catched an exception from DFHack while reading screen tiles: " << e.what());
        return;
    }

    for (ui32 i1 = 0; i1 < df_h; ++i1)
        for (ui32 i2 = 0; i2 < df_w; ++i2)
        {
            ui32 offset = i2 + i1*df_w;

            Color f_color = (Color) screendata[offset].foreground;
            Color b_color = (Color) screendata[offset].background;

            if (f_color == Red) f_color = Blue;
            else if (f_color == Blue) f_color = Red;
            else if (f_color == Yellow) f_color = Cyan;
            else if (f_color == Cyan) f_color = Yellow;
            if (b_color == Red) b_color = Blue;
            else if (b_color == Blue) b_color = Red;
            else if (b_color == Yellow) b_color = Cyan;
            else if (b_color == Cyan) b_color = Yellow;

            df_terminal.setTile(i2, i1, TerminalTile(screendata[offset].symbol, 
                                                     f_color, 
                                                     b_color,
                                                     false, 
                                                     screendata[offset].bright));
        }

    LockedObject<Terminal> term = df_cache_terminal.lock();
    term->copyPreserve(&df_terminal);
}

class enumDFWindow_struct
{
    public:
    DWORD process_id;
    vector<HWND> window;
    enumDFWindow_struct()
    {
        process_id = 0;
    };
};

// Callback for finding the DF window
BOOL CALLBACK DFHackSlot::enumDFWindow(HWND hwnd, LPARAM strct)
{
    enumDFWindow_struct* edw = (enumDFWindow_struct*) strct;

    DWORD window_pid;
    GetWindowThreadProcessId(hwnd, &window_pid);
    if (edw->process_id == window_pid)
        edw->window.push_back(hwnd);;

    return TRUE;
}

bool DFHackSlot::findDFWindow(vector<HWND>* df_windows, DWORD pid)
{
    assert(df_windows);
    (*df_windows).clear();

    // Find the DF window
    enumDFWindow_struct edw;
    edw.process_id = pid;
    EnumWindows(enumDFWindow, (LPARAM) &edw);
    if (edw.window.empty())
        return false;

    (*df_windows) = edw.window;
    return true;
}

bool DFHackSlot::launchDFProcess(HANDLE* df_process, vector<HWND>* df_windows)
{
    assert(df_process && df_windows);

    /* Wait until "path" and "work" are set for 60 seconds. */
    /* Copied the code from slot_terminal.cc */

    ui8 counter = 60;
    map<string, UnicodeString>::iterator i1;
    while (counter > 0)
    {
        unique_lock<recursive_mutex> lock(glue_mutex);
        if (close_thread)
        {
            lock.unlock();
            return false;
        }

        if (parameters.find("path") != parameters.end() &&
            parameters.find("work") != parameters.end())
            break;
        lock.unlock();
        --counter;
        try
        {
            this_thread::sleep(posix_time::time_duration(posix_time::microseconds(1000000LL)));
        }
        catch (const thread_interrupted &ti)
        {
            return false;
        }
    }
    if (counter == 0)
    {
        lock_guard<recursive_mutex> lock(glue_mutex);
        alive = false;
        return false;
    }

    unique_lock<recursive_mutex> ulock(glue_mutex);
    UnicodeString path = parameters["path"];
    UnicodeString work_dir = parameters["work"];
    ulock.unlock();

    wchar_t path_cstr[MAX_PATH+1], working_path[MAX_PATH+1];
    memset(path_cstr, 0, (MAX_PATH+1)*sizeof(wchar_t));
    memset(working_path, 0, (MAX_PATH+1)*sizeof(wchar_t));
    size_t path_size = MAX_PATH;
    size_t work_size = MAX_PATH;
    TO_WCHAR_STRING(path, path_cstr, &path_size);
    TO_WCHAR_STRING(work_dir, working_path, &work_size);
    
    STARTUPINFO sup;
    memset(&sup, 0, sizeof(STARTUPINFO));
    sup.cb = sizeof(sup);

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessW(NULL, path_cstr, NULL, NULL, FALSE, 0, NULL, working_path, &sup, &pi))
    {
        LOG(Error, "SLOT/DFHackSlot: CreateProcess() failed with GetLastError() == " << GetLastError());
        return false;
    }

    unique_lock<recursive_mutex> lock(glue_mutex);
    df_terminal.printString("Take it easy!! Game is being launched and attached in 20 seconds.", 1, 1, 7, 0, false, false);
    lock.unlock();

    /* Sleep for 18 seconds before attaching */
    try
    {
        this_thread::sleep(posix_time::time_duration(posix_time::microseconds(18000000LL)));
    }
    catch (const thread_interrupted &ti)
    {
        if (pi.hThread != INVALID_HANDLE_VALUE) CloseHandle(pi.hThread);
        if (pi.hProcess) { UINT dummy = 0; TerminateProcess(pi.hProcess, dummy); };
        CloseHandle(pi.hProcess);
        return false;
    }

    (*df_process) = pi.hProcess;
    if (pi.hThread != INVALID_HANDLE_VALUE) CloseHandle(pi.hThread);

    findDFWindow(df_windows, pi.dwProcessId);
    if ((*df_windows).empty())
    {
        CloseHandle(*df_process);
        return false;
    }

    return true;
};

bool DFHackSlot::findDFProcess(HANDLE* df_process, vector<HWND>* df_windows)
{
    assert(df_process);
    assert(df_windows);

    DWORD processes[1000], num_processes;
    EnumProcesses(processes, sizeof(DWORD)*1000, &num_processes);

    num_processes /= sizeof(DWORD);

    HANDLE df_handle = NULL;

    // Check if Dwarf Fortress is running
    int i1;
    for (i1 = 0; i1 < (int) num_processes; ++i1)
    {
        HANDLE h_p = OpenProcess(PROCESS_CREATE_THREAD|PROCESS_VM_WRITE|PROCESS_VM_OPERATION|PROCESS_QUERY_INFORMATION|PROCESS_VM_READ, FALSE, processes[i1]);
        if (!h_p)
            continue;

        HMODULE h_mod;
        DWORD cb_needed;
        WCHAR process_name[MAX_PATH+1];
        process_name[MAX_PATH] = 0;

        if (EnumProcessModules(h_p, &h_mod, sizeof(h_mod), &cb_needed))
        {
            GetModuleBaseNameW(h_p, h_mod, process_name, sizeof(process_name) / sizeof(TCHAR));

            if (TO_UTF8(process_name) == string("dwarfort.exe") ||
                TO_UTF8(process_name) == string("Dwarf Fortress.exe"))
            {
                df_handle = h_p;
                break;
            }
        }

        CloseHandle(h_p);
    }

    if (!df_handle)
        return false;

    findDFWindow(df_windows, processes[i1]);
    if ((*df_windows).empty())
    {
        CloseHandle(df_handle);
        return false;
    }

    (*df_process) = df_handle;
    return true;
}

void DFHackSlot::getSize(ui32* width, ui32* height)
{
    assert(width && height);

    lock_guard<recursive_mutex> alive_lock(glue_mutex);
    ui32 w = 80, h = 25;
    (*width) = w;
    (*height) = h;
}

void DFHackSlot::LoggerReadProcessMemory(HANDLE handle, const void* address, void* target, SIZE_T size, SIZE_T* read_size)
{
    int result = ReadProcessMemory(handle, address, target, size, read_size);
    if (!result && !reported_memory_error)
    {
        reported_memory_error = true;
        int err = GetLastError();
        LOG(Error, "ReadProcessMemory() failed for address " << address << " with GetLastError() == " << err << ". (This error will only be reported once per slot)");
    }
}

void DFHackSlot::updateWindowSizeFromDFMemory()
{
    lock_guard<recursive_mutex> alive_lock(glue_mutex);

    assert(df_position_module);

    ::int32_t width, height;
    try
    {
        df_context->Suspend();
        df_position_module->getWindowSize(width, height);
        df_context->ForceResume();
    }
    catch (std::exception &e)
    {
        LOG(Error, "Catched an exception from DFHack while reading window size: " << e.what());
        return;
    }

    // Some protection against bogus offsets
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    if (width > 300) width = 300;
    if (height > 300) height = 300;

    df_w = width;
    df_h = height;

    LockedObject<Terminal> t = df_cache_terminal.lock();
    if (t->getWidth() != df_w || t->getHeight() != df_h)
        t->resize(df_w, df_h);
}

void DFHackSlot::unloadToWindow(SP<Interface2DWindow> target_window)
{
    assert(alive && target_window);

    LockedObject<Terminal> term = df_cache_terminal.lock();
    ui32 t_w = term->getWidth(), t_h = term->getHeight();

    ui32 actual_window_w, actual_window_h;
    t_w = min(t_w, (ui32) 256);
    t_h = min(t_h, (ui32) 256);
    target_window->setMinimumSize(t_w, t_h);
    target_window->getSize(&actual_window_w, &actual_window_h);

    ui32 game_offset_x = 0;
    ui32 game_offset_y = 0;
    if (actual_window_w > t_w)
        game_offset_x = (actual_window_w - t_w) / 2;
    if (actual_window_h > t_h)
        game_offset_y = (actual_window_h - t_h) / 2;

    CursesElement* elements = new CursesElement[actual_window_w * actual_window_h];
    ui32 i1, i2;
    for (i1 = 0; i1 < actual_window_w; ++i1)
        for (i2 = 0; i2 < actual_window_h; ++i2)
        {
            TerminalTile t;
            if (i1 < game_offset_x || i2 < game_offset_y || (i1 - game_offset_x) >= t_w || (i2 - game_offset_y) >= t_h)
                t = TerminalTile(' ', 7, 0, false, false);
            else
                t = term->getTile(i1 - game_offset_x, i2 - game_offset_y);

            ui32 symbol = t.getSymbol();
            if (symbol > 255) symbol = symbol % 256;
            elements[i1 + i2 * actual_window_w] = CursesElement(mapCharacter(symbol), (Color) t.getForegroundColor(), (Color) t.getBackgroundColor(), t.getBold());
        }
    target_window->setScreenDisplayNewElements(elements, sizeof(CursesElement), actual_window_w, actual_window_w, actual_window_h, 0, 0);
    delete[] elements;
}

bool DFHackSlot::injectDLL(string dllname)
{
    assert(!dllname.empty());
    assert(df_handle != INVALID_HANDLE_VALUE);

    /* Allocate memory for the dll string in the target process */
    LPVOID address = VirtualAllocEx(df_handle, NULL, dllname.size()+1, MEM_COMMIT, PAGE_READWRITE);
    if (!address)
        return false;
    if (!WriteProcessMemory(df_handle, address, 
                            (LPCVOID) dllname.c_str(), dllname.size(), NULL))
        return false;

    HMODULE k32 = GetModuleHandleW(L"Kernel32");
    FARPROC loadlibrary_address = GetProcAddress(k32, "LoadLibraryA");

    HANDLE remote_thread = CreateRemoteThread(df_handle, NULL, 0, (LPTHREAD_START_ROUTINE) loadlibrary_address, address, 0, NULL);
    if (!remote_thread)
        return false;

    return true;
}

void DFHackSlot::feedInput(const KeyPress &kp)
{
    lock_guard<recursive_mutex> alive_lock(glue_mutex);
    input_queue.push_back(kp);
}

bool DFHackSlot::isDFClosed()
{
    assert(df_contextmanager);

    DFHack::BadContexts bc;
    df_contextmanager->Refresh(&bc);
    if (bc.Contains(df_context))
    {
        df_context = (DFHack::Context*) 0;
        if (df_handle) df_handle = INVALID_HANDLE_VALUE;
        df_windows.clear();
        return true;
    }
    
    return false;
};
