#include "packet_shim.h"
#include "packet_version_edges.h"

enum { VERSION_REGION_BYTES = 768, VERSION_EXPECTED_CHECKS = 2912 };
union version_storage { uint64_t alignment; byte bytes[VERSION_REGION_BYTES]; };
struct version_case {
    const char *name;
    short type, count, native_size;
    unsigned int placeholder_size, wire_size;
    byte golden[6];
};
/* Wire quantities are explicit: excluded multibyte scalars emit count bytes. */
static const struct version_case version_cases[] = {
    {"bytes_count3", _data_packet_field_bytes, 3, 3, 3, 6, {1,0xa9,0,0,0,0xe7}},
    {"shorts_count3", _data_packet_field_shorts, 3, 6, 3, 6, {1,0xa9,0,0,0,0xe7}},
    {"longs_count3", _data_packet_field_longs, 3, 12, 3, 6, {1,0xa9,0,0,0,0xe7}},
    {"int64s_count3", _data_packet_field_int64s, 3, 24, 3, 6, {1,0xa9,0,0,0,0xe7}},
    {"raw_count3", _data_packet_field_raw, 3, 3, 3, 6, {1,0xa9,0,0,0,0xe7}},
    {"string_max4", _data_packet_field_string, 4, 5, 1, 4, {1,0xa9,0,0xe7}},
    {"data_max255", _data_packet_field_data, 255, 257, 1, 4, {1,0xa9,0,0xe7}},
    {"data_max256", _data_packet_field_data, 256, 258, 2, 5, {1,0xa9,0,0,0xe7}},
    {"pad_count3", _data_packet_field_pad, 3, 3, 0, 3, {1,0xa9,0xe7}}
};
struct version_context {
    FILE *report;
    const struct version_case *test;
    const char *mode;
    unsigned int offset, input_size, checks, failures;
    int collect;
};

static int version_check(struct version_context *ctx, int condition, const char *assertion)
{
    ++ctx->checks;
    if (condition) return 1;
    ++ctx->failures;
    if (fprintf(ctx->report,
                "POLICY EDGES FAIL case=%s mode=%s assertion=%s offset=%u input_size=%u check=%u\n",
                ctx->test->name, ctx->mode, assertion, ctx->offset, ctx->input_size, ctx->checks) < 0 ||
        fflush(ctx->report) != 0) return 0;
    return ctx->collect;
}
#define CHECK(condition, name) do { if (!version_check(ctx, (condition), name)) return 1; } while (0)
#define INCLUDED(type, count) { type, count, 0, 0, 0 }
#define END_FIELD INCLUDED(_data_packet_field_end, 0)

static int version_wire(struct version_context *ctx, const byte *wire, short used)
{
    unsigned int bounded = used < 0 ? 0U : (unsigned int)used;
    if (bounded > 16) bounded = 16;
    if (fprintf(ctx->report,
                "POLICY EDGES WIRE case=%s offset=%u count=%d native_extent=%d placeholder_bytes=%u length=%d actual=",
                ctx->test->name, ctx->offset, (int)ctx->test->count, (int)ctx->test->native_size,
                ctx->test->placeholder_size, (int)used) < 0) return 1;
    for (unsigned int i=0; i<bounded; ++i)
        if (fprintf(ctx->report, "%02x", wire[i]) < 0) return 1;
    if (fprintf(ctx->report, " golden=") < 0) return 1;
    for (unsigned int i=0; i<ctx->test->wire_size; ++i)
        if (fprintf(ctx->report, "%02x", ctx->test->golden[i]) < 0) return 1;
    return fprintf(ctx->report, "\n") < 0 || fflush(ctx->report) != 0;
}

static int version_decode_valid(struct version_context *ctx,
                                struct data_packet_definition *definition, unsigned int variant)
{
    union version_storage encoded, decoded;
    byte expected[VERSION_REGION_BYTES], snapshot[VERSION_REGION_BYTES];
    const struct version_case *test=ctx->test;
    short version=-1, consumed=-1;
    unsigned int input_size=test->wire_size+(variant==2 ? 2U : 0U);
    ctx->mode=variant==0 ? "zero_placeholders" : variant==1 ? "nonzero_placeholders_accepted" : "trailing_bytes";
    ctx->input_size=input_size;
    memset(encoded.bytes,0xcc,sizeof(encoded.bytes));
    memcpy(encoded.bytes+ctx->offset,test->golden,test->wire_size);
    if (variant==1)
        for (unsigned int i=0; i<test->placeholder_size; ++i)
            encoded.bytes[ctx->offset+2+i]=(byte)(0x71+i);
    if (variant==2) {
        encoded.bytes[ctx->offset+test->wire_size]=0x91;
        encoded.bytes[ctx->offset+test->wire_size+1]=0x92;
    }
    memcpy(snapshot,encoded.bytes,sizeof(snapshot));
    memset(decoded.bytes,0xcc,sizeof(decoded.bytes));
    memset(expected,0xcc,sizeof(expected)); expected[0]=0xa9;
    memset(expected+1,0,(size_t)test->native_size); expected[1+test->native_size]=0xe7;
    CHECK(policy_packet_decode(definition,encoded.bytes+ctx->offset,(short)input_size,
                               decoded.bytes,&version,&consumed), "decode_success");
    CHECK(version==1 && consumed==(short)test->wire_size, "version_and_exact_consumption");
    CHECK(memcmp(decoded.bytes,expected,sizeof(expected))==0, "whole_native_extent_and_canaries");
    CHECK(memcmp(encoded.bytes,snapshot,sizeof(snapshot))==0, "whole_wire_unchanged");
    return 0;
}

static int version_truncations(struct version_context *ctx,
                               struct data_packet_definition *definition)
{
    const struct version_case *test=ctx->test;
    union version_storage encoded, decoded;
    byte expected[VERSION_REGION_BYTES], snapshot[VERSION_REGION_BYTES];
    ctx->mode="every_truncation_boundary";
    for (unsigned int length=0; length<test->wire_size; ++length) {
        short version=-1, consumed=-1;
        unsigned int prior=length==0 ? 0U : length==1 ? 1U : 2U;
        int placeholder_completed=length>=2+test->placeholder_size;
        ctx->input_size=length;
        /* Bytes beyond the declared input remain available as checked canaries. */
        memset(encoded.bytes,0xcc,sizeof(encoded.bytes));
        memcpy(encoded.bytes+ctx->offset,test->golden,test->wire_size);
        memcpy(snapshot,encoded.bytes,sizeof(snapshot));
        memset(decoded.bytes,0xcc,sizeof(decoded.bytes)); memset(expected,0xcc,sizeof(expected));
        if (length>=2) expected[0]=0xa9;
        if (placeholder_completed) {
            memset(expected+1,0,(size_t)test->native_size);
            prior=2+test->placeholder_size;
        }
        CHECK(!policy_packet_decode(definition,encoded.bytes+ctx->offset,(short)length,
                                    decoded.bytes,&version,&consumed), "truncated_decode_false");
        CHECK(version==(length==0 ? 0 : 1) && consumed==(short)prior,
              "cursor_retains_only_completed_wire");
        CHECK(memcmp(decoded.bytes,expected,sizeof(expected))==0,
              "failed_current_field_and_following_tail_untouched");
        CHECK(memcmp(encoded.bytes,snapshot,sizeof(snapshot))==0, "truncated_whole_wire_unchanged");
    }
    return 0;
}

static int version_sticky(struct version_context *ctx)
{
    const struct version_case *test=ctx->test;
    struct data_packet_field fields[]={
        {test->type,test->count,2,0,0}, INCLUDED(_data_packet_field_bytes,1), END_FIELD
    };
    struct data_packet_definition definition={"authored_sticky_version",0,(short)(test->native_size+1),2,fields,FALSE};
    union version_storage encoded,decoded;
    byte snapshot[VERSION_REGION_BYTES],expected[VERSION_REGION_BYTES];
    struct data_encoding_state state;
    ctx->mode="sticky_before_excluded_field"; ctx->input_size=16;
    candidate_packet_verify(&definition);
    memset(encoded.bytes,0xcc,sizeof(encoded.bytes)); memcpy(snapshot,encoded.bytes,sizeof(snapshot));
    memset(decoded.bytes,0xcc,sizeof(decoded.bytes)); memset(expected,0xcc,sizeof(expected));
    data_decode_new(&state,encoded.bytes+ctx->offset,16); state.offset=1; state.overflow=TRUE;
    packet_policy_dispatch_decode(&definition,&state,1,decoded.bytes);
    CHECK(state.overflow && state.offset==1 && state.buffer==encoded.bytes+ctx->offset && state.buffer_size==16,
          "sticky_state_and_cursor_unchanged");
    CHECK(memcmp(decoded.bytes,expected,sizeof(expected))==0, "sticky_excluded_pad_and_tail_untouched");
    CHECK(memcmp(encoded.bytes,snapshot,sizeof(snapshot))==0, "sticky_whole_wire_unchanged");
    return 0;
}

static int version_one_case(struct version_context *ctx)
{
    const struct version_case *test=ctx->test;
    struct data_packet_field fields[]={
        INCLUDED(_data_packet_field_raw,1), {test->type,test->count,2,0,0},
        INCLUDED(_data_packet_field_bytes,1), END_FIELD
    };
    struct data_packet_definition definition={"authored_version_edges",0,(short)(test->native_size+2),2,fields,FALSE};
    union version_storage native,encoded;
    byte expected[VERSION_REGION_BYTES],snapshot[VERSION_REGION_BYTES];
    short used=-1;
    ctx->mode="original_encoder_zero_placeholders"; ctx->input_size=16;
    memset(native.bytes,0xa5,sizeof(native.bytes)); native.bytes[0]=0xa9;
    native.bytes[1+test->native_size]=0xe7;
    memcpy(snapshot,native.bytes,sizeof(snapshot));
    candidate_packet_verify(&definition);
    CHECK(definition.initialized && fields[0].size==1 && fields[1].size==test->native_size && fields[2].size==1,
          "definition_all_fields_eligible_native_extent");
    memset(encoded.bytes,0xcc,sizeof(encoded.bytes));
    CHECK(candidate_packet_encode(&definition,1,native.bytes,encoded.bytes+ctx->offset,&used,16),
          "preserved_encoder_success");
    CHECK(used==(short)test->wire_size, "preserved_encoder_wire_length");
    memset(expected,0xcc,sizeof(expected)); memcpy(expected+ctx->offset,test->golden,test->wire_size);
    CHECK(memcmp(encoded.bytes,expected,sizeof(expected))==0, "independent_zero_golden_and_whole_wire_canaries");
    CHECK(memcmp(native.bytes,snapshot,sizeof(snapshot))==0, "preserved_encoder_whole_source_unchanged");
    if (version_wire(ctx,encoded.bytes+ctx->offset,used)) return 1;
    for (unsigned int variant=0; variant<3; ++variant)
        if (version_decode_valid(ctx,&definition,variant)) return 1;
    if (version_truncations(ctx,&definition) || version_sticky(ctx)) return 1;
    return 0;
}

int wii_packet_version_edges(FILE *report, int collect_failures)
{
    struct version_context context={report,NULL,"",0,0,0,0,collect_failures};
    struct version_context *ctx=&context;
    int aborted=0;
    if (fprintf(report,
                "POLICY EDGES BEGIN expected_checks=%u cases=9 wire_offsets=0..7 definition_version=2 wire_version=1\n",
                (unsigned int)VERSION_EXPECTED_CHECKS)<0 || fflush(report)!=0) return 1;
    for (ctx->offset=0; ctx->offset<8; ++ctx->offset) {
        for (unsigned int n=0; n<sizeof(version_cases)/sizeof(version_cases[0]); ++n) {
            ctx->test=&version_cases[n];
            if (version_one_case(ctx)) { aborted=1; goto done; }
        }
    }
    if (ctx->checks!=VERSION_EXPECTED_CHECKS) aborted=1;
done:
    if (fprintf(report,
                "POLICY EDGES SUMMARY checks=%u expected_checks=%u failures=%u offsets_completed=%u aborted=%d\nPOLICY EDGES END result=%d\n",
                ctx->checks,(unsigned int)VERSION_EXPECTED_CHECKS,ctx->failures,ctx->offset,aborted,
                aborted || ctx->failures!=0)<0 || fflush(report)!=0) return 1;
    return aborted || ctx->failures!=0;
}
#undef CHECK
#undef INCLUDED
#undef END_FIELD
