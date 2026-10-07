#include "packet_shim.h"
#include "packet_fixture.h"

/* Native prefixes stay aligned; only encoded storage is offset by 0..7. */
enum { PACKET_REGION_BYTES = 768 };
union packet_storage { uint64_t alignment; byte bytes[PACKET_REGION_BYTES]; };
/* Flat array: csstrcpy compares pointers within this one array object. */
union packet_decode_storage { uint64_t alignment; byte bytes[2 * PACKET_REGION_BYTES]; };
_Static_assert(PACKET_REGION_BYTES % _Alignof(uint64_t) == 0, "Aligned native decode region");
struct packet_ops {
    const char *name;
    void (*verify)(struct data_packet_definition *);
    boolean (*encode)(struct data_packet_definition *, long, const void *, void *, short *, short);
    boolean (*decode)(struct data_packet_definition *, const void *, short, void *, short *, short *);
    void (*dispatch)(struct data_packet_definition *, struct data_encoding_state *, short, void *);
};
struct packet_context {
    FILE *report;
    const struct packet_ops *ops;
    const char *test;
    unsigned int offset, checks, failures;
    int collect;
};

static int packet_check(struct packet_context *ctx, int condition, const char *name)
{
    ++ctx->checks;
    if (condition) return 1;
    ++ctx->failures;
    if (fprintf(ctx->report, "PACKET %s FAIL case=%s assertion=%s offset=%u check=%u\n",
                ctx->ops->name, ctx->test, name, ctx->offset, ctx->checks) < 0 ||
        fflush(ctx->report) != 0) return 0;
    return ctx->collect;
}
#define CHECK(condition, name) do { if (!packet_check(ctx, (condition), name)) return 1; } while (0)
#define FIELD(type, count) { type, count, 0, 0, 0 }
#define END_FIELD FIELD(_data_packet_field_end, 0)

static int packet_wire(struct packet_context *ctx, const byte *actual, short length,
                       const byte *golden, unsigned int golden_length)
{
    /* A bad length is reported verbatim, but never controls an unchecked read. */
    unsigned int bounded = length < 0 ? 0U : (unsigned int)length;
    if (bounded > 300) bounded = 300;
    if (fprintf(ctx->report, "PACKET %s WIRE case=%s offset=%u length=%d actual=",
                ctx->ops->name, ctx->test, ctx->offset, (int)length) < 0) return 1;
    for (unsigned int i = 0; i < bounded; ++i)
        if (fprintf(ctx->report, "%02x", actual[i]) < 0) return 1;
    if (fprintf(ctx->report, " golden=") < 0) return 1;
    for (unsigned int i = 0; i < golden_length; ++i)
        if (fprintf(ctx->report, "%02x", golden[i]) < 0) return 1;
    return fprintf(ctx->report, "\n") < 0 || fflush(ctx->report) != 0;
}

static int current_packet(struct packet_context *ctx, unsigned int variant)
{
    struct data_packet_field fields[] = {
        FIELD(_data_packet_field_shorts, 2), FIELD(_data_packet_field_longs, 1),
        FIELD(_data_packet_field_int64s, 1), FIELD(_data_packet_field_raw, 3),
        FIELD(_data_packet_field_string, 4), FIELD(_data_packet_field_data, 4),
        FIELD(_data_packet_field_array, 2), FIELD(_data_packet_field_shorts, 1),
        END_FIELD, FIELD(_data_packet_field_bytes, 2), FIELD(_data_packet_field_pad, 2), END_FIELD
    };
    struct data_packet_definition definition = {"authored_current", 0, 40, 1, fields, FALSE};
    /* Independent hand-authored goldens: version, scalars, raw, variable fields, tail. */
    static const byte goldens[][37] = {
        {1,0x12,0x34,0xff,0xfe,0x89,0xab,0xcd,0xef,0xfe,0xdc,0xba,0x98,0x76,0x54,0x32,0x10,
         0xa0,0xa1,0xa2,0,0,0,0xee,0xff},
        {1,0x12,0x34,0xff,0xfe,0x89,0xab,0xcd,0xef,0xfe,0xdc,0xba,0x98,0x76,0x54,0x32,0x10,
         0xa0,0xa1,0xa2,'h','i',0,1,0xd0,1,0x56,0x78,0xee,0xff},
        {1,0x12,0x34,0xff,0xfe,0x89,0xab,0xcd,0xef,0xfe,0xdc,0xba,0x98,0x76,0x54,0x32,0x10,
         0xa0,0xa1,0xa2,'w','x','y','z',0,2,0xd0,0xd1,2,0x56,0x78,0x9a,0xbc,0xee,0xff},
        {1,0x12,0x34,0xff,0xfe,0x89,0xab,0xcd,0xef,0xfe,0xdc,0xba,0x98,0x76,0x54,0x32,0x10,
         0xa0,0xa1,0xa2,'h','i',0,3,0xd0,0xd1,0xd2,1,0x56,0x78,0xee,0xff},
        {1,0x12,0x34,0xff,0xfe,0x89,0xab,0xcd,0xef,0xfe,0xdc,0xba,0x98,0x76,0x54,0x32,0x10,
         0xa0,0xa1,0xa2,'w','x','y','z',0,4,0xd0,0xd1,0xd2,0xd3,2,0x56,0x78,0x9a,0xbc,0xee,0xff}
    };
    static const unsigned int lengths[] = {25,30,35,32,37};
    static const short array_counts[] = {0,1,2,1,2};
    static const char *strings[] = {"", "hi", "wxyz", "hi", "wxyz"};
    static const unsigned int string_lengths[] = {0,2,4,2,4};
    const uint16_t shorts[] = {0x1234,0xfffe}, children[] = {0x5678,0x9abc};
    const uint32_t number = UINT32_C(0x89abcdef);
    const uint64_t large = UINT64_C(0xfedcba9876543210);
    short data_count = (short)variant, child_count = array_counts[variant];
    union packet_storage native;
    union packet_decode_storage backing;
    byte *encoded = backing.bytes, *decoded = backing.bytes + PACKET_REGION_BYTES;
    byte source_snapshot[768], expected[768], wire_expected[768];
    unsigned int length = lengths[variant], start = ctx->offset;
    short used = -1, version = -1, consumed = -1;
    ctx->test = "current_variable";
    memset(native.bytes, 0xa5, sizeof(native.bytes));
    memcpy(native.bytes, shorts, 4); memcpy(native.bytes + 4, &number, 4);
    memcpy(native.bytes + 8, &large, 8);
    native.bytes[16]=0xa0; native.bytes[17]=0xa1; native.bytes[18]=0xa2;
    memcpy(native.bytes + 19, strings[variant], string_lengths[variant] + 1);
    memcpy(native.bytes + 24, &data_count, 2);
    for (unsigned int i=0; i<variant; ++i) native.bytes[26+i]=(byte)(0xd0+i);
    memcpy(native.bytes + 30, &child_count, 2); memcpy(native.bytes + 32, children, 4);
    native.bytes[36]=0xee; native.bytes[37]=0xff;
    memcpy(source_snapshot, native.bytes, sizeof(source_snapshot));
    ctx->ops->verify(&definition);
    CHECK(definition.initialized && fields[0].size==4 && fields[4].size==5 &&
          fields[5].size==6 && fields[6].size==0 && fields[7].size==2 &&
          fields[8].size==6 && fields[9].size==2 && fields[10].size==2, "verified_layout_and_array_end_size");
    memset(encoded, 0xcc, PACKET_REGION_BYTES);
    CHECK(ctx->ops->encode(&definition, 1, native.bytes, encoded+start, &used, 64), "encode_success");
    CHECK(used==(short)length, "encode_wire_length");
    memset(expected, 0xcc, sizeof(expected)); memcpy(expected+start, goldens[variant], length);
    CHECK(memcmp(encoded, expected, sizeof(expected))==0, "independent_wire_and_all_canaries");
    CHECK(memcmp(native.bytes, source_snapshot, sizeof(source_snapshot))==0, "encode_source_unchanged");
    if (packet_wire(ctx, encoded+start, used, goldens[variant], length)) return 1;

    /* Decode a fresh golden, never encoder output. Preserve unused native reserves. */
    memset(encoded, 0xcc, PACKET_REGION_BYTES); memcpy(encoded+start, goldens[variant], length);
    memcpy(wire_expected, encoded, sizeof(wire_expected));
    memcpy(wire_expected+start+1, native.bytes, 16);
    unsigned int child_wire = 20 + string_lengths[variant] + 1 + 1 + variant + 1;
    memcpy(wire_expected+start+child_wire, children, (size_t)child_count*2);
    memset(decoded, 0xcc, PACKET_REGION_BYTES); memset(expected, 0xcc, sizeof(expected));
    memcpy(expected, native.bytes, 19); memcpy(expected+19, strings[variant], string_lengths[variant]+1);
    memcpy(expected+24, &data_count, 2); memcpy(expected+26, native.bytes+26, variant);
    memcpy(expected+30, &child_count, 2); memcpy(expected+32, children, (size_t)child_count*2);
    expected[36]=0xee; expected[37]=0xff;
    CHECK(ctx->ops->decode(&definition, encoded+start, (short)length, decoded, &version, &consumed), "golden_decode_success");
    CHECK(version==1 && consumed==(short)length, "decode_version_and_wire_consumption");
    CHECK(memcmp(decoded, expected, sizeof(expected))==0, "native_output_reserves_and_canaries");
    CHECK(memcmp(encoded, wire_expected, sizeof(wire_expected))==0, "in_place_scalar_mutation_and_canaries");

    encoded[start+length]=0x71; encoded[start+length+1]=0x72;
    /* Reset because the preceding decode mutated its input. */
    memcpy(encoded+start, goldens[variant], length);
    memset(decoded, 0xcc, PACKET_REGION_BYTES);
    CHECK(ctx->ops->decode(&definition, encoded+start, (short)(length+2), decoded, &version, &consumed) &&
          consumed==(short)length && encoded[start+length]==0x71 && encoded[start+length+1]==0x72,
          "trailing_wire_bytes_accepted_unconsumed");
    CHECK(memcmp(decoded, expected, sizeof(expected))==0, "trailing_case_native_output");

    memset(encoded, 0xcc, PACKET_REGION_BYTES); memcpy(encoded+start, goldens[variant], length-1);
    memcpy(wire_expected, encoded, sizeof(wire_expected));
    memcpy(wire_expected+start+1, native.bytes, 16);
    memcpy(wire_expected+start+child_wire, children, (size_t)child_count*2);
    memset(decoded, 0xcc, PACKET_REGION_BYTES); expected[36]=0xcc; expected[37]=0xcc;
    CHECK(!ctx->ops->decode(&definition, encoded+start, (short)(length-1), decoded, &version, &consumed),
          "late_tail_truncation_false");
    CHECK(version==1 && consumed==(short)(length-2), "late_tail_truncation_partial_consumption");
    CHECK(memcmp(decoded, expected, sizeof(expected))==0, "late_tail_truncation_retains_prior_native_fields");
    CHECK(memcmp(encoded, wire_expected, sizeof(wire_expected))==0, "late_tail_truncation_retains_prior_input_mutation");

    memset(encoded, 0xcc, PACKET_REGION_BYTES);
    used=-1;
    CHECK(!ctx->ops->encode(&definition, 1, native.bytes, encoded+start, &used, (short)(length-1)), "encode_truncated_false");
    CHECK(used==(short)(length-2), "encode_partial_prefix_length");
    /* Both scalar services preserve prior writes; their partial wire endian differs. */
    memset(expected, 0xcc, sizeof(expected));
    memcpy(expected+start, goldens[variant], length-2);
    if (ctx->ops->name[0]=='R') {
        const byte *input=native.bytes;
        unsigned int at=start+1;
        const unsigned int widths[]={2,2,4,8};
        for (unsigned int n=0; n<4; ++n) {
            for (unsigned int i=0; i<widths[n]; ++i) expected[at+i]=input[widths[n]-i-1];
            input+=widths[n]; at+=widths[n];
        }
        for (short n=0; n<child_count; ++n) {
            const byte *child=(const byte *)children+2*n;
            expected[start+child_wire+2*n]=child[1]; expected[start+child_wire+2*n+1]=child[0];
        }
    }
    CHECK(memcmp(encoded, expected, sizeof(expected))==0, "encode_retains_only_completed_fields");
    CHECK(memcmp(native.bytes, source_snapshot, sizeof(source_snapshot))==0, "truncated_encode_source_unchanged");
    return 0;
}

static int length_packet(struct packet_context *ctx, int array, short count)
{
    struct data_packet_field data_fields[]={FIELD(_data_packet_field_data,256),FIELD(_data_packet_field_bytes,2),END_FIELD};
    struct data_packet_field array_fields[]={FIELD(_data_packet_field_array,256),FIELD(_data_packet_field_bytes,1),END_FIELD,
                                             FIELD(_data_packet_field_bytes,2),END_FIELD};
    struct data_packet_definition definition={"authored_signed_length",0,260,0,array ? array_fields : data_fields,FALSE};
    union packet_storage native, encoded, decoded;
    byte expected[768], snapshot[768], golden[260], wire_expected[768];
    unsigned int start=ctx->offset, length=4+(unsigned int)count;
    short used=-1, version=-1, consumed=-1;
    ctx->test=array ? "array_signed16_valid" : "data_signed16_valid";
    memset(native.bytes,0xa5,sizeof(native.bytes)); memcpy(native.bytes,&count,2);
    memset(native.bytes+2,0xd3,(size_t)count); native.bytes[258]=0xee; native.bytes[259]=0xff;
    memcpy(snapshot,native.bytes,sizeof(snapshot));
    /* Independent wire pattern: fixed hex prefix/payload/tail, not encoder-derived. */
    golden[0]=count==256 ? 1 : 0; golden[1]=count==1 ? 1 : 0;
    memset(golden+2,0xd3,(size_t)count); golden[2+count]=0xee; golden[3+count]=0xff;
    ctx->ops->verify(&definition);
    CHECK(definition.initialized && (array ? array_fields[2].size==258 : data_fields[0].size==258), "signed16_native_reserve");
    memset(encoded.bytes,0xcc,sizeof(encoded.bytes));
    CHECK(ctx->ops->encode(&definition,0,native.bytes,encoded.bytes+start,&used,300), "signed16_encode_success");
    memset(expected,0xcc,sizeof(expected)); memcpy(expected+start,golden,length);
    CHECK(used==(short)length && memcmp(encoded.bytes,expected,sizeof(expected))==0, "signed16_explicit_wire_and_canaries");
    CHECK(memcmp(native.bytes,snapshot,sizeof(snapshot))==0, "signed16_source_unchanged");
    if(packet_wire(ctx,encoded.bytes+start,used,golden,length)) return 1;
    memset(encoded.bytes,0xcc,sizeof(encoded.bytes)); memcpy(encoded.bytes+start,golden,length);
    memcpy(wire_expected,encoded.bytes,sizeof(wire_expected)); memcpy(wire_expected+start,&count,2);
    memset(decoded.bytes,0xcc,sizeof(decoded.bytes)); memset(expected,0xcc,sizeof(expected));
    memcpy(expected,&count,2); memset(expected+2,0xd3,(size_t)count); expected[258]=0xee; expected[259]=0xff;
    CHECK(ctx->ops->decode(&definition,encoded.bytes+start,(short)length,decoded.bytes,&version,&consumed), "signed16_golden_decode_success");
    CHECK(version==0 && consumed==(short)length, "signed16_wire_consumption");
    CHECK(memcmp(decoded.bytes,expected,sizeof(expected))==0, "signed16_output_and_unused_reserves");
    CHECK(memcmp(encoded.bytes,wire_expected,sizeof(wire_expected))==0, "signed16_input_mutation");
    return 0;
}

static int malformed_lengths(struct packet_context *ctx, int array)
{
    struct data_packet_field data_fields[]={FIELD(_data_packet_field_data,256),FIELD(_data_packet_field_bytes,2),END_FIELD};
    struct data_packet_field array_fields[]={FIELD(_data_packet_field_array,256),FIELD(_data_packet_field_bytes,1),END_FIELD,
                                             FIELD(_data_packet_field_bytes,2),END_FIELD};
    struct data_packet_definition definition={"authored_bad_length",0,260,0,array ? array_fields : data_fields,FALSE};
    static const byte wires[][3]={{1,1,0xee},{0xff,0xff,0xee},{0},{0,2,0xd3}};
    static const short lengths[]={3,3,1,3};
    static const char *data_names[]={"data_length_too_large","data_length_negative","data_length_truncated","data_payload_truncated"};
    static const char *array_names[]={"array_length_too_large","array_length_negative","array_length_truncated","array_payload_truncated"};
    union packet_storage encoded,decoded;
    byte expected[768],wire_expected[768];
    short zero=0;
    ctx->ops->verify(&definition);
    for(unsigned int n=0;n<4;++n) {
        short version=-1,consumed=-1;
        ctx->test=array ? array_names[n] : data_names[n];
        memset(encoded.bytes,0xcc,sizeof(encoded.bytes)); memcpy(encoded.bytes+ctx->offset,wires[n],(size_t)lengths[n]);
        memcpy(wire_expected,encoded.bytes,sizeof(wire_expected));
        if(n!=2) { short bits=n==0 ? 257 : n==1 ? -1 : 2; memcpy(wire_expected+ctx->offset,&bits,2); }
        memset(decoded.bytes,0xcc,sizeof(decoded.bytes)); memset(expected,0xcc,sizeof(expected)); memcpy(expected,&zero,2);
        if(n==3) { short partial_count=2; memcpy(expected,&partial_count,2); if(array) expected[2]=0xd3; }
        CHECK(!ctx->ops->decode(&definition,encoded.bytes+ctx->offset,lengths[n],decoded.bytes,&version,&consumed), "invalid_length_decode_false");
        CHECK(version==0 && consumed==(n==2 ? 0 : n==3 && array ? 3 : 2), "invalid_length_partial_consumption");
        CHECK(memcmp(decoded.bytes,expected,sizeof(expected))==0, "invalid_length_defined_prefix_and_partial_output");
        CHECK(memcmp(encoded.bytes,wire_expected,sizeof(wire_expected))==0, "invalid_length_partial_input_mutation");
    }
    return 0;
}

static int sticky_and_version(struct packet_context *ctx)
{
    struct data_packet_field sticky_fields[]={FIELD(_data_packet_field_shorts,1),FIELD(_data_packet_field_string,4),
                                              FIELD(_data_packet_field_bytes,1),END_FIELD};
    struct data_packet_definition sticky={"authored_sticky",0,8,0,sticky_fields,FALSE};
    struct data_packet_field version_fields[]={FIELD(_data_packet_field_bytes,1),END_FIELD};
    struct data_packet_definition versioned={"authored_version",0,1,1,version_fields,FALSE};
    union packet_decode_storage backing;
    byte *encoded = backing.bytes, *decoded = backing.bytes + PACKET_REGION_BYTES;
    byte expected[768],snapshot[768];
    struct data_encoding_state state;
    short version=-1,consumed=-1;
    ctx->test="sticky_overflow_string_side_effect";
    ctx->ops->verify(&sticky);
    memset(encoded,0xcc,PACKET_REGION_BYTES); encoded[ctx->offset]=0;
    memcpy(snapshot,encoded,sizeof(snapshot));
    memset(decoded,0xcc,PACKET_REGION_BYTES); memset(expected,0xcc,sizeof(expected)); expected[2]=0;
    data_decode_new(&state,encoded+ctx->offset,1); state.overflow=TRUE;
    ctx->ops->dispatch(&sticky,&state,0,decoded);
    CHECK(state.overflow && state.offset==1, "sticky_scalar_overflow_retained_string_advances");
    CHECK(memcmp(decoded,expected,sizeof(expected))==0, "sticky_string_copies_while_other_fields_untouched");
    CHECK(memcmp(encoded,snapshot,sizeof(snapshot))==0, "sticky_input_unchanged");

    ctx->test="version_too_new";
    memset(encoded,0xcc,PACKET_REGION_BYTES); encoded[ctx->offset]=2; encoded[ctx->offset+1]=0x99;
    memcpy(snapshot,encoded,sizeof(snapshot)); memset(decoded,0xcc,PACKET_REGION_BYTES); memset(expected,0xcc,sizeof(expected));
    CHECK(!ctx->ops->decode(&versioned,encoded+ctx->offset,2,decoded,&version,&consumed), "too_new_decode_false");
    CHECK(version==2 && consumed==1, "too_new_version_only_consumed");
    CHECK(memcmp(decoded,expected,sizeof(expected))==0 && memcmp(encoded,snapshot,sizeof(snapshot))==0,
          "too_new_output_and_input_unchanged");
    return 0;
}

static int cross_version(struct packet_context *ctx)
{
    struct data_packet_field fields[]={
        {_data_packet_field_bytes,1,2,0,0}, {_data_packet_field_shorts,1,2,0,0},
        FIELD(_data_packet_field_bytes,1),END_FIELD
    };
    struct data_packet_definition definition={"authored_cross_version",0,4,2,fields,FALSE};
    static const byte golden[]={1,0,0,0xe7};
    union packet_storage native,encoded,decoded;
    byte expected[768],snapshot[768];
    short used=-1,version=-1,consumed=-1,value=0x1234;
    ctx->test="cross_version_excluded_byte_short_tail";
    memset(native.bytes,0xa5,sizeof(native.bytes)); native.bytes[0]=0x5a;
    memcpy(native.bytes+1,&value,2); native.bytes[3]=0xe7;
    memcpy(snapshot,native.bytes,sizeof(snapshot));
    ctx->ops->verify(&definition);
    CHECK(definition.initialized && fields[0].size==1 && fields[1].size==2 && fields[2].size==1,
          "cross_version_definition_fields_all_eligible");
    memset(encoded.bytes,0xcc,sizeof(encoded.bytes));
    CHECK(ctx->ops->encode(&definition,1,native.bytes,encoded.bytes+ctx->offset,&used,16), "older_encode_success");
    memset(expected,0xcc,sizeof(expected)); memcpy(expected+ctx->offset,golden,sizeof(golden));
    CHECK(used==4 && memcmp(encoded.bytes,expected,sizeof(expected))==0, "older_explicit_padding_wire");
    CHECK(memcmp(native.bytes,snapshot,sizeof(snapshot))==0, "older_encode_source_unchanged");
    if(packet_wire(ctx,encoded.bytes+ctx->offset,used,golden,sizeof(golden))) return 1;
    memset(decoded.bytes,0xcc,sizeof(decoded.bytes));
    CHECK(ctx->ops->decode(&definition,encoded.bytes+ctx->offset,4,decoded.bytes,&version,&consumed), "older_decode_reports_success");
    if(fprintf(ctx->report,"PACKET %s OUTCOME case=%s offset=%u consumed=%d expected=4 tail=%02x expected=e7\n",
               ctx->ops->name,ctx->test,ctx->offset,(int)consumed,decoded.bytes[3])<0 || fflush(ctx->report)!=0) return 1;
    /* Desired interoperability assertions deliberately expose the preserved defect. */
    CHECK(version==1 && consumed==4, "cross_version_desired_full_wire_consumption");
    memset(expected,0xcc,sizeof(expected)); memset(expected,0,3); expected[3]=0xe7;
    CHECK(memcmp(decoded.bytes,expected,sizeof(expected))==0, "cross_version_desired_native_tail_and_canaries");
    return 0;
}

static int packet_section(FILE *report,int collect,const struct packet_ops *ops)
{
    struct packet_context context={report,ops,"",0,0,0,collect};
    struct packet_context *ctx=&context;
    int aborted=0;
    if(fprintf(report,"PACKET %s BEGIN\nPACKET LAYOUT current_native=40 data_prefix=24 array_prefix=30 reserve_pad=38:2 signed16_native=260 maximum_count=256 wire_offsets=0..7\n",
               ops->name)<0 || fflush(report)!=0) return 1;
    for(ctx->offset=0;ctx->offset<8;++ctx->offset) {
        for(unsigned int variant=0;variant<5;++variant)
            if(current_packet(ctx,variant)) { aborted=1; goto done; }
        for(int array=0;array<2;++array) {
            const short counts[]={0,1,256};
            for(unsigned int n=0;n<3;++n)
                if(length_packet(ctx,array,counts[n])) { aborted=1; goto done; }
            if(malformed_lengths(ctx,array)) { aborted=1; goto done; }
        }
        if(sticky_and_version(ctx) || cross_version(ctx)) { aborted=1; goto done; }
    }
done:
    if(fprintf(report,"PACKET %s SUMMARY checks=%u failures=%u offsets=8 aborted=%d\nPACKET %s END result=%d\n",
               ops->name,ctx->checks,ctx->failures,aborted,ops->name,aborted || ctx->failures!=0)<0 || fflush(report)!=0) return 1;
    return aborted || ctx->failures!=0;
}

int wii_packet_compare(FILE *report,int collect_failures)
{
    const struct packet_ops reference={"REFERENCE",data_packet_verify,data_packet_encode,data_packet_decode,packet_reference_dispatch_decode};
    const struct packet_ops candidate={"CANDIDATE",candidate_packet_verify,candidate_packet_encode,candidate_packet_decode,packet_candidate_dispatch_decode};
    int original=packet_section(report,collect_failures,&reference);
    int adapted=packet_section(report,collect_failures,&candidate);
    return original || adapted;
}
#undef CHECK
#undef FIELD
#undef END_FIELD
