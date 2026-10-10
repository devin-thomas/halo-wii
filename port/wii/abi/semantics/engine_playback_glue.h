/* Authored glue for the extracted recorded_animation_apply_event_stream
   (tools/wii/run_abi_semantics.py includes it in the generated abi_engine.c,
   ahead of the actual `#define apply_funcs` and event_data_sizes). The real
   table's procs apply events to unit control; here every non-NULL entry is
   one stub that traces the applied event (type and stream position) and
   consumes its data as the real procs do (event_data_sizes, in
   engine_exports.h). The NULL entries are the real table's. */

static void wii_abi_playback_apply_stub(struct animation_playback_controller *animation_state,
                                        struct recorded_unit_control *control,
                                        struct animation_event_header const *header,
                                        byte const **playback_stream);

static struct
{
    recorded_animation_apply_proc apply_funcs[23];
} data_002dcf20 =
{
    {
        NULL, NULL,
        wii_abi_playback_apply_stub, wii_abi_playback_apply_stub, wii_abi_playback_apply_stub,
        wii_abi_playback_apply_stub, wii_abi_playback_apply_stub, wii_abi_playback_apply_stub,
        wii_abi_playback_apply_stub, wii_abi_playback_apply_stub, wii_abi_playback_apply_stub,
        wii_abi_playback_apply_stub, wii_abi_playback_apply_stub, wii_abi_playback_apply_stub,
        wii_abi_playback_apply_stub, wii_abi_playback_apply_stub, wii_abi_playback_apply_stub,
        wii_abi_playback_apply_stub, wii_abi_playback_apply_stub, wii_abi_playback_apply_stub,
        wii_abi_playback_apply_stub, wii_abi_playback_apply_stub, wii_abi_playback_apply_stub,
    },
};

/* The source's version reports through errors.h error() once; here each call
   is counted, and the result (FALSE: the stream stops) is the source's. */
static boolean recorded_animation_stream_damaged(void)
{
    wii_abi_playback_damaged++;
    return FALSE;
}
