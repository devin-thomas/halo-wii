#include "cache_address_fixture.h"
#include "cache_address_probe.h"
#include <stdlib.h>
#include <string.h>

enum { FIXTURE_BLOB=1024,FIXTURE_STORAGE=1056,FIXTURE_GUARD=16 };
#define SYNTHETIC_BASE UINT32_C(0x803a6000)
#define SCENARIO_GROUP UINT32_C(0x73636e72)
#define BSP_GROUP UINT32_C(0x73627370)
#define UNKNOWN_GROUP UINT32_C(0x71727374)
struct fixture_context {FILE *report;const char *name;unsigned cases,checks,failures;int collect;};
struct fixture_storage {
    unsigned char bytes[FIXTURE_STORAGE],before[FIXTURE_STORAGE],changed[FIXTURE_STORAGE];
    unsigned char *temporary_source;
    struct cache_address_graph graph;
};
static int check(struct fixture_context *ctx,int condition,const char *assertion)
{
    ++ctx->checks;if(condition)return 1;++ctx->failures;
    if(fprintf(ctx->report,"CACHE_ADDRESS FAIL case=%s assertion=%s check=%u\n",ctx->name,assertion,ctx->checks)<0 || fflush(ctx->report)!=0)return 0;
    return ctx->collect;
}
#define CHECK(c,n) do {if(!check(ctx,(c),(n)))return 1;} while(0)
static int abort_fixture(struct fixture_context *ctx,const char *reason)
{
    (void)fprintf(ctx->report,"CACHE_ADDRESS ABORT case=%s reason=%s\n",ctx->name,reason);(void)fflush(ctx->report);return 1;
}
static void put32(unsigned char *p,uint32_t value)
{
    p[0]=(unsigned char)value;p[1]=(unsigned char)(value>>8);p[2]=(unsigned char)(value>>16);p[3]=(unsigned char)(value>>24);
}
static void instance(unsigned char *p,uint32_t group,uint32_t datum,uint32_t name,uint32_t root)
{
    put32(p,group);put32(p+4,UINT32_C(0x6f626a65));put32(p+8,UINT32_C(0x756e6974));put32(p+12,datum);
    put32(p+16,name);put32(p+20,root);put32(p+24,UINT32_C(0x89abcdef));put32(p+28,UINT32_C(0xfedcba98));
}
static struct cache_address_region synthetic(struct fixture_storage *s,unsigned offset)
{
    memset(s->bytes,0x5a,sizeof(s->bytes));unsigned char *p=s->bytes+FIXTURE_GUARD+offset;
    put32(p,SYNTHETIC_BASE+64);put32(p+4,UINT32_C(0x12340000));put32(p+8,UINT32_C(0x13579bdf));put32(p+12,3);
    put32(p+16,1);put32(p+20,SYNTHETIC_BASE+192);put32(p+24,2);put32(p+28,SYNTHETIC_BASE+204);put32(p+32,UINT32_C(0x74616773));
    instance(p+64,SCENARIO_GROUP,UINT32_C(0x12340000),SYNTHETIC_BASE+256,SYNTHETIC_BASE+400);
    instance(p+96,BSP_GROUP,UINT32_C(0xabcd0001),SYNTHETIC_BASE+272,0);
    instance(p+128,UNKNOWN_GROUP,UINT32_C(0x98760002),SYNTHETIC_BASE+288,SYNTHETIC_BASE+500);
    memcpy(p+256,"synth_root",11);memcpy(p+272,"synth_bsp",10);memcpy(p+288,"synth_other",12);
    /* D3DResource Data values are resource offsets, not tag addresses. */
    put32(p+192,UINT32_C(0x11223344));put32(p+196,UINT32_MAX);put32(p+200,UINT32_C(0xdeadbeef));
    memcpy(s->before,s->bytes,sizeof(s->bytes));return (struct cache_address_region){p,FIXTURE_BLOB,SYNTHETIC_BASE};
}
static int decoder_vectors(struct fixture_context *ctx,struct fixture_storage *s)
{
    ctx->name="explicit_le_numeric_header_instance";
    for(unsigned offset=0;offset<8;++offset){struct cache_address_region region=synthetic(s,offset);++ctx->cases;
        struct cache_address_header header;memset(&header,0xa5,sizeof(header));struct cache_address_result r;
        int accepted=cache_address_decode_header(&region,&header,&r);
        CHECK(accepted && r.error==CACHE_ADDRESS_OK,"header_explicit_le_at_all_alignments");
        CHECK(header.instances_address==SYNTHETIC_BASE+64 && header.scenario_datum==UINT32_C(0x12340000) && header.checksum==UINT32_C(0x13579bdf) && header.tag_count==3,"header_numeric_addresses_datums_checksum_count");
        CHECK(header.vertex_count==1 && header.vertex_address==SYNTHETIC_BASE+192 && header.index_count==2 && header.index_address==SYNTHETIC_BASE+204 && header.signature==UINT32_C(0x74616773),"header_buffer_counts_encoded_addresses_signature");
        struct cache_address_instance value;accepted=cache_address_decode_instance(region.bytes,region.size,128,&value,&r);
        CHECK(accepted && value.group==UNKNOWN_GROUP && value.parent[0]==UINT32_C(0x6f626a65) && value.parent[1]==UINT32_C(0x756e6974) && value.datum==UINT32_C(0x98760002),"instance_unknown_classes_parents_exact_datum_preserved");
        CHECK(value.name_address==SYNTHETIC_BASE+288 && value.root_address==SYNTHETIC_BASE+500 && value.unused[0]==UINT32_C(0x89abcdef) && value.unused[1]==UINT32_C(0xfedcba98),"instance_encoded_addresses_unused_preserved");
        CHECK(memcmp(s->bytes,s->before,sizeof(s->bytes))==0,"decode_input_and_guards_unchanged");
        for(size_t length=0;length<36;++length){++ctx->cases;struct cache_address_header before;memset(&header,0xa5,sizeof(header));memcpy(&before,&header,sizeof(before));struct cache_address_region short_region=region;short_region.size=length;
            accepted=cache_address_decode_header(&short_region,&header,&r);
            CHECK(!accepted && r.error==CACHE_ADDRESS_SPAN && memcmp(&header,&before,sizeof(header))==0,"every_short_header_atomic_output");
        }
        for(size_t length=0;length<32;++length){++ctx->cases;struct cache_address_instance before;memset(&value,0xa5,sizeof(value));memcpy(&before,&value,sizeof(before));
            accepted=cache_address_decode_instance(region.bytes+128,length,0,&value,&r);
            CHECK(!accepted && r.error==CACHE_ADDRESS_SPAN && memcmp(&value,&before,sizeof(value))==0,"every_short_instance_atomic_output");
        }
    }
    return 0;
}
static int resolver_vectors(struct fixture_context *ctx,struct fixture_storage *s)
{
    ctx->name="encoded_address_count_stride_bounds";struct cache_address_region region=synthetic(s,0);
    struct {uint32_t address;int64_t count;size_t stride;enum cache_address_error error;size_t offset,length;} cases[]={
        {SYNTHETIC_BASE,1,1,CACHE_ADDRESS_OK,0,1},{SYNTHETIC_BASE+1023,1,1,CACHE_ADDRESS_OK,1023,1},
        {SYNTHETIC_BASE+64,3,32,CACHE_ADDRESS_OK,64,96},{SYNTHETIC_BASE+1024,0,32,CACHE_ADDRESS_OK,0,0},
        {0,0,0,CACHE_ADDRESS_OK,0,0},{UINT32_MAX,0,SIZE_MAX,CACHE_ADDRESS_OK,0,0},
        {SYNTHETIC_BASE-1,1,1,CACHE_ADDRESS_SPAN,0,0},{SYNTHETIC_BASE+1024,1,1,CACHE_ADDRESS_SPAN,0,0},
        {SYNTHETIC_BASE+1023,2,1,CACHE_ADDRESS_SPAN,0,0},{SYNTHETIC_BASE,-1,1,CACHE_ADDRESS_COUNT,0,0},
        {SYNTHETIC_BASE,1,0,CACHE_ADDRESS_ARGUMENT,0,0},{SYNTHETIC_BASE,INT64_MAX,3,CACHE_ADDRESS_OVERFLOW,0,0},
    };
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i){++ctx->cases;struct cache_address_span value,before;memset(&value,0xa5,sizeof(value));memcpy(&before,&value,sizeof(before));struct cache_address_result r;
        int accepted=cache_address_resolve(&region,cases[i].address,cases[i].count,cases[i].stride,&value,&r);
        CHECK(accepted==(cases[i].error==CACHE_ADDRESS_OK) && r.error==cases[i].error,"resolver_exact_acceptance_reason");
        if(accepted)CHECK(value.offset==cases[i].offset && value.length==cases[i].length,"resolver_independent_offset_length");
        else CHECK(memcmp(&value,&before,sizeof(value))==0,"resolver_rejection_atomic_output");
    }
    ++ctx->cases;struct cache_address_span span={123,456},before=span;struct cache_address_result r;struct cache_address_region wrap=region;wrap.encoded_base=UINT32_MAX-511;
    CHECK(!cache_address_resolve(&wrap,wrap.encoded_base,1,1,&span,&r) && r.error==CACHE_ADDRESS_OVERFLOW && memcmp(&span,&before,sizeof(span))==0,"encoded_region_wrap_rejected_before_access");
    CHECK(memcmp(s->bytes,s->before,sizeof(s->bytes))==0,"resolver_does_not_mutate_buffer");return 0;
}
static int lookup_failure(struct fixture_context *ctx,struct fixture_storage *s,uint32_t datum,uint32_t group,enum cache_address_error error)
{
    ++ctx->cases;struct cache_address_reference reference,before;memset(&reference,0xa5,sizeof(reference));memcpy(&before,&reference,sizeof(before));struct cache_address_result r;
    int accepted=cache_address_graph_lookup(&s->graph,datum,group,&reference,&r);
    CHECK(!accepted && r.error==error && memcmp(&reference,&before,sizeof(reference))==0,"lookup_failure_atomic_reference");return 0;
}
static int lifecycle(struct fixture_context *ctx,struct fixture_storage *s)
{
    ctx->name="load_use_lossless_unload_repeat";
    for(unsigned offset=0;offset<8;++offset)for(unsigned cycle=0;cycle<64;++cycle){++ctx->cases;struct cache_address_region region=synthetic(s,offset);struct cache_address_result r;
        int accepted=cache_address_graph_load(&region,&s->graph,&r);CHECK(accepted && r.error==CACHE_ADDRESS_OK,"owned_graph_load");if(!accepted)return abort_fixture(ctx,"synthetic_graph_load_failed");
        CHECK(s->graph.count==3 && s->graph.header.scenario_datum==UINT32_C(0x12340000) && s->graph.bytes!=region.bytes,"owned_graph_decoded_table_and_separate_storage");
        size_t length=0;const unsigned char *owned=cache_address_graph_bytes(&s->graph,&length);
        CHECK(owned && length==FIXTURE_BLOB && memcmp(owned,region.bytes,length)==0,"lossless_blob_roundtrip_exact");if(!owned || length!=FIXTURE_BLOB)return abort_fixture(ctx,"owned_blob_missing");
        memset(s->bytes+FIXTURE_GUARD+offset,0xee,FIXTURE_BLOB);
        CHECK(memcmp(owned,s->before+FIXTURE_GUARD+offset,FIXTURE_BLOB)==0,"owned_snapshot_independent_of_mutated_original_source");
        memcpy(s->bytes,s->before,sizeof(s->bytes));
        struct cache_address_reference reference;accepted=cache_address_graph_lookup(&s->graph,UINT32_C(0x12340000),SCENARIO_GROUP,&reference,&r);
        CHECK(accepted && reference.name.offset==256 && reference.name.length==11 && reference.root.offset==400 && reference.root.length==1,"scenario_numeric_lookup_bounded_name_root");
        accepted=cache_address_graph_lookup(&s->graph,UINT32_C(0xabcd0001),BSP_GROUP,&reference,&r);
        CHECK(accepted && reference.root.length==0 && reference.instance.root_address==0,"unloaded_bsp_null_root_explicit");
        accepted=cache_address_graph_lookup(&s->graph,UINT32_C(0x98760002),UNKNOWN_GROUP,&reference,&r);
        CHECK(accepted && reference.name.offset==288 && reference.root.offset==500 && reference.instance.unused[0]==UINT32_C(0x89abcdef),"unknown_group_numeric_preservation_lookup");
        accepted=cache_address_graph_lookup(&s->graph,UINT32_C(0x98760002),UINT32_C(0x6f626a65),&reference,&r);
        CHECK(accepted,"parent_group_lookup_numeric_match");
        struct cache_address_graph graph_before;memcpy(&graph_before,&s->graph,sizeof(graph_before));
        accepted=cache_address_graph_load(&region,&s->graph,&r);
        CHECK(!accepted && r.error==CACHE_ADDRESS_STATE && memcmp(&graph_before,&s->graph,sizeof(graph_before))==0,"reload_owned_control_rejected_atomically");
        CHECK(memcmp(s->bytes,s->before,sizeof(s->bytes))==0,"lifecycle_source_guards_unchanged");
        cache_address_graph_unload(&s->graph);struct cache_address_graph zero;memset(&zero,0,sizeof(zero));
        CHECK(memcmp(&s->graph,&zero,sizeof(zero))==0,"unload_control_all_zero");
        CHECK(cache_address_graph_bytes(&s->graph,&length)==NULL && length==0,"unloaded_blob_not_exposed");
        if(lookup_failure(ctx,s,UINT32_C(0x12340000),SCENARIO_GROUP,CACHE_ADDRESS_STATE))return 1;
        cache_address_graph_unload(&s->graph);CHECK(memcmp(&s->graph,&zero,sizeof(zero))==0,"repeated_unload_idempotent");
    }
    struct cache_address_region region=synthetic(s,0);struct cache_address_result r;int accepted=cache_address_graph_load(&region,&s->graph,&r);
    CHECK(accepted,"lookup_rejection_graph_load");if(!accepted)return abort_fixture(ctx,"lookup_graph_load_failed");
    if(lookup_failure(ctx,s,UINT32_C(0x12350000),SCENARIO_GROUP,CACHE_ADDRESS_DATUM) ||
       lookup_failure(ctx,s,UINT32_MAX,SCENARIO_GROUP,CACHE_ADDRESS_DATUM) ||
       lookup_failure(ctx,s,UINT32_C(0x12340000),UNKNOWN_GROUP,CACHE_ADDRESS_GROUP) ||
       lookup_failure(ctx,s,UINT32_C(0x98760002),UINT32_MAX,CACHE_ADDRESS_GROUP))return 1;
    cache_address_graph_unload(&s->graph);return 0;
}
static int released_source(struct fixture_context *ctx,struct fixture_storage *s)
{
    ctx->name="owned_graph_source_freed";++ctx->cases;struct cache_address_region region=synthetic(s,0);
    s->temporary_source=malloc(FIXTURE_BLOB);CHECK(s->temporary_source!=NULL,"source_heap_allocation");
    if(!s->temporary_source)return abort_fixture(ctx,"source_heap_allocation_failed");
    memcpy(s->temporary_source,region.bytes,FIXTURE_BLOB);region.bytes=s->temporary_source;
    struct cache_address_result r;int accepted=cache_address_graph_load(&region,&s->graph,&r);
    CHECK(accepted,"source_heap_graph_load");if(!accepted)return abort_fixture(ctx,"source_heap_graph_load_failed");
    memset(s->temporary_source,0xee,FIXTURE_BLOB);free(s->temporary_source);s->temporary_source=NULL;
    struct cache_address_reference reference;accepted=cache_address_graph_lookup(&s->graph,UINT32_C(0x12340000),SCENARIO_GROUP,&reference,&r);
    CHECK(accepted && reference.name.offset==256 && reference.root.offset==400,"owned_graph_use_after_original_source_free");
    size_t size=0;const unsigned char *bytes=cache_address_graph_bytes(&s->graph,&size);
    CHECK(bytes && size==FIXTURE_BLOB && memcmp(bytes,s->before+FIXTURE_GUARD,FIXTURE_BLOB)==0,"owned_graph_lossless_after_source_free");
    cache_address_graph_unload(&s->graph);return 0;
}
static int malformed(struct fixture_context *ctx,struct fixture_storage *s)
{
    ctx->name="malformed_atomic_graph_load";
    const struct {size_t offset;uint32_t value;enum cache_address_error error;} edits[]={
        {32,UINT32_C(0x73676174),CACHE_ADDRESS_SIGNATURE},{12,0,CACHE_ADDRESS_COUNT},{12,UINT32_MAX,CACHE_ADDRESS_COUNT},
        {12,65536,CACHE_ADDRESS_COUNT},{16,UINT32_MAX,CACHE_ADDRESS_COUNT},{24,UINT32_MAX,CACHE_ADDRESS_COUNT},
        {0,SYNTHETIC_BASE-1,CACHE_ADDRESS_SPAN},{0,SYNTHETIC_BASE+1024,CACHE_ADDRESS_SPAN},
        {20,SYNTHETIC_BASE+1020,CACHE_ADDRESS_SPAN},{28,SYNTHETIC_BASE+1012,CACHE_ADDRESS_SPAN},
        {4,UINT32_C(0x12350000),CACHE_ADDRESS_DATUM},{4,UINT32_C(0x12340003),CACHE_ADDRESS_DATUM},
        {64,UNKNOWN_GROUP,CACHE_ADDRESS_DATUM},{108,UINT32_C(0xabcd0002),CACHE_ADDRESS_DATUM},
        {80,0,CACHE_ADDRESS_NAME},{80,SYNTHETIC_BASE+1024,CACHE_ADDRESS_NAME},
        {80,SYNTHETIC_BASE+1023,CACHE_ADDRESS_NAME},{84,0,CACHE_ADDRESS_ROOT},
        {84,SYNTHETIC_BASE-1,CACHE_ADDRESS_ROOT},{84,SYNTHETIC_BASE+1024,CACHE_ADDRESS_ROOT},
    };
    for(unsigned alignment=0;alignment<8;++alignment)for(size_t i=0;i<sizeof(edits)/sizeof(edits[0]);++i){++ctx->cases;struct cache_address_region region=synthetic(s,alignment);
        memcpy(s->changed,s->bytes,sizeof(s->bytes));unsigned char *blob=s->changed+FIXTURE_GUARD+alignment;put32(blob+edits[i].offset,edits[i].value);region.bytes=blob;
        struct cache_address_graph before;memcpy(&before,&s->graph,sizeof(before));struct cache_address_result r;int accepted=cache_address_graph_load(&region,&s->graph,&r);
        CHECK(!accepted && r.error==edits[i].error,"malformed_exact_reason");
        CHECK(memcmp(&before,&s->graph,sizeof(before))==0,"malformed_graph_output_atomic");
        if(accepted){cache_address_graph_unload(&s->graph);return abort_fixture(ctx,"malformed_graph_unexpectedly_loaded");}
        CHECK(memcmp(s->bytes,s->before,sizeof(s->bytes))==0,"malformed_input_baseline_guards_preserved");
    }
    for(size_t length=0;length<=160;++length){++ctx->cases;struct cache_address_region region=synthetic(s,0);region.size=length;
        struct cache_address_graph before;memcpy(&before,&s->graph,sizeof(before));struct cache_address_result r;int accepted=cache_address_graph_load(&region,&s->graph,&r);
        CHECK(!accepted && memcmp(&before,&s->graph,sizeof(before))==0,"short_blob_complete_graph_rejected_atomically");
        if(accepted){cache_address_graph_unload(&s->graph);return abort_fixture(ctx,"short_graph_unexpectedly_loaded");}
    }
    ++ctx->cases;struct cache_address_region region=synthetic(s,0);unsigned char *blob=s->bytes+FIXTURE_GUARD;put32(blob+16,0);put32(blob+20,UINT32_MAX);put32(blob+24,0);put32(blob+28,0);
    struct cache_address_result r;int accepted=cache_address_graph_load(&region,&s->graph,&r);
    CHECK(accepted,"zero_buffer_counts_ignore_unfollowed_numeric_addresses");if(!accepted)return abort_fixture(ctx,"zero_buffers_graph_failed");cache_address_graph_unload(&s->graph);
    return 0;
}
int wii_cache_address_fixture(FILE *report,int collect)
{
    if(!report)return 1;
    struct fixture_context context={report,"initialization",0,0,0,collect};struct fixture_context *ctx=&context;
    struct fixture_storage *s=calloc(1,sizeof(*s));int aborted=0;
    if(!s){aborted=abort_fixture(ctx,"fixture_storage_allocation_failed");goto done;}
    if(decoder_vectors(ctx,s) || resolver_vectors(ctx,s) || lifecycle(ctx,s) || released_source(ctx,s) || malformed(ctx,s))aborted=1;
done:
    if(s){cache_address_graph_unload(&s->graph);free(s->temporary_source);free(s);}
    if(fprintf(report,"CACHE_ADDRESS SUMMARY cases=%u checks=%u failures=%u aborted=%d scope=synthetic_index_name_root_address_only source_mutation=none nested_tag_BSP_body_conversion=not_performed\n",ctx->cases,ctx->checks,ctx->failures,aborted)<0 || fflush(report)!=0)return 1;
    return aborted || ctx->failures?1:0;
}
