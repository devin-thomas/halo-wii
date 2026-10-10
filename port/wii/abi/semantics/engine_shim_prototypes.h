#ifndef WII_ABI_ENGINE_SHIM_PROTOTYPES_H
#define WII_ABI_ENGINE_SHIM_PROTOTYPES_H
/* Prototypes for extracted bodies whose original declarations live in
   platform headers the fixture does not include (port/linux/include). */
LONG WINAPI halo_linux_InterlockedIncrement(LPLONG addend);
LONG WINAPI halo_linux_InterlockedDecrement(LPLONG addend);
LONG WINAPI halo_linux_InterlockedExchange(LPLONG target, LONG value);
LONG WINAPI halo_linux_InterlockedExchangeAdd(LPLONG addend, LONG value);
LONG WINAPI halo_linux_InterlockedCompareExchange(LPLONG destination, LONG exchange, LONG comparand);

/* recorded_animation_playback.c's definition (its header declares the
   parameters as void * and struct unit_control_data *). */
boolean recorded_animation_apply_event_stream(struct animation_playback_controller *animation_state,
                                              struct recorded_unit_control *control, long *ticks,
                                              byte const **playback_stream, byte const *playback_stream_end);

/* Authored: engine_exports.h and the playback trace in engine_probe.c. */
word wii_abi_decode_event_header(byte const *stream, byte const *stream_end,
                                 struct animation_event_header *header, word *time_delta);
extern unsigned long wii_abi_playback_damaged;
void wii_abi_playback_trace(unsigned event_type, byte const *position);
#endif
