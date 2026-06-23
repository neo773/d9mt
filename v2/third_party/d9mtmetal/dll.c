/* d9mtmetal.dll: PE side. Reimplements the ~15 lines of winecrt0's
 * __wine_init_unix_call so we need no wine SDK: query our own module for
 * the unixlib handle (MemoryWineUnixFuncs) and dispatch through ntdll's
 * __wine_unix_call export.
 */
#define D9MTMETAL_EXPORTS
#include <windows.h>
#include "d9mtmetal.h"

typedef UINT64 unixlib_handle_t;

/* wine-internal MEMORY_INFORMATION_CLASS value, stable since wine 7 */
#define MemoryWineUnixFuncs 1000

extern NTSTATUS WINAPI __wine_unix_call(unixlib_handle_t handle,
                                        unsigned int code, void *args);
extern NTSTATUS WINAPI NtQueryVirtualMemory(HANDLE process, LPCVOID addr,
                                            int info_class, PVOID buffer,
                                            SIZE_T length, SIZE_T *res_len);

static unixlib_handle_t d9mt_unix_handle;

extern IMAGE_DOS_HEADER __ImageBase; /* this dll's own base, like winecrt0 */

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
    /* must run at attach time: wine's builtin bookkeeping for the unixlib
     * is only guaranteed alive while the module load is in progress */
    NtQueryVirtualMemory(GetCurrentProcess(), &__ImageBase,
                         MemoryWineUnixFuncs, &d9mt_unix_handle,
                         sizeof(d9mt_unix_handle), NULL);
  }
  return TRUE;
}

/* lazy so a missing unixlib is reportable instead of failing dll load */
int __cdecl D9MT_UnixCall(unsigned int code, void *params) {
  if (!d9mt_unix_handle) {
    NTSTATUS status = NtQueryVirtualMemory(
        GetCurrentProcess(), &__ImageBase, MemoryWineUnixFuncs,
        &d9mt_unix_handle, sizeof(d9mt_unix_handle), NULL);
    if (status)
      return status;
  }
  return __wine_unix_call(d9mt_unix_handle, code, params);
}
