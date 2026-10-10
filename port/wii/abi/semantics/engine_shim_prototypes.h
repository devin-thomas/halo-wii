#ifndef WII_ABI_ENGINE_SHIM_PROTOTYPES_H
#define WII_ABI_ENGINE_SHIM_PROTOTYPES_H
/* Prototypes for extracted bodies whose original declarations live in
   platform headers the fixture does not include (port/linux/include). */
LONG WINAPI halo_linux_InterlockedIncrement(LPLONG addend);
LONG WINAPI halo_linux_InterlockedDecrement(LPLONG addend);
LONG WINAPI halo_linux_InterlockedExchange(LPLONG target, LONG value);
LONG WINAPI halo_linux_InterlockedExchangeAdd(LPLONG addend, LONG value);
LONG WINAPI halo_linux_InterlockedCompareExchange(LPLONG destination, LONG exchange, LONG comparand);
#endif
