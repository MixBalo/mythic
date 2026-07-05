/* proc_child.c — the child side of the S1 CreateProcess smoke test.
 *
 * Does a little real work (heap, virtual memory, a few syscalls) so a
 * "boots but is broken" child fails visibly, prints a marker (may not
 * surface depending on console plumbing), then exits 42 — the value the
 * parent checks for.
 */
#include <windows.h>
#include <stdio.h>

static void out(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    DWORD written;
    int len;
    va_start(ap, fmt);
    len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buf, len, &written, NULL);
}

int main(int argc, char **argv)
{
    void *mem;
    DWORD tick = GetTickCount();

    out("[child-test] alive! pid=%lu argc=%d argv[1]=%s tick=%lu\n",
        (unsigned long)GetCurrentProcessId(), argc,
        argc > 1 ? argv[1] : "(none)", (unsigned long)tick);

    /* Exercise virtual memory + heap in the child's own process context */
    mem = VirtualAlloc(NULL, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem)
    {
        out("[child-test] VirtualAlloc FAILED err=%lu\n", GetLastError());
        return 7;
    }
    memset(mem, 0xAB, 0x10000);
    if (((unsigned char *)mem)[0x8000] != 0xAB)
    {
        out("[child-test] memory readback MISMATCH\n");
        return 8;
    }
    VirtualFree(mem, 0, MEM_RELEASE);

    Sleep(300);
    out("[child-test] done, exiting 42\n");
    return 42;
}
