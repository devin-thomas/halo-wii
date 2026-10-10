/* Authored tail of the generated abi_engine.c: the playback stub's body
   (it needs the extracted event_data_sizes) and an export of the static
   decoder under test. The --regression-base unit (abi_original.c) takes the
   stub only. */

static void wii_abi_playback_apply_stub(struct animation_playback_controller *animation_state,
                                        struct recorded_unit_control *control,
                                        struct animation_event_header const *header,
                                        byte const **playback_stream)
{
    wii_abi_playback_trace(header->event_type, *playback_stream);
    *playback_stream += event_data_sizes[header->event_type];
}

#ifndef WII_ABI_NO_DECODER_EXPORT
word wii_abi_decode_event_header(byte const *stream, byte const *stream_end,
                                 struct animation_event_header *header, word *time_delta)
{
    return recorded_animation_decode_event_header(stream, stream_end, header, time_delta);
}
#endif
