// WineProcessBridge.m - Initialize Wine's ntdll Unix-side on iOS
// This calls __wine_main() to bootstrap the Wine process, connecting
// to the already-running wineserver thread.

#import <Foundation/Foundation.h>
#import <os/log.h>
#import <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <setjmp.h>

#include "WineProcessBridge.h"
#include "WineServerBridge.h"
#include "PrefixExtractor.h"

// Thread-local globals for wine_ios_exit longjmp (used by wine_ios_exit.h shim in ntdll)
// Each Wine "process" thread has its own jmpbuf so child processes can exit independently.
_Thread_local jmp_buf wine_ios_exit_jmpbuf;
_Thread_local volatile int wine_ios_exit_code = 0;
_Thread_local pthread_t wine_ios_main_thread;
_Thread_local int wine_ios_exit_initialized = 0;

static os_log_t wine_proc_log(void) {
    static os_log_t log;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ log = os_log_create("com.mythic.emulator", "wine-proc"); });
    return log;
}

#define LOG(fmt, ...) os_log(wine_proc_log(), "[WineProc] " fmt, ##__VA_ARGS__)

// Wine's main entry point (from ntdll unix loader.c, statically linked)
extern void __wine_main(int argc, char *argv[]);

// File-based logging (from server_ios.c)
extern void wine_log_set_file(const char *path);

static pthread_t g_wine_thread;
static volatile int g_wine_running = 0;
static char *g_prefix_path = NULL;

static void *wine_process_thread(void *arg) {
    @autoreleasepool {
        LOG("Wine process thread started");

        // Seed prefix from bundled template on first launch (Proton-style).
        // The presence of .update-timestamp means wineboot has already run
        // (either for real or via our pre-seed), so the ntdll-side
        // run_wineboot will skip the launch path.
        {
            NSString *prefix = [NSString stringWithUTF8String:g_prefix_path];
            NSString *stamp = [prefix stringByAppendingPathComponent:@".update-timestamp"];
            NSFileManager *fm = [NSFileManager defaultManager];
            if (![fm fileExistsAtPath:stamp]) {
                NSString *tgz = [[NSBundle mainBundle] pathForResource:@"prefix-template" ofType:@"tar.gz"];
                if (!tgz) {
                    LOG("prefix-template.tar.gz missing from bundle!");
                } else {
                    LOG("Seeding prefix from %{public}s", tgz.UTF8String);
                    if (mythic_extract_prefix_tgz(tgz.UTF8String, g_prefix_path) != 0) {
                        LOG("prefix extraction FAILED");
                    } else {
                        LOG("prefix seeded to %{public}s", g_prefix_path);
                    }
                }
            }

            // (Re)create dosdevices/c: -> ../drive_c. The tarball omits
            // dosdevices because Mac's z: -> / is wrong here.
            NSString *dosdev = [prefix stringByAppendingPathComponent:@"dosdevices"];
            [fm createDirectoryAtPath:dosdev withIntermediateDirectories:YES attributes:nil error:nil];
            NSString *cLink = [dosdev stringByAppendingPathComponent:@"c:"];
            [fm removeItemAtPath:cLink error:nil];
            [fm createSymbolicLinkAtPath:cLink withDestinationPath:@"../drive_c" error:nil];
        }

        // Set environment for Wine
        setenv("WINEPREFIX", g_prefix_path, 1);
        setenv("HOME", g_prefix_path, 1);

        // Skip check_command_line / reexec_loader
        setenv("WINELOADERNOEXEC", "1", 1);

        // Set DLL search path to app bundle (contains aarch64-windows/ with PE DLLs)
        {
            NSString *bundlePath = [[NSBundle mainBundle] bundlePath];
            setenv("WINEDLLPATH", bundlePath.UTF8String, 1);
            LOG("WINEDLLPATH=%{public}s", bundlePath.UTF8String);
        }

        // Debug output
        setenv("WINEDEBUG", "err+all,fixme+all,warn+module,warn+file,trace+process,trace+module,trace+loaddll,trace+loadorder,trace+win,trace+user32,trace+syscall,trace+file", 1);

        // Phase 3D investigation: re-enabled. Investigation C concluded
        // wineserver dispatch is fine; the `ws_log drops at high rate`
        // artifact was the prior false signal. Now chasing a real bug:
        // get_desktop_window's returned HWND fails get_user_object lookup
        // when create_window receives it as req->parent.
        setenv("MYTHIC_WIN32U", "1", 1);

        /* Steam game vars — Thumper queries SteamAppPath dozens of times in init
         * and uses it as base path for asset loading. Prior comment claimed
         * setenv didn't propagate to Wine's GetEnvironmentVariableW, but reading
         * env.c::get_initial_environment shows non-WINE/non-special vars DO
         * pass through (line 915 fall-through). Re-trying this empirically. */
        setenv("SteamAppPath", "C:\\Program Files\\Thumper", 1);
        setenv("SteamGameId", "356400", 1);  // Thumper's Steam app ID
        setenv("SteamAppId",  "356400", 1);
        LOG("setenv check: SteamAppPath=%{public}s SteamGameId=%{public}s",
            getenv("SteamAppPath"), getenv("SteamGameId"));

        /* iOS-Mythic: TSO stays ENABLED (default). The unaligned LDAR/LDAPR/
         * STLR backpatch is now in signal_arm64_ios.c's Mach handler, which
         * replicates FEX's HandleUnalignedAccess (Arm64.cpp:2072) so iOS
         * EXC_BAD_ACCESS faults get the same in-place LDAR→LDR+DMB_LD
         * recovery FEX does for Windows EXCEPTION_DATATYPE_MISALIGNMENT. */

        /* iOS-Mythic: a tiny stub steamclient64.dll is shipped in the game
         * directory (built from /tmp/steamclient_stub/stub.c). It exports
         * just VR_InitInternal (returns NULL) — that's the only function
         * CODEX64.dll imports from steamclient64. The real steamclient64.dll
         * (heavily packed, RWX self-modifying, unwind info v5) was
         * blowing up Wine's loader; the stub lets CODEX bind imports and
         * proceed without OpenVR support. Note: no WINEDLLOVERRIDES needed
         * — we just shipped a different file at the same path. */

        LOG("WINEPREFIX=%{public}s", g_prefix_path);

        // Set up file-based logging for Wine C code
        {
            NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
            NSString *logPath = [docs stringByAppendingPathComponent:@"mythic-log.txt"];
            wine_log_set_file(logPath.UTF8String);
            LOG("Wine log file: %{public}s", logPath.UTF8String);
            /* Expose the app Documents dir to Wine code (e.g. for fex-jit-dump.bin) */
            setenv("MYTHIC_DOCS_DIR", docs.UTF8String, 1);
        }

        // Redirect stderr AND stdout to log file so Wine debug output (WINEDEBUG)
        // and the guest program's printf are both captured.
        {
            NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
            NSString *logPath2 = [docs stringByAppendingPathComponent:@"mythic-log.txt"];
            int logfd = open(logPath2.UTF8String, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (logfd >= 0) {
                dup2(logfd, STDERR_FILENO);
                dup2(logfd, STDOUT_FILENO);
                close(logfd);
            }
        }

        // Pick which exe to run (env var override, default = cube.exe).
        // Set MYTHIC_EXE=hello-x64.exe in env to launch the ARM64EC test path.
        const char *mythic_exe = getenv("MYTHIC_EXE");
        if (!mythic_exe || !*mythic_exe) mythic_exe = "cube.exe";
        // Heuristic: x86_64 guest exes (cube-x64, hello-x64, real games like
        // Thumper) need the arm64ec-windows bundle (ARM64EC hybrid system
        // DLLs that interop with FEX-translated x86_64 code). ARM64-native
        // tests (cube.exe) use the aarch64-windows bundle.
        // MYTHIC_USE_ARM64EC=1 forces the arm64ec path explicitly.
        // Otherwise: detect "x64" in the exe name (cube-x64, fib-x64, etc.)
        // OR a Win32 full path (real game launches typically need ARM64EC).
        const char *force_ec = getenv("MYTHIC_USE_ARM64EC");
        BOOL use_arm64ec = (force_ec && *force_ec == '1') ||
                           (strstr(mythic_exe, "x64") != NULL) ||
                           (strchr(mythic_exe, '\\') != NULL);
        const char *bundle_subdir = use_arm64ec ? "arm64ec-windows" : "aarch64-windows";
        LOG("Target exe: %{public}s (bundle=%{public}s)", mythic_exe, bundle_subdir);
        dprintf(STDERR_FILENO, "[WineProc] Target exe: %s (bundle=%s)\n", mythic_exe, bundle_subdir);

        // Ensure Wine prefix has system32 directory with DLLs from bundle
        {
            NSString *bundlePath = [[NSBundle mainBundle] bundlePath];
            NSString *dllSource = [bundlePath stringByAppendingPathComponent:[NSString stringWithUTF8String:bundle_subdir]];
            NSString *prefix = [NSString stringWithUTF8String:g_prefix_path];
            NSString *sys32Dir = [prefix stringByAppendingPathComponent:@"drive_c/windows/system32"];
            NSFileManager *fm = [NSFileManager defaultManager];

            [fm createDirectoryAtPath:sys32Dir withIntermediateDirectories:YES attributes:nil error:nil];

            NSArray *dlls = [fm contentsOfDirectoryAtPath:dllSource error:nil];
            int linked = 0;
            for (NSString *dll in dlls) {
                NSString *src = [dllSource stringByAppendingPathComponent:dll];
                NSString *dst = [sys32Dir stringByAppendingPathComponent:dll];
                // Remove stale symlinks and re-create (bundle path changes on reinstall)
                [fm removeItemAtPath:dst error:nil];
                if ([fm createSymbolicLinkAtPath:dst withDestinationPath:src error:nil])
                    linked++;
            }
            LOG("Symlinked %d DLLs from %{public}s to %{public}s", linked, bundle_subdir, sys32Dir.UTF8String);
            dprintf(STDERR_FILENO, "[WineProc] Symlinked %d DLLs from %s -> sys32\n", linked, bundle_subdir);

            // Layer Microsoft's real VC++ Runtime DLLs ON TOP of the ARM64EC
            // bundle (only for x86_64 guests). These overwrite Wine's stub
            // builtins — Wine then loads the real MS x86_64 implementation
            // (via FEX) instead of its partial ARM64EC reimplementation.
            //
            // Same pattern Proton/Winlator use: drop in the real concrt140 /
            // msvcp140 / vcruntime140 binaries from VC_redist.x64.exe so games
            // that exercise the full C++ runtime (parallel_for, atomic_wait,
            // <filesystem>, etc.) don't trip __wine_unimplemented stubs.
            if (use_arm64ec) {
                NSString *vcrtSource = [bundlePath stringByAppendingPathComponent:@"x86_64-vcruntime"];
                NSArray *vcrtDlls = [fm contentsOfDirectoryAtPath:vcrtSource error:nil];
                int vcrtLinked = 0, vcrtSkipped = 0;
                for (NSString *dll in vcrtDlls) {
                    /* Keep vcruntime140.dll as the ARM64EC builtin: its
                     * __C_specific_handler is invoked by Wine's SEH dispatch,
                     * and routing that through FEX corrupts x86 RSP (SEH
                     * dispatcher's exit-thunk arg setup is broken). With the
                     * native arm64ec vcruntime140, Wine calls the handler
                     * directly in ARM64 — no FEX bridging on the exception
                     * path. Other vcruntime/msvcp/concrt DLLs still overlay. */
                    if ([[dll lowercaseString] isEqualToString:@"vcruntime140.dll"]) {
                        vcrtSkipped++;
                        continue;
                    }
                    NSString *src = [vcrtSource stringByAppendingPathComponent:dll];
                    NSString *dst = [sys32Dir stringByAppendingPathComponent:dll];
                    [fm removeItemAtPath:dst error:nil];
                    if ([fm createSymbolicLinkAtPath:dst withDestinationPath:src error:nil])
                        vcrtLinked++;
                }
                LOG("Symlinked %d MS VC++ Runtime DLLs (x86_64 native) over arm64ec builtins, skipped %d", vcrtLinked, vcrtSkipped);
                dprintf(STDERR_FILENO, "[WineProc] Symlinked %d MS VC++ Runtime DLLs over arm64ec builtins (skipped %d for native EC SEH)\n", vcrtLinked, vcrtSkipped);
            }
        }

        // Build the launch path for Wine's PE loader.
        // If MYTHIC_EXE contains a backslash or starts with a drive letter
        // (e.g. "C:\\Program Files\\Thumper\\THUMPER_win10.exe"), use it
        // as-is. Otherwise treat it as a bare exe name in system32 (legacy
        // path used by cube/fib/hello tests).
        char exe_path[512];
        if (strchr(mythic_exe, '\\') || (mythic_exe[0] && mythic_exe[1] == ':')) {
            snprintf(exe_path, sizeof(exe_path), "%s", mythic_exe);
        } else {
            snprintf(exe_path, sizeof(exe_path), "C:\\windows\\system32\\%s", mythic_exe);
        }

        // Optional MYTHIC_ARGS env var: space-separated args appended to argv.
        // Tokenized in-place; max 16 extra tokens.
        static char args_buf[1024];
        char *extra_argv[16] = {0};
        int extra_argc = 0;
        const char *mythic_args = getenv("MYTHIC_ARGS");
        if (mythic_args && *mythic_args) {
            strncpy(args_buf, mythic_args, sizeof(args_buf) - 1);
            args_buf[sizeof(args_buf) - 1] = 0;
            char *saveptr = NULL;
            for (char *tok = strtok_r(args_buf, " ", &saveptr);
                 tok && extra_argc < 16;
                 tok = strtok_r(NULL, " ", &saveptr)) {
                extra_argv[extra_argc++] = tok;
            }
        }

        char *argv[24];
        int argc = 0;
        argv[argc++] = "wine";
        argv[argc++] = exe_path;
        for (int i = 0; i < extra_argc; i++) argv[argc++] = extra_argv[i];
        argv[argc] = NULL;
        dprintf(STDERR_FILENO, "[WineProc] argv[1] = %s\n", exe_path);
        for (int i = 0; i < extra_argc; i++) {
            dprintf(STDERR_FILENO, "[WineProc] argv[%d] = %s\n", 2 + i, extra_argv[i]);
        }

        /* iOS-Mythic: chdir to the unix path that maps to the exe's Wine
         * directory BEFORE __wine_main. Wine inherits the iOS app sandbox
         * cwd, which becomes a `unix\private\var\mobile\...\Documents\wine\`
         * Wine path — and Thumper's relative cache opens (e.g.,
         * "cache/721e72f7.pc") then resolve to doubled paths that don't
         * exist. Per GPT diagnosis 2026-05-12. Only chdir for full-path EXE
         * launches; bare-name launches (cube, hello-x64) use C:\windows\system32. */
        if (strchr(mythic_exe, '\\') || (mythic_exe[0] && mythic_exe[1] == ':')) {
            /* Convert "C:\Program Files\Thumper\X.exe" → unix path */
            char unix_dir[1024];
            const char *drive_c = "drive_c";
            const char *after_drive = mythic_exe + 3; /* skip "C:\" */
            char *last_sep = strrchr(mythic_exe, '\\');
            if (last_sep && last_sep > mythic_exe + 3) {
                /* Get "Program Files\Thumper" from "C:\Program Files\Thumper\X.exe" */
                size_t dir_len = (size_t)(last_sep - after_drive);
                char windir[512];
                memcpy(windir, after_drive, dir_len);
                windir[dir_len] = 0;
                /* Translate backslashes to forward slashes */
                for (char *p = windir; *p; p++) if (*p == '\\') *p = '/';
                snprintf(unix_dir, sizeof(unix_dir), "%s/%s/%s",
                         g_prefix_path, drive_c, windir);
                int rc = chdir(unix_dir);
                setenv("PWD", unix_dir, 1);
                /* Also set the iOS-specific override so env_ios.c's
                 * get_initial_directory bypasses unix_to_nt_file_name (which
                 * fails to resolve drive_c via dosdevices on iOS). */
                char wine_cwd[768];
                /* Strip trailing exe name from mythic_exe to get the dir part */
                {
                    const char *exe = mythic_exe;
                    size_t dir_len = (size_t)(last_sep - exe);
                    if (dir_len < sizeof(wine_cwd) - 2) {
                        memcpy(wine_cwd, exe, dir_len);
                        wine_cwd[dir_len] = '\\';
                        wine_cwd[dir_len + 1] = 0;
                        setenv("MYTHIC_INITIAL_CWD", wine_cwd, 1);
                    }
                }
                dprintf(STDERR_FILENO, "[WineProc] chdir(%s) = %d errno=%d, PWD + MYTHIC_INITIAL_CWD=%s\n",
                        unix_dir, rc, rc ? errno : 0, wine_cwd);
            }
        }

        // Record this thread so wine_ios_exit knows where to longjmp
        wine_ios_main_thread = pthread_self();
        wine_ios_exit_initialized = 1;

        LOG("Calling __wine_main...");

        if (setjmp(wine_ios_exit_jmpbuf) == 0) {
            __wine_main(argc, argv);
            dprintf(STDERR_FILENO, "[WineProc] __wine_main returned normally\n");
        } else {
            dprintf(STDERR_FILENO, "[WineProc] Wine exited with code %d (caught by longjmp)\n", wine_ios_exit_code);
        }

        g_wine_running = 0;

        // Stop wineserver to prevent CPU spin (iOS kills for excessive CPU)
        dprintf(STDERR_FILENO, "[WineProc] stopping wineserver...\n");
        wineserver_stop();

        dprintf(STDERR_FILENO, "[WineProc] Wine process thread finished cleanly\n");
    }
    return NULL;
}

int wine_process_start(const char *prefix_path) {
    if (g_wine_running) {
        LOG("Wine process already running");
        return 0;
    }

    if (g_prefix_path) free(g_prefix_path);
    g_prefix_path = strdup(prefix_path);

    LOG("Starting Wine process with prefix: %{public}s", prefix_path);

    g_wine_running = 1;

    // Create socketpair to bypass broken iOS UDS accept()
    // pair[0] = wineserver side (injected as client fd)
    // pair[1] = ntdll side (used as fd_socket)
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == -1) {
        LOG("socketpair failed: %{public}s", strerror(errno));
        g_wine_running = 0;
        return -1;
    }
    LOG("socketpair created: server_fd=%d, client_fd=%d", pair[0], pair[1]);

    // Set env var for ntdll to pick up instead of server_connect()
    // Must use WINESERVERSOCKET — that's what Wine's server_init_process() checks
    char fd_str[16];
    snprintf(fd_str, sizeof(fd_str), "%d", pair[1]);
    setenv("WINESERVERSOCKET", fd_str, 1);

    // Inject wineserver side — the event loop will pick this up
    wineserver_inject_client_fd(pair[0]);

    // Lower priority so Wine init doesn't starve the main thread
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    struct sched_param sched = { .sched_priority = 20 };  // lower than default (31)
    pthread_attr_setschedparam(&attr, &sched);

    int ret = pthread_create(&g_wine_thread, &attr, wine_process_thread, NULL);
    pthread_attr_destroy(&attr);
    if (ret != 0) {
        LOG("Failed to create Wine process thread: %d", ret);
        close(pair[0]);
        close(pair[1]);
        g_wine_running = 0;
        return -1;
    }

    pthread_detach(g_wine_thread);
    LOG("Wine process thread created");
    return 0;
}

int wine_process_is_running(void) {
    return g_wine_running;
}
