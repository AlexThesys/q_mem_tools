#include "process_mem_tool.h"
#include "common.h"

struct process_context {
    common_context common;
    DWORD pid;
};

static struct block {
    const char* ptr;
    size_t size;
    size_t info_id;
};

static int list_processes();
static int list_process_modules(DWORD dw_pid);
static int list_process_threads(DWORD dw_owner_pid);
static int traverse_heap_list(DWORD dw_pid, bool list_blocks, bool calculate_entropy);
static void print_error(TCHAR const* msg);

static std::mutex g_err_mtx;

static void find_pattern(HANDLE process, const char* pattern, size_t pattern_len) {
    std::vector<std::vector<const char*>> match;
    std::vector<block> blocks;
    std::vector<MEMORY_BASIC_INFORMATION> info;
    const LONG64 max_memory_usage = g_memory_limit;

    SYSTEM_INFO sysinfo = { 0 };
    ::GetSystemInfo(&sysinfo);
    const DWORD alloc_granularity = sysinfo.dwAllocationGranularity;
    assert(is_pow_2(alloc_granularity));

    g_memory_usage_bytes = 0;

    puts("Searching committed memory...");
    puts("\n------------------------------------\n");

    const size_t extra_chunk = multiple_of_n(pattern_len, sizeof(__m128i));
    const size_t block_size = alloc_granularity * g_num_alloc_blocks;
    const size_t bytes_to_read_ideal = block_size + extra_chunk;

    {
        const char* _p = NULL;
        MEMORY_BASIC_INFORMATION _info;
        for (_p = NULL; VirtualQueryEx(process, _p, &_info, sizeof(_info)) == sizeof(_info); _p += _info.RegionSize) {
            if (_info.State == MEM_COMMIT) {
                size_t region_size = _info.RegionSize;
                if (region_size < pattern_len) {
                    continue;
                }
                info.push_back(_info);
                size_t bytes_offset = 0;
                while (region_size) {
                    size_t bytes_to_read;
                    if (region_size >= bytes_to_read_ideal) {
                        bytes_to_read = bytes_to_read_ideal;
                        region_size -= block_size;
                    } else {
                        bytes_to_read = region_size;
                        region_size = 0;
                    }
                    block b = { _p + bytes_offset, bytes_to_read, info.size() - 1 };
                    blocks.push_back(b);
                    bytes_offset += block_size;
                }
            }
        }
    }
    const size_t num_blocks = blocks.size();
    match.resize(num_blocks);

    const int num_threads = _min(num_blocks, _min(g_max_omp_threads, omp_get_num_procs()));
    std::vector<char*> buffers(num_threads);
    for (char*& b : buffers) {
        b = (char*)malloc(bytes_to_read_ideal);
    }

    omp_set_num_threads(num_threads);   
#pragma omp parallel for schedule(dynamic, 1) shared(match,blocks,info)
    for (int64_t i = 0;  i < (int64_t)num_blocks; i++) {
        const int omp_tid = omp_get_thread_num();
        char* buffer = buffers[omp_tid];
        const size_t info_id = blocks[i].info_id;
        const MEMORY_BASIC_INFORMATION& r_info = info[info_id];
        const char* ptr = blocks[i].ptr;
        const size_t bytes_to_read = blocks[i].size;

        assert((r_info.Type == MEM_MAPPED || r_info.Type == MEM_PRIVATE || r_info.Type == MEM_IMAGE));

        {
            std::unique_lock<std::mutex> lk(g_mtx);
            while (true) {
                g_cv.wait(lk, [max_memory_usage] { return (g_memory_usage_bytes < max_memory_usage); });
                if (g_memory_usage_bytes < max_memory_usage) {
                    g_memory_usage_bytes += bytes_to_read;
                    break;
                }
            }
        }

        SIZE_T bytes_read;
        const BOOL res = ReadProcessMemory(process, ptr, buffer, bytes_to_read, &bytes_read);

        if (!res || (bytes_read != bytes_to_read)) {
            const size_t skip = _min(block_size, bytes_read);
            if (!g_show_failed_readings) {
                if (!res) {
                    std::unique_lock<std::mutex> lk(g_mtx);
                    g_memory_usage_bytes -= bytes_to_read;
                    continue;
                }
            } else {
                std::unique_lock<std::mutex> lk(g_err_mtx);
                if (r_info.Type == MEM_IMAGE) {
                    char module_name[MAX_PATH];
                    if (GetModuleFileNameExA(process, (HMODULE)r_info.AllocationBase, module_name, MAX_PATH)) {
                        puts("------------------------------------\n");
                        printf("Module name: %s\n", module_name);
                    }
                }
                printf("Base addres: 0x%p\tAllocation Base: 0x%p\tRegion Size: 0x%08llx\nState: %s\tProtect: %s\t",
                    r_info.BaseAddress, r_info.AllocationBase, r_info.RegionSize, get_page_protect(r_info.Protect), get_page_state(r_info.State));
                print_page_type(r_info.Type);
                if (!res) {
                    fprintf(stderr, "Failed reading process memory. Error code: %lu\n\n", GetLastError());
                    std::unique_lock<std::mutex> lk(g_mtx);
                    g_memory_usage_bytes -= bytes_to_read;
                    continue;
                } else {
                    printf("Process memory not read in it's entirety! 0x%llx bytes skipped out of 0x%llx\n\n", (bytes_to_read - bytes_read), bytes_to_read);
                }
            }
        }

        if (bytes_read >= pattern_len) {
            const char* buffer_ptr = buffer;
            size_t buffer_size = bytes_read;

            while (buffer_size >= pattern_len) {
                const char* old_buf_ptr = buffer_ptr;
                buffer_ptr = (const char*)strstr_u8((const uint8_t*)buffer_ptr, buffer_size, (const uint8_t*)pattern, pattern_len);
                if (!buffer_ptr) {
                    break;
                }

                const size_t buffer_offset = buffer_ptr - buffer;
                match[i].push_back(ptr + buffer_offset);

                buffer_ptr++;
                buffer_size -= (buffer_ptr - old_buf_ptr);
            }
        }
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            g_memory_usage_bytes -= bytes_to_read;
        }
        g_cv.notify_all(); // permitted to be called concurrentely
    }

    for (char* b : buffers) {
        free(b);
    }

    size_t num_matches = 0;
    {
    for (const auto& m : match) {
        num_matches += m.size();
    }
    }
    if (!num_matches) {
        puts("*** No matches found. ***");
        return;
    }
    if (too_many_results(num_matches)) {
        return;
    }
    printf("*** Approximate number of matches: %llu ***\n\n", num_matches);

    uint64_t prev_match = (uint64_t)(-1); // there could be duplicates because of the blocks' overlap

    for (size_t i = 0; i < num_blocks; i++) {
        if (match[i].size()) {
            const size_t info_id = blocks[i].info_id;
            const MEMORY_BASIC_INFORMATION& r_info = info[info_id];
            if (r_info.Type == MEM_IMAGE) {
                char module_name[MAX_PATH];
                if (GetModuleFileNameExA(process, (HMODULE)r_info.AllocationBase, module_name, MAX_PATH)) {
                    puts("------------------------------------\n");
                    printf("Module name: %s\n", module_name);
                }
            }
            printf("Base addres: 0x%p\tAllocation Base: 0x%p\tRegion Size: 0x%08llx\nState: %s\tProtect: %s\t",
                r_info.BaseAddress, r_info.AllocationBase, r_info.RegionSize, get_page_protect(r_info.Protect), get_page_state(r_info.State));
            print_page_type(r_info.Type);
            puts("");
            for (const char* m : match[i]) {
                if (prev_match == (uint64_t)m) {
                    continue;
                }
                prev_match = (uint64_t)m;
                printf("\tMatch at address: 0x%p\n", m);
            }
            puts("");
        }
    }
}

static void print_help() {
    puts("--------------------------------");
    puts("p <pid>\t\t\t - select PID");
    puts("lp\t\t\t - list system PIDs");
    puts("th\t\t\t - travers process heaps (slow)");
    puts("the\t\t\t - travers process heaps, calculate entropy (slower)");
    puts("thb\t\t\t - travers process heaps, list heap blocks (extra slow)");
    puts("********************************\n");
}

static input_command parse_command(process_context *ctx, search_data *data, char* cmd, char *pattern) {
    input_command command;
    if (cmd[0] == 'p') {
        size_t pid_len = strlen(cmd);
        char* args = skip_to_args(cmd, pid_len);
        if (args == nullptr) {
            puts("PID missing.");
            return c_continue;
        }
        pid_len -= (ptrdiff_t)(args - cmd);
        char* end = NULL;
        const DWORD pid = strtoul(args, &end, is_hex(args, pid_len) ? 16 : 10);
        if (args == end) {
            puts("Invalid PID! Exiting...");
            command = c_quit_program;
        } else {
            ctx->pid = pid;
            command = c_continue;
        }
    } else if (cmd[0] == 'l') {
        if (cmd[1] == 'p') {
            command = c_list_pids;
        } else if (cmd[1] == 'M') {
            command = c_list_modules;
        } else if (cmd[1] == 't') {
            command = c_list_threads;
        } else {
            puts(unknown_command);
            command = c_continue;
        }
    } else if ((cmd[0] == 't') && (cmd[1] == 'h')) {
        if (cmd[2] == 0) {
            command = c_travers_heap;
        } else if (cmd[2] == 'e') {
            command = c_travers_heap_calc_entropy;
        } else if (cmd[2] == 'b') {
            command = c_travers_heap_blocks;
        } else {
            puts(unknown_command);
            command = c_continue;
        }
    } else {
        puts(unknown_command);
        command = c_continue;
    }
    puts("");

    return command;
}

static void execute_command(input_command cmd, process_context *ctx) {
    if ((cmd != c_help) && ((cmd != c_list_pids)) && (ctx->pid == (DWORD)(-1))) {
        puts("Select the PID first!");
        return;
    }

    switch (cmd) {
    case c_help :
        print_help_common();
        print_help();
        break;
    case c_search_pattern : {
        assert(ctx->common.pattern != nullptr);
        HANDLE process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, false, ctx->pid);
        if (process == NULL) {
            fprintf(stderr, "Failed opening the process. Error code: %lu\n", GetLastError());
            return;
        }

        char proc_name[MAX_PATH];
        if (GetModuleFileNameExA(process, NULL, proc_name, MAX_PATH)) {
            printf("Process name: %s\n\n", proc_name);
        }

        find_pattern(process, ctx->common.pattern, ctx->common.pattern_len);

        CloseHandle(process);
        break;
    }
    case c_search_pattern_in_registers :
        puts(command_not_implemented);
        puts("");
        break;
    case c_list_pids:
        list_processes();
        break;
    case c_list_modules:
        list_process_modules(ctx->pid);
        break;
    case c_list_threads:
        list_process_threads(ctx->pid);
        break;
    case c_travers_heap:
        traverse_heap_list(ctx->pid, false, false);
        break;
    case c_travers_heap_calc_entropy:
        traverse_heap_list(ctx->pid, false, true);
        break;
    case c_travers_heap_blocks:
        traverse_heap_list(ctx->pid, true, false);
        break;
    default :
        puts(unknown_command);
        break;
    }
    puts("====================================\n");
}

int run_process_inspection() {
    print_help_common();
    print_help();

    char pattern[MAX_PATTERN_LEN];
    char command[MAX_COMMAND_LEN + MAX_ARG_LEN];

    search_data data;
    process_context ctx = { { nullptr, 0 }, (DWORD)(-1) };

    while (1) {
        printf(">: ");
        input_command cmd = parse_command_common(&ctx.common, &data, command, pattern);
        if (cmd == input_command::c_not_set) {
            cmd = parse_command(&ctx, &data, command, pattern);
        } else {
            puts("");
        }
        if (cmd == c_quit_program) {
            return 0;
        } else if (cmd == c_continue) {
            continue;
        }
        execute_command(cmd, &ctx);
    }

    return 0;
}

static int list_processes() {
    HANDLE hProcessSnap;
    HANDLE hProcess;
    PROCESSENTRY32 pe32;
    DWORD dwPriorityClass;

    // Take a snapshot of all processes in the system.
    hProcessSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hProcessSnap == INVALID_HANDLE_VALUE) {
        print_error(TEXT("CreateToolhelp32Snapshot (of processes)"));
        return(FALSE);
    }

    // Set the size of the structure before using it.
    pe32.dwSize = sizeof(PROCESSENTRY32);

    // Retrieve information about the first process,
    // and exit if unsuccessful
    if (!Process32First(hProcessSnap, &pe32)) {
        print_error(TEXT("Process32First")); // show cause of failure
        CloseHandle(hProcessSnap);          // clean the snapshot object
        return(FALSE);
    }

    // Now walk the snapshot of processes, and
    // display information about each process in turn
    do {
        _tprintf(TEXT("\n\n====================================================="));
        _tprintf(TEXT("\nPROCESS NAME:  %s"), pe32.szExeFile);
        _tprintf(TEXT("\n-------------------------------------------------------"));

        // Retrieve the priority class.
        dwPriorityClass = 0;
        hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pe32.th32ProcessID);
        if (hProcess == NULL) {
            print_error(TEXT("OpenProcess"));
        } else {
            dwPriorityClass = GetPriorityClass(hProcess);
            if (!dwPriorityClass)
                print_error(TEXT("GetPriorityClass"));
            CloseHandle(hProcess);
        }

        _tprintf(TEXT("\n  Process ID        = 0x%08X"), pe32.th32ProcessID);
        _tprintf(TEXT("\n  Thread count      = %d"), pe32.cntThreads);
        _tprintf(TEXT("\n  Parent process ID = 0x%08X"), pe32.th32ParentProcessID);
        _tprintf(TEXT("\n  Priority base     = %d"), pe32.pcPriClassBase);
        if (dwPriorityClass)
            _tprintf(TEXT("\n  Priority class    = %d"), dwPriorityClass);

    } while (Process32Next(hProcessSnap, &pe32));

    CloseHandle(hProcessSnap);
    return(TRUE);
}

static int list_process_modules(DWORD dw_pid) {
    HANDLE hModuleSnap = INVALID_HANDLE_VALUE;
    MODULEENTRY32 me32;

    // Take a snapshot of all modules in the specified process.
    hModuleSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, dw_pid);
    if (hModuleSnap == INVALID_HANDLE_VALUE) {
        print_error(TEXT("CreateToolhelp32Snapshot (of modules)"));
        return(FALSE);
    }

    // Set the size of the structure before using it.
    me32.dwSize = sizeof(MODULEENTRY32);

    // Retrieve information about the first module,
    // and exit if unsuccessful
    if (!Module32First(hModuleSnap, &me32)) {
        print_error(TEXT("Module32First"));  // show cause of failure
        CloseHandle(hModuleSnap);           // clean the snapshot object
        return(FALSE);
    }

    // Now walk the module list of the process,
    // and display information about each module
    do {
        _tprintf(TEXT("\n\n     MODULE NAME:     %s"), me32.szModule);
        _tprintf(TEXT("\n     Executable     = %s"), me32.szExePath);
        _tprintf(TEXT("\n     Process ID     = 0x%08X"), me32.th32ProcessID);
        _tprintf(TEXT("\n     Ref count (g)  = 0x%04X"), me32.GlblcntUsage);
        _tprintf(TEXT("\n     Ref count (p)  = 0x%04X"), me32.ProccntUsage);
        _tprintf(TEXT("\n     Base address   = 0x%08X"), (DWORD)me32.modBaseAddr);
        _tprintf(TEXT("\n     Base size      = 0x%x"), me32.modBaseSize);

    } while (Module32Next(hModuleSnap, &me32));

    puts("\n");

    CloseHandle(hModuleSnap);
    return(TRUE);
}

typedef struct stack_info {
    DWORD_PTR sp;
    SIZE_T size;
} stack_info;

static int get_thread_stack_base(HANDLE hThread, HANDLE process, stack_info *res) {
    CONTEXT context;
    context.ContextFlags = CONTEXT_CONTROL;
    GetThreadContext(hThread, &context);
#ifdef _WIN64
    DWORD_PTR sp = context.Rsp; // Stack pointer for 64-bit
#else
    DWORD_PTR sp = context.Esp; // Stack pointer for 32-bit
#endif

    int stack_base_found = 0;
    MEMORY_BASIC_INFORMATION _info;
    if (VirtualQueryEx(process, (LPCVOID)sp, &_info, sizeof(_info)) == sizeof(_info)) {
        if (_info.State == MEM_COMMIT && _info.Type == MEM_PRIVATE) {
            res->sp = (DWORD_PTR)_info.BaseAddress;
            res->size = _info.RegionSize;
            stack_base_found = 1;
        }
    }

    return stack_base_found;
}

static int list_process_threads(DWORD dw_owner_pid) {
    HANDLE hThreadSnap = INVALID_HANDLE_VALUE;
    THREADENTRY32 te32;
    stack_info si = { NULL, 0 };

    // Take a snapshot of all running threads  
    hThreadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hThreadSnap == INVALID_HANDLE_VALUE)
        return (FALSE);

    // Fill in the size of the structure before using it. 
    te32.dwSize = sizeof(THREADENTRY32);

    // Retrieve information about the first thread,
    // and exit if unsuccessful
    if (!Thread32First(hThreadSnap, &te32)) {
        print_error(TEXT("Thread32First")); // show cause of failure
        CloseHandle(hThreadSnap);
        return (FALSE);
    }

    HANDLE process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, false, dw_owner_pid);
    if (process == NULL) {
        fprintf(stderr, "Failed opening the process. Error code: %lu\n", GetLastError());
        CloseHandle(hThreadSnap);
        return (FALSE);
    }

    // Now walk the thread list of the system,
    // and display information about each thread
    // associated with the specified process
    do {
        if (te32.th32OwnerProcessID == dw_owner_pid) {
            HANDLE hThread = OpenThread(THREAD_ALL_ACCESS, FALSE, te32.th32ThreadID);
            if (hThread != NULL) {
                if (!get_thread_stack_base(hThread, process, &si)) {
                    puts("Failed acquiring stack base!");
                }
                CloseHandle(hThread);
            }

            _tprintf(TEXT("\n\n     THREAD ID         = 0x%08X"), te32.th32ThreadID);
            _tprintf(TEXT("\n     Base priority     = %d"), te32.tpBasePri);
            _tprintf(TEXT("\n     Delta priority    = %d"), te32.tpDeltaPri);
            _tprintf(TEXT("\n     Stack Base        = 0x%p"), si.sp);
            _tprintf(TEXT("\n     Stack Size        = 0x%llx"), si.size);
            _tprintf(TEXT("\n"));
        }
    } while (Thread32Next(hThreadSnap, &te32));

    puts("");

    CloseHandle(process);
    CloseHandle(hThreadSnap);
    return(TRUE);
}

#define NUM_VALUES 0x100

struct entropy_context {
    size_t freq[NUM_VALUES];
};

static void entropy_init(entropy_context * ctx) {
    memset(ctx->freq, 0, sizeof(ctx->freq));
}

static void entropy_calculate_frequencies(entropy_context* ctx, const uint8_t* data, size_t  size) {
    // Calculate frequencies of each byte
    for (size_t i = 0; i < size; i++) {
        ctx->freq[data[i]]++;
    }
}

static double entropy_compute(entropy_context* ctx, size_t size) {
    double entropy = 0.0;
    const double sz = (double)size;
    for (int i = 0; i < NUM_VALUES; i++) {
        if (ctx->freq[i] != 0) {
            double probability = (double)ctx->freq[i] / sz;
            entropy -= probability * log2(probability);
        }
    }

    return entropy;
}

static int traverse_heap_list(DWORD dw_pid, bool list_blocks, bool calculate_entropy) {
    HEAPLIST32 hl;

    HANDLE hHeapSnap = CreateToolhelp32Snapshot(TH32CS_SNAPHEAPLIST, dw_pid);

    hl.dwSize = sizeof(HEAPLIST32);

    if (hHeapSnap == INVALID_HANDLE_VALUE) {
        printf("CreateToolhelp32Snapshot failed (%d)\n", GetLastError());
        return 1;
    }

    if (Heap32ListFirst(hHeapSnap, &hl)) {
        HANDLE process = 0;
        if (calculate_entropy) {
            process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, false, dw_pid);
            if (process == NULL) {
                fprintf(stderr, "Failed opening the process. Error code: %lu\n", GetLastError());
                puts("Entropy won't be computed!");
                calculate_entropy = 0;
            }
        }
        entropy_context e_ctx;
        size_t total_size_blocks = 0;
        do {
            HEAPENTRY32 he;
            ZeroMemory(&he, sizeof(HEAPENTRY32));
            he.dwSize = sizeof(HEAPENTRY32);
            if (Heap32First(&he, dw_pid, hl.th32HeapID)) {
                printf("\n---- Heap ID: 0x%x ----\n", hl.th32HeapID);

                ULONG_PTR start_address = he.dwAddress;
                ULONG_PTR end_address = start_address;
                SIZE_T last_block_size = 0;

                uint8_t* ent_buffer = NULL;
                size_t max_block_size = 0;
                if (calculate_entropy) {
                    entropy_init(&e_ctx);
                }
                do {
                    if (list_blocks) {
                        printf("Start address: 0x%p Block size: 0x%x\n", he.dwAddress, he.dwBlockSize);
                    }

                    if (calculate_entropy) {
                        if (max_block_size < he.dwBlockSize) {
                            max_block_size = he.dwBlockSize;
                            uint8_t* buf = (uint8_t*)realloc(ent_buffer, he.dwBlockSize);
                            if (buf) {
                                ent_buffer = buf;
                            } else {
                                fprintf(stderr, "Buffer allocation faied! Error code: %lu\n", GetLastError());
                                CloseHandle(process);
                                CloseHandle(hHeapSnap);
                                return FALSE;
                            }
                        }
                        SIZE_T bytes_read;
                        if (ReadProcessMemory(process, (LPCVOID)he.dwAddress, (LPVOID)ent_buffer, he.dwBlockSize, &bytes_read) && (bytes_read == he.dwBlockSize)) {
                            entropy_calculate_frequencies(&e_ctx, ent_buffer, he.dwBlockSize);
                            total_size_blocks += he.dwBlockSize;
                        } else {
                            printf("Start address: 0x%p Block size: 0x%x\n", he.dwAddress, he.dwBlockSize);
                            fprintf(stderr, "Failed reading process memory. Error code: %lu\n", GetLastError());
                        }
                    }

                    start_address = _min(start_address, he.dwAddress);
                    if (end_address < he.dwAddress) {
                        end_address = he.dwAddress;
                        last_block_size = he.dwBlockSize;
                    }

                    he.dwSize = sizeof(HEAPENTRY32);
                } while (Heap32Next(&he));
                end_address += last_block_size;
                printf("\nStart Address: 0x%p\n", start_address);
                printf("End Address: 0x%p\n", end_address);
                printf("Size: 0x%llx\n", ptrdiff_t(end_address - start_address));
                if (calculate_entropy) {
                    const double entropy = entropy_compute(&e_ctx, total_size_blocks);
                    printf("Entropy: %.2F\n", entropy);
                    free(ent_buffer);
                }
            }
            hl.dwSize = sizeof(HEAPLIST32);
        } while (Heap32ListNext(hHeapSnap, &hl));

        if (calculate_entropy) {
            CloseHandle(process);
        }
    } else {
        printf("Cannot list first heap (%d)\n", GetLastError());
    }

    puts("");

    CloseHandle(hHeapSnap);

    return 0;
}

static void print_error(TCHAR const* msg) {
    DWORD eNum;
    TCHAR sysMsg[256];
    TCHAR* p;

    eNum = GetLastError();
    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, eNum,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
        sysMsg, 256, NULL);

    // Trim the end of the line and terminate it with a null
    p = sysMsg;
    while ((*p > 31) || (*p == 9))
        ++p;
    do { *p-- = 0; } while ((p >= sysMsg) &&
        ((*p == '.') || (*p < 33)));

    // Display the message
    _tprintf(TEXT("\n  WARNING: %s failed with error %d (%s)"), msg, eNum, sysMsg);
}