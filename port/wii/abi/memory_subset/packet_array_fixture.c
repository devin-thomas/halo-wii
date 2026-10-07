#include "packet_array_policy.h"
#include "packet_array_fixture.h"
#include <limits.h>

enum { OBJECT_SIZE=4352, GUARD=16, EVENT_LIMIT=1024, FIELD_LIMIT=32 };
#define F(t,c,lo,hi) {t,c,lo,hi,113}
#define E {_data_packet_field_end,-7,-8,-9,117}
struct array_context { FILE *report; const char *name; unsigned checks, failures, cases; int collect; void *owned[8]; };
struct event { unsigned wire, span, native, native_span; int mutate; };
struct vector {
    struct data_packet_field fields[FIELD_LIMIT]; size_t field_count;
    short size, own_version, version;
    unsigned char input[OBJECT_SIZE], output[OBJECT_SIZE], encode[OBJECT_SIZE], decode[OBJECT_SIZE];
    struct event events[EVENT_LIMIT]; unsigned event_count, wire_size;
};
struct objects { unsigned char native[OBJECT_SIZE], wire[OBJECT_SIZE], expected_native[OBJECT_SIZE], expected_wire[OBJECT_SIZE]; };
static int array_check(struct array_context *ctx,int condition,const char *assertion)
{
    ++ctx->checks;
    if(condition) return 1;
    ++ctx->failures;
    if(fprintf(ctx->report,"ARRAY FAIL case=%s assertion=%s check=%u\n",ctx->name,assertion,ctx->checks)<0 || fflush(ctx->report)!=0) return 0;
    return ctx->collect;
}
#define CHECK(c,n) do { if(!array_check(ctx,(c),(n))) return 1; } while(0)
static void remember_resource(struct array_context *ctx,void *resource)
{
    for(unsigned n=0;n<8;++n)if(!ctx->owned[n]){ctx->owned[n]=resource;return;}
    match_assert(__FILE__,__LINE__,FALSE);
}
static void forget_resource(struct array_context *ctx,void *resource)
{
    for(unsigned n=0;n<8;++n)if(ctx->owned[n]==resource){ctx->owned[n]=NULL;return;}
    match_assert(__FILE__,__LINE__,FALSE);
}
static void *fixture_allocate(struct array_context *ctx,size_t bytes,const char *name)
{
    void *resource=malloc(bytes);
    if(resource) remember_resource(ctx,resource);
    else if(fprintf(ctx->report,"ARRAY ABORT allocation=%s case=%s\n",name,ctx->name)<0 || fflush(ctx->report)!=0) return NULL;
    return resource;
}
static void fixture_release(struct array_context *ctx,void *resource)
{
    if(resource){forget_resource(ctx,resource);free(resource);}
}
static boolean fixture_compile(struct array_context *ctx,const struct data_packet_definition *d,size_t bound,struct packet_array_plan *plan,struct packet_array_result *result)
{
    boolean accepted=packet_array_compile(d,bound,plan,result);
    if(accepted) remember_resource(ctx,plan->nodes);
    return accepted;
}
static void fixture_destroy(struct array_context *ctx,struct packet_array_plan *plan)
{
    if(plan->nodes)forget_resource(ctx,plan->nodes);
    packet_array_destroy(plan);
}
static void definition(struct data_packet_definition *d,struct data_packet_field *fields,short size,short version)
{
    memset(d,0xa5,sizeof(*d)); d->name="authored_array_schema"; d->flags=0x1234;
    d->fields=fields; d->size=size; d->version=version; d->initialized=TRUE;
}
static void vector_init(struct vector *v,const struct data_packet_field *fields,size_t count,short size,short own_version,short version)
{
    memset(v,0,sizeof(*v)); memcpy(v->fields,fields,count*sizeof(*fields));
    v->field_count=count; v->size=size; v->own_version=own_version; v->version=version;
    memset(v->input,0x4a,sizeof(v->input)); memset(v->output,0x5a,sizeof(v->output));
}
static void add_event(struct vector *v,unsigned native,unsigned native_span,const unsigned char *encoded,unsigned span,int mutate)
{
    match_assert(__FILE__,__LINE__,v->event_count<EVENT_LIMIT && v->wire_size+span<OBJECT_SIZE);
    struct event *e=&v->events[v->event_count++];
    e->wire=v->wire_size; e->span=span; e->native=native; e->native_span=native_span; e->mutate=mutate;
    if(span) {memcpy(v->encode+v->wire_size,encoded,span); memcpy(v->decode+v->wire_size,encoded,span);}
    v->wire_size+=span;
}
static void version_event(struct vector *v)
{
    if(v->own_version>0) {unsigned char b=(unsigned char)v->version; add_event(v,0,0,&b,1,0);}
}
static void count_event(struct vector *v,unsigned at,short count,short maximum)
{
    unsigned char bytes[2]={(unsigned char)((unsigned short)count>>8),(unsigned char)count};
    memcpy(v->input+at,&count,2); memcpy(v->output+at,&count,2);
    add_event(v,at,2,bytes+(maximum<=255),maximum<=255?1:2,maximum>255);
}
static void raw_event(struct vector *v,unsigned at,unsigned span,unsigned seed)
{
    for(unsigned n=0;n<span;++n) v->input[at+n]=v->output[at+n]=(unsigned char)(seed+n);
    add_event(v,at,span,v->input+at,span,0);
}
static void excluded_event(struct vector *v,unsigned at,unsigned extent,unsigned span)
{
    unsigned char zeros[256]={0}; match_assert(__FILE__,__LINE__,span<=sizeof(zeros));
    memset(v->output+at,0,extent); add_event(v,at,extent,zeros,span,0);
    /* Placeholder contents are deliberately nonzero on decode. */
    for(unsigned n=v->wire_size-span;n<v->wire_size;++n) v->decode[n]=(unsigned char)(0x91+n%17);
}
static void words_event(struct vector *v,unsigned at,unsigned count,unsigned width,unsigned seed)
{
    unsigned char bytes[32];match_assert(__FILE__,__LINE__,count*width<=sizeof(bytes));
    for(unsigned n=0;n<count;++n) {
        uint32_t value=seed+n;
        if(width==2) {uint16_t small=(uint16_t)value;memcpy(v->input+at+n*width,&small,2);memcpy(v->output+at+n*width,&small,2);}else {memcpy(v->input+at+n*width,&value,4);memcpy(v->output+at+n*width,&value,4);}
        for(unsigned b=0;b<width;++b)bytes[n*width+b]=(unsigned char)(value>>(8*(width-b-1)));
    }
    add_event(v,at,count*width,bytes,count*width,1);
}
static int run_vector(struct array_context *ctx,struct vector *v,const char *name,struct objects *o)
{
    struct data_packet_definition d,before; struct data_packet_field fields_before[FIELD_LIMIT];
    struct packet_array_plan plan={0}; struct packet_array_result result;
    ctx->name=name; ++ctx->cases; definition(&d,v->fields,v->size,v->own_version);
    memcpy(&before,&d,sizeof(d)); memcpy(fields_before,v->fields,sizeof(fields_before));
    boolean compiled=fixture_compile(ctx,&d,v->field_count,&plan,&result);
    CHECK(compiled && result.error==PACKET_ARRAY_OK,"compile_valid_stable_layout");
    if(!compiled) return 1;
    CHECK(plan.extent==(unsigned)v->size,"full_native_reserve");
    CHECK(memcmp(&before,&d,sizeof(d))==0 && memcmp(fields_before,v->fields,sizeof(fields_before))==0,"source_metadata_unchanged");
    struct packet_array_plan plan_before;memcpy(&plan_before,&plan,sizeof(plan));
    size_t node_bytes=plan.count*sizeof(*plan.nodes);struct packet_array_node *nodes_before=fixture_allocate(ctx,node_bytes,"plan_snapshot");
    if(!nodes_before) {fixture_destroy(ctx,&plan);return 1;}
    memcpy(nodes_before,plan.nodes,node_bytes);
    if(fprintf(ctx->report,"ARRAY CASE name=%s native=%u wire=%u schema=%zu depth=%zu offsets=0-7 every_truncation_and_capacity=1\n",name,(unsigned)plan.extent,v->wire_size,plan.count,plan.depth)<0 || fflush(ctx->report)!=0) {fixture_destroy(ctx,&plan);return 1;}
    for(unsigned offset=0;offset<8;++offset) {
        unsigned base=GUARD+offset;
        for(unsigned cap=0;cap<=v->wire_size;++cap) {
            memset(o->native,0xc3,sizeof(o->native)); memcpy(o->native+base,v->input,(unsigned)v->size);
            memcpy(o->expected_native,o->native,sizeof(o->native));
            memset(o->wire,0xcc,sizeof(o->wire)); memcpy(o->expected_wire,o->wire,sizeof(o->wire));
            unsigned used=0; int complete=1;
            for(unsigned n=0;n<v->event_count;++n) {const struct event *e=&v->events[n]; if(e->span>cap-used) {complete=0;break;} memcpy(o->expected_wire+base+used,v->encode+e->wire,e->span);used+=e->span;}
            boolean accepted=packet_array_encode(&plan,v->version,o->native+base,v->size,o->wire+base,cap,&result);
            CHECK(accepted==complete,"encode_capacity_acceptance");
            CHECK(result.wire_used==(long)used && result.native_required==plan.extent,"encode_capacity_cursor_and_extent");
            CHECK(memcmp(o->wire,o->expected_wire,sizeof(o->wire))==0,"encode_full_wire_canaries_and_prefix");
            CHECK(memcmp(o->native,o->expected_native,sizeof(o->native))==0,"encode_full_native_input_preservation");
        }
        for(unsigned length=0;length<=v->wire_size+1;++length) {
            memset(o->native,0xc3,sizeof(o->native)); memset(o->native+base,0x5a,(unsigned)v->size); memcpy(o->expected_native,o->native,sizeof(o->native));
            memset(o->wire,0xcc,sizeof(o->wire)); memcpy(o->wire+base,v->decode,v->wire_size); o->wire[base+v->wire_size]=0xe7; memcpy(o->expected_wire,o->wire,sizeof(o->wire));
            unsigned used=0; int complete=1;
            for(unsigned n=0;n<v->event_count;++n) {const struct event *e=&v->events[n]; if(e->span>length-used) {complete=0;break;}
                if(e->native_span) memcpy(o->expected_native+base+e->native,v->output+e->native,e->native_span);
                if(e->mutate) memcpy(o->expected_wire+base+used,v->output+e->native,e->span);
                used+=e->span;
            }
            short version=-111; boolean accepted=packet_array_decode(&plan,o->wire+base,length,o->native+base,v->size,&version,&result);
            CHECK(accepted==complete,"decode_truncation_or_trailing_acceptance");
            CHECK(result.wire_used==(long)used && result.native_required==plan.extent,"decode_truncation_cursor_and_extent");
            CHECK(memcmp(o->native,o->expected_native,sizeof(o->native))==0,"decode_full_native_canaries_partial_count_and_payload");
            CHECK(memcmp(o->wire,o->expected_wire,sizeof(o->wire))==0,"decode_full_wire_canaries_scalar_mutation_and_trailing");
            if(complete) CHECK(version==v->version && result.error==PACKET_ARRAY_OK,"decode_version_and_success_reason");
        }
        memset(o->native,0xc3,sizeof(o->native)); memcpy(o->expected_native,o->native,sizeof(o->native));
        memset(o->wire,0xcc,sizeof(o->wire)); memcpy(o->wire+base,v->decode,v->wire_size); memcpy(o->expected_wire,o->wire,sizeof(o->wire));
        short version=-111;
        CHECK(!packet_array_decode(&plan,o->wire+base,v->wire_size,o->native+base,v->size-1,&version,&result) && result.error==PACKET_ARRAY_CAPACITY && result.wire_used==0,"decode_capacity_checked_before_any_io");
        CHECK(memcmp(o->native,o->expected_native,sizeof(o->native))==0 && memcmp(o->wire,o->expected_wire,sizeof(o->wire))==0,"decode_capacity_whole_objects_unchanged");
        CHECK(!packet_array_encode(&plan,v->version,o->native+base,v->size-1,o->wire+base,v->wire_size,&result) && result.error==PACKET_ARRAY_CAPACITY && result.wire_used==0,"encode_capacity_checked_before_any_io");
        CHECK(memcmp(o->native,o->expected_native,sizeof(o->native))==0 && memcmp(o->wire,o->expected_wire,sizeof(o->wire))==0,"encode_capacity_whole_objects_unchanged");
    }
    CHECK(memcmp(&plan,&plan_before,sizeof(plan))==0 && memcmp(plan.nodes,nodes_before,node_bytes)==0,"compiled_plan_snapshot_immutable_across_runtime_paths");
    fixture_release(ctx,nodes_before);fixture_destroy(ctx,&plan); struct packet_array_plan zero={0};
    CHECK(memcmp(&plan,&zero,sizeof(plan))==0,"destroy_releases_and_zeros_plan");
    CHECK(memcmp(&before,&d,sizeof(d))==0 && memcmp(fields_before,v->fields,sizeof(fields_before))==0,"source_metadata_unchanged_after_all_runtime_paths");
    return 0;
}

static int basic_vectors(struct array_context *ctx,struct vector *v,struct objects *o)
{
    const struct data_packet_field simple[]={F(_data_packet_field_array,3,0,0),F(_data_packet_field_bytes,1,0,0),E,F(_data_packet_field_bytes,1,0,0),E};
    for(short count=0;count<=3;++count) {
        char name[64]; snprintf(name,sizeof(name),"array_byte_count_%d",count); vector_init(v,simple,5,6,1,1); version_event(v); count_event(v,0,count,3);
        for(short n=0;n<count;++n) raw_event(v,2+(unsigned)n,1,0x21+(unsigned)n);
        raw_event(v,5,1,0xe7); if(run_vector(ctx,v,name,o)) return 1;
    }
    for(short maximum=255;maximum<=256;++maximum) {
        struct data_packet_field selector[]={F(_data_packet_field_array,maximum,0,0),F(_data_packet_field_bytes,1,0,0),E,F(_data_packet_field_bytes,1,0,0),E};
        for(unsigned choice=0;choice<3;++choice) {
            short count=choice==0?0:choice==1?1:maximum; char name[64]; snprintf(name,sizeof(name),"array_selector_%d_count_%d",maximum,count);
            vector_init(v,selector,5,(short)(maximum+3),1,1); version_event(v);count_event(v,0,count,maximum);
            for(short n=0;n<count;++n) raw_event(v,2+(unsigned)n,1,0x21+(unsigned)n);
            raw_event(v,2+(unsigned)maximum,1,0xe7); if(run_vector(ctx,v,name,o))return 1;
        }
        struct data_packet_field data[]={F(_data_packet_field_data,maximum,0,0),F(_data_packet_field_bytes,1,0,0),E};
        for(unsigned choice=0;choice<3;++choice) {
            short count=choice==0?0:choice==1?1:maximum; char name[64];snprintf(name,sizeof(name),"data_selector_%d_length_%d",maximum,count);
            vector_init(v,data,3,(short)(maximum+3),1,1);version_event(v);count_event(v,0,count,maximum);raw_event(v,2,(unsigned)count,0x31);raw_event(v,2+(unsigned)maximum,1,0xe7);
            if(run_vector(ctx,v,name,o))return 1;
        }
    }
    const struct data_packet_field nested[]={F(_data_packet_field_array,2,0,0),F(_data_packet_field_array,2,0,0),F(_data_packet_field_shorts,1,0,0),E,F(_data_packet_field_bytes,1,0,0),E,F(_data_packet_field_bytes,1,0,0),E};
    vector_init(v,nested,8,17,1,1);version_event(v);count_event(v,0,2,2);
    for(unsigned outer=0;outer<2;++outer) {
        unsigned at=2+outer*7; count_event(v,at,(short)(outer+1),2);
        for(unsigned inner=0;inner<=outer;++inner) {unsigned native=at+2+inner*2; unsigned short value=(unsigned short)(0x1234+outer*0x100+inner); unsigned char bytes[2]={(unsigned char)(value>>8),(unsigned char)value}; memcpy(v->input+native,&value,2);memcpy(v->output+native,&value,2);add_event(v,native,2,bytes,2,1);}
        raw_event(v,at+6,1,0x71+outer);
    }
    raw_event(v,16,1,0xe7);if(run_vector(ctx,v,"nested_two_levels_short_and_tail",o))return 1;
    const struct data_packet_field empty[]={F(_data_packet_field_array,32767,0,0),E,F(_data_packet_field_bytes,1,0,0),E};
    vector_init(v,empty,4,3,1,1);version_event(v);count_event(v,0,32767,32767);raw_event(v,2,1,0xe7);if(run_vector(ctx,v,"empty_child_max_count_no_iteration",o))return 1;
    const struct data_packet_field zero_version[]={F(_data_packet_field_array,1,0,0),F(_data_packet_field_raw,1,0,0),E,E};
    vector_init(v,zero_version,4,3,0,0);count_event(v,0,1,1);raw_event(v,2,1,0x51);if(run_vector(ctx,v,"version_zero_has_no_prefix",o))return 1;
    return 0;
}

static int gated_vectors(struct array_context *ctx,struct vector *v,struct objects *o)
{
    for(short maximum=255;maximum<=256;++maximum) {
        struct data_packet_field fields[]={F(_data_packet_field_array,maximum,2,0),F(_data_packet_field_shorts,1,0,0),E,F(_data_packet_field_bytes,1,0,0),E};
        vector_init(v,fields,5,(short)(3+2*maximum),2,1);version_event(v);excluded_event(v,0,2+2*(unsigned)maximum,maximum<=255?1:2);raw_event(v,2+2*(unsigned)maximum,1,0xe7);
        if(run_vector(ctx,v,maximum==255?"excluded_array_selector255_skips_children":"excluded_array_selector256_skips_children",o))return 1;
    }
    const struct data_packet_field fields[]={F(_data_packet_field_array,2,0,0),F(_data_packet_field_shorts,1,2,2),F(_data_packet_field_bytes,1,0,0),E,F(_data_packet_field_bytes,1,0,0),E};
    for(short version=0;version<=3;++version) {
        char name[64];snprintf(name,sizeof(name),"nested_child_inclusive_gate_runtime_%d",version);vector_init(v,fields,6,9,3,version);version_event(v);count_event(v,0,2,2);
        for(unsigned n=0;n<2;++n) {unsigned at=2+3*n;if(version==2) {unsigned short value=(unsigned short)(0x1234+n);unsigned char bytes[2]={(unsigned char)(value>>8),(unsigned char)value};memcpy(v->input+at,&value,2);memcpy(v->output+at,&value,2);add_event(v,at,2,bytes,2,1);}else excluded_event(v,at,2,1);raw_event(v,at+2,1,0x41+n);}
        raw_event(v,8,1,0xe7);if(run_vector(ctx,v,name,o))return 1;
    }
    const struct data_packet_field latent[]={F(_data_packet_field_array,1,2,0),F(_data_packet_field_raw,3,0,0),E,F(_data_packet_field_bytes,1,0,0),E};
    vector_init(v,latent,5,6,1,1);version_event(v);excluded_event(v,0,5,1);raw_event(v,5,1,0xe7);if(run_vector(ctx,v,"own_version_excluded_still_reserves_full_extent",o))return 1;
    const struct data_packet_field composite[]={F(_data_packet_field_array,1,0,0),F(_data_packet_field_pad,2,2,0),F(_data_packet_field_string,3,0,0),F(_data_packet_field_data,3,0,0),E,E};
    for(unsigned string_size=3;string_size<=4;++string_size) {
        const char *text=string_size==3?"ab":"abc";
        vector_init(v,composite,6,13,2,1);version_event(v);count_event(v,0,1,1);excluded_event(v,2,2,0);
        memcpy(v->input+4,text,string_size);memcpy(v->output+4,text,string_size);add_event(v,4,string_size,(const unsigned char *)text,string_size,0);count_event(v,8,2,3);raw_event(v,10,2,0x61);
        if(run_vector(ctx,v,string_size==3?"nested_pad_string_data_partial_payload":"nested_string_maximum_nul_boundary",o))return 1;
    }
    return 0;
}

static int schema_case(struct array_context *ctx,const char *name,struct data_packet_field *fields,size_t count,size_t bound,short size,short version,int accepted,enum packet_array_error error)
{
    struct data_packet_definition d,before; struct packet_array_plan plan={0},zero={0};struct packet_array_result result;
    struct data_packet_field snapshot[FIELD_LIMIT];ctx->name=name;++ctx->cases;
    memcpy(snapshot,fields,count*sizeof(*fields));definition(&d,fields,size,version);memcpy(&before,&d,sizeof(d));
    boolean actual=fixture_compile(ctx,&d,bound,&plan,&result);
    CHECK(actual==accepted,"schema_acceptance");CHECK(result.error==error,"schema_structured_error");
    CHECK(memcmp(&before,&d,sizeof(d))==0 && memcmp(snapshot,fields,count*sizeof(*fields))==0,"schema_source_metadata_transaction");
    if(accepted) CHECK(plan.extent==(unsigned short)size,"schema_exact_full_extent");else CHECK(memcmp(&plan,&zero,sizeof(plan))==0,"failed_compile_plan_transaction");
    fixture_destroy(ctx,&plan);
    CHECK(memcmp(&plan,&zero,sizeof(plan))==0,"schema_plan_destroyed_or_unmodified");
    if(fprintf(ctx->report,"ARRAY SCHEMA name=%s accepted=%u error=%s index=%zu required=%u\n",name,(unsigned)actual,packet_array_error_name(result.error),result.field_index,(unsigned)result.native_required)<0 || fflush(ctx->report)!=0)return 1;
    return 0;
}
static int schema_vectors(struct array_context *ctx)
{
    struct data_packet_field valid[]={F(_data_packet_field_array,3,0,0),F(_data_packet_field_bytes,1,0,0),E,F(_data_packet_field_bytes,1,0,0),E};
    if(schema_case(ctx,"schema_simple_array",valid,5,5,6,1,1,PACKET_ARRAY_OK))return 1;
    struct data_packet_definition d;struct packet_array_plan plan={0};struct packet_array_result result;definition(&d,valid,6,1);
    boolean compiled=fixture_compile(ctx,&d,5,&plan,&result);
    CHECK(compiled,"schema_plan_for_node_snapshot");
    if(!compiled) {
        if(fprintf(ctx->report,"ARRAY ABORT case=schema_plan_for_node_snapshot error=%s\n",packet_array_error_name(result.error))<0 || fflush(ctx->report)!=0)return 1;
        return 1;
    }
    CHECK(plan.count==5 && plan.depth==2 && plan.nodes[0].extent==5 && plan.nodes[0].child_extent==1 && plan.nodes[0].next==3,"array_header_reserve_stride_and_child_skip");
    CHECK(plan.nodes[1].extent==1 && plan.nodes[3].extent==1,"child_and_tail_node_extents");
    fixture_destroy(ctx,&plan);
    struct data_packet_field root_missing[]={F(_data_packet_field_array,1,0,0),F(_data_packet_field_bytes,1,0,0),E};
    if(schema_case(ctx,"missing_root_after_closed_child",root_missing,3,3,3,1,0,PACKET_ARRAY_MISSING_END))return 1;
    struct data_packet_field child_missing[]={F(_data_packet_field_array,1,0,0),F(_data_packet_field_bytes,1,0,0)};
    if(schema_case(ctx,"missing_child_end_truthful_bound",child_missing,2,2,3,1,0,PACKET_ARRAY_MISSING_END))return 1;
    struct data_packet_field nested_missing[]={F(_data_packet_field_array,1,0,0),F(_data_packet_field_array,1,0,0),E,E};
    if(schema_case(ctx,"missing_nested_root_end",nested_missing,4,4,4,1,0,PACKET_ARRAY_MISSING_END))return 1;
    struct data_packet_field bad_child[]={F(_data_packet_field_array,1,2,0),F(-1,1,0,0),E,E};
    if(schema_case(ctx,"excluded_array_still_validates_child_type",bad_child,4,4,3,1,0,PACKET_ARRAY_SCHEMA))return 1;
    bad_child[1].type=_data_packet_field_bytes;bad_child[1].count=-1;
    if(schema_case(ctx,"excluded_array_rejects_negative_child_count",bad_child,4,4,3,1,0,PACKET_ARRAY_SCHEMA))return 1;
    bad_child[1].count=1;bad_child[1].minimum_version=2;bad_child[1].maximum_version=1;
    if(schema_case(ctx,"excluded_array_rejects_reversed_child_gate",bad_child,4,4,3,1,0,PACKET_ARRAY_SCHEMA))return 1;
    struct data_packet_field array_bad[]={F(_data_packet_field_array,0,0,0),E,E};
    if(schema_case(ctx,"array_nonpositive_schema_max",array_bad,3,3,2,1,0,PACKET_ARRAY_SCHEMA))return 1;
    array_bad[0].count=-1;if(schema_case(ctx,"array_negative_schema_max",array_bad,3,3,2,1,0,PACKET_ARRAY_SCHEMA))return 1;
    struct data_packet_field exact[]={F(_data_packet_field_array,1,0,0),F(_data_packet_field_raw,32765,0,0),E,E};
    if(schema_case(ctx,"array_reserve_exact32767",exact,4,4,32767,0,1,PACKET_ARRAY_OK))return 1;
    exact[1].count=32766;if(schema_case(ctx,"array_reserve_one_over32767",exact,4,4,32767,0,0,PACKET_ARRAY_EXTENT))return 1;
    struct data_packet_field multiplication[]={F(_data_packet_field_array,32767,0,0),F(_data_packet_field_raw,32767,0,0),E,E};
    if(schema_case(ctx,"array_large_product_rejected_before_reserve",multiplication,4,4,32767,0,0,PACKET_ARRAY_EXTENT))return 1;
    struct data_packet_field total[]={F(_data_packet_field_array,1,0,0),E,F(_data_packet_field_raw,32766,0,0),E};
    if(schema_case(ctx,"root_total_one_over32767",total,4,4,32767,0,0,PACKET_ARRAY_EXTENT))return 1;
    struct data_packet_field latent[]={F(_data_packet_field_array,1,2,0),F(_data_packet_field_raw,3,0,0),E,E};
    if(schema_case(ctx,"excluded_latent_reserve_size_mismatch",latent,4,4,0,1,0,PACKET_ARRAY_SIZE))return 1;
    if(schema_case(ctx,"negative_definition_size",valid,5,5,-1,1,0,PACKET_ARRAY_SCHEMA))return 1;
    if(schema_case(ctx,"version_above_byte",valid,5,5,6,256,0,PACKET_ARRAY_SCHEMA))return 1;
    if(schema_case(ctx,"zero_truthful_bound",valid,5,0,6,1,0,PACKET_ARRAY_ARGUMENT))return 1;
    if(schema_case(ctx,"bound_above_signed_index_representation",valid,5,32768,6,1,0,PACKET_ARRAY_ARGUMENT))return 1;
    struct data_packet_field unused[6];memcpy(unused,valid,sizeof(valid));unused[5]=(struct data_packet_field)F(-1,-1,-1,-1);
    if(schema_case(ctx,"root_end_stops_before_malformed_unused_suffix",unused,6,6,6,1,1,PACKET_ARRAY_OK))return 1;
    const struct data_packet_field deep_template=F(_data_packet_field_array,1,0,0);
    struct data_packet_field deep[FIELD_LIMIT];for(unsigned n=0;n<15;++n)deep[n]=deep_template;for(unsigned n=15;n<31;++n){struct data_packet_field end=E;deep[n]=end;}
    if(schema_case(ctx,"fifteen_nested_empty_arrays_explicit_stack",deep,31,31,30,1,1,PACKET_ARRAY_OK))return 1;
    definition(&d,deep,30,1);CHECK(fixture_compile(ctx,&d,31,&plan,&result),"deep_compile_for_depth_assertion");CHECK(plan.depth==16,"deep_stack_depth_matches_schema");fixture_destroy(ctx,&plan);
    return 0;
}

static int engine_shape_vectors(struct array_context *ctx,struct vector *v,struct objects *o)
{
    for(unsigned server=0;server<2;++server) {
        short maximum=server?128:4;unsigned prefix=server?12:4;short extent=(short)(prefix+4+32*(unsigned)maximum);
        struct data_packet_field fields[]={F(_data_packet_field_longs,server?3:1,0,0),F(_data_packet_field_pad,2,0,0),F(_data_packet_field_array,maximum,0,0),F(_data_packet_field_longs,6,0,0),F(_data_packet_field_shorts,3,0,0),F(_data_packet_field_pad,2,0,0),E,E};
        for(unsigned choice=0;choice<3;++choice) {
            short count=choice==0?0:choice==1?1:maximum;char name[96];snprintf(name,sizeof(name),"actual_%s_update_shape_count_%d",server?"server":"client",count);
            vector_init(v,fields,8,extent,1,1);version_event(v);words_event(v,0,server?3:1,4,0x12345678);count_event(v,prefix+2,count,maximum);
            for(unsigned n=0;n<(unsigned short)count;++n){unsigned at=prefix+4+32*n;words_event(v,at,6,4,0x11223344+n);words_event(v,at+24,3,2,0x1234+n);}
            if(run_vector(ctx,v,name,o))return 1;
        }
    }
    struct data_packet_field deep[31];const struct data_packet_field header=F(_data_packet_field_array,1,0,0),end=E;
    for(unsigned n=0;n<15;++n) deep[n]=header;
    for(unsigned n=15;n<31;++n) deep[n]=end;
    vector_init(v,deep,31,30,1,1);version_event(v);for(unsigned n=0;n<15;++n)count_event(v,2*n,1,1);
    if(run_vector(ctx,v,"fifteen_nested_empty_arrays_runtime_stack",o))return 1;
    return 0;
}

static int rejected_runtime(struct array_context *ctx,struct vector *v,struct objects *o)
{
    for(short maximum=255;maximum<=256;++maximum) {
        for(unsigned type_choice=0;type_choice<2;++type_choice) {
            struct data_packet_field fields[]={F(_data_packet_field_array,maximum,0,0),F(_data_packet_field_bytes,1,0,0),E,E};
            size_t count=4;if(type_choice){fields[0].type=_data_packet_field_data;fields[1].type=_data_packet_field_end;count=2;}
            vector_init(v,fields,count,(short)(maximum+2),1,1);struct data_packet_definition d;struct packet_array_plan plan={0};struct packet_array_result result;definition(&d,v->fields,v->size,1);
            CHECK(fixture_compile(ctx,&d,count,&plan,&result),"invalid_runtime_test_compile");
            for(unsigned offset=0;offset<8;++offset) {
                unsigned base=GUARD+offset;
                for(unsigned bad=0;bad<2;++bad) {
                    short number=bad?-1:(short)(maximum+1);char name[96];snprintf(name,sizeof(name),"invalid_%s_native_selector%d_value%d_offset%u",type_choice?"data":"array",maximum,number,offset);ctx->name=name;++ctx->cases;
                    memset(o->native,0xc3,sizeof(o->native));memcpy(o->native+base,&number,2);memcpy(o->expected_native,o->native,sizeof(o->native));
                    memset(o->wire,0xcc,sizeof(o->wire));memcpy(o->expected_wire,o->wire,sizeof(o->wire));o->expected_wire[base]=1;
                    CHECK(!packet_array_encode(&plan,1,o->native+base,v->size,o->wire+base,8,&result) && result.error==PACKET_ARRAY_COUNT && result.wire_used==1,"invalid_native_count_before_length_io");
                    CHECK(memcmp(o->native,o->expected_native,sizeof(o->native))==0 && memcmp(o->wire,o->expected_wire,sizeof(o->wire))==0,"invalid_native_count_whole_canaries");
                }
                /* One-byte selector cannot represent 256; every encoded value is <=255. */
                if(maximum==256) for(unsigned bad=0;bad<2;++bad) {
                    short number=bad?-1:257;ctx->name=type_choice?"invalid_wire_data_signed_count":"invalid_wire_array_signed_count";++ctx->cases;
                    memset(o->native,0xc3,sizeof(o->native));memcpy(o->expected_native,o->native,sizeof(o->native));
                    memset(o->wire,0xcc,sizeof(o->wire));o->wire[base]=1;o->wire[base+1]=(unsigned char)((unsigned short)number>>8);o->wire[base+2]=(unsigned char)number;memcpy(o->expected_wire,o->wire,sizeof(o->wire));memcpy(o->expected_wire+base+1,&number,2);
                    short version=-111;CHECK(!packet_array_decode(&plan,o->wire+base,3,o->native+base,v->size,&version,&result) && result.error==PACKET_ARRAY_COUNT && result.wire_used==3,"invalid_wire_count_after_prefix_before_native_write");
                    CHECK(memcmp(o->native,o->expected_native,sizeof(o->native))==0 && memcmp(o->wire,o->expected_wire,sizeof(o->wire))==0,"invalid_wire_count_canaries_and_prefix_mutation");
                }
            }
            fixture_destroy(ctx,&plan);
        }
    }
    const struct data_packet_field basic[]={F(_data_packet_field_array,1,0,0),F(_data_packet_field_bytes,1,0,0),E,E};
    vector_init(v,basic,4,3,1,1);struct data_packet_definition d;struct packet_array_plan plan={0};struct packet_array_result result;definition(&d,v->fields,3,1);
    CHECK(fixture_compile(ctx,&d,4,&plan,&result),"version_and_signed_bounds_compile");
    for(unsigned offset=0;offset<8;++offset) {
        unsigned base=GUARD+offset;memset(o->native,0xc3,sizeof(o->native));memcpy(o->expected_native,o->native,sizeof(o->native));memset(o->wire,0xcc,sizeof(o->wire));memcpy(o->expected_wire,o->wire,sizeof(o->wire));
        for(unsigned placement=0;placement<3;++placement) {
            unsigned native_at=base+(placement==2?2:0),wire_at=base+(placement==1?2:0);
            memset(o->native,0xc3,sizeof(o->native));memcpy(o->expected_native,o->native,sizeof(o->native));short overlap_version=-111;
            ctx->name=placement==0?"same_pointer_overlap":placement==1?"wire_inside_native_overlap":"native_inside_wire_overlap";++ctx->cases;
            CHECK(!packet_array_encode(&plan,1,o->native+native_at,3,o->native+wire_at,3,&result) && result.error==PACKET_ARRAY_OVERLAP && result.wire_used==0,"encode_alias_rejected_before_any_io");
            CHECK(memcmp(o->native,o->expected_native,sizeof(o->native))==0,"encode_overlap_full_single_object_unchanged");
            CHECK(!packet_array_decode(&plan,o->native+wire_at,3,o->native+native_at,3,&overlap_version,&result) && result.error==PACKET_ARRAY_OVERLAP && result.wire_used==0,"decode_alias_rejected_before_any_io");
            CHECK(memcmp(o->native,o->expected_native,sizeof(o->native))==0,"decode_overlap_full_single_object_unchanged");
        }
        short boundary_count=0;memset(o->native,0xc3,sizeof(o->native));memcpy(o->native+base+3,&boundary_count,2);memcpy(o->expected_native,o->native,sizeof(o->native));o->expected_native[base]=1;o->expected_native[base+1]=0;ctx->name="disjoint_touching_encode_objects";++ctx->cases;
        CHECK(packet_array_encode(&plan,1,o->native+base+3,3,o->native+base,3,&result) && result.error==PACKET_ARRAY_OK && result.wire_used==2,"encode_touching_boundary_allowed");
        CHECK(memcmp(o->native,o->expected_native,sizeof(o->native))==0,"encode_disjoint_boundary_full_single_object");
        memset(o->native,0xc3,sizeof(o->native));o->native[base]=1;o->native[base+1]=0;memcpy(o->expected_native,o->native,sizeof(o->native));memcpy(o->expected_native+base+2,&boundary_count,2);short boundary_version=-111;ctx->name="disjoint_touching_decode_objects";++ctx->cases;
        CHECK(packet_array_decode(&plan,o->native+base,2,o->native+base+2,3,&boundary_version,&result) && result.error==PACKET_ARRAY_OK && result.wire_used==2 && boundary_version==1,"decode_touching_boundary_allowed");
        CHECK(memcmp(o->native,o->expected_native,sizeof(o->native))==0,"decode_disjoint_boundary_full_single_object");
        memset(o->native,0xc3,sizeof(o->native));memcpy(o->expected_native,o->native,sizeof(o->native));
        const long versions[]={-2,2,256,LONG_MAX};
        for(unsigned n=0;n<sizeof(versions)/sizeof(versions[0]);++n) {ctx->name="encode_invalid_version_no_io";++ctx->cases;CHECK(!packet_array_encode(&plan,versions[n],o->native+base,3,o->wire+base,8,&result) && result.error==PACKET_ARRAY_VERSION && result.wire_used==0,"encode_version_range_before_io");CHECK(memcmp(o->native,o->expected_native,sizeof(o->native))==0 && memcmp(o->wire,o->expected_wire,sizeof(o->wire))==0,"encode_version_whole_objects");}
        const long lengths[]={-1,32768,LONG_MAX};
        for(unsigned n=0;n<sizeof(lengths)/sizeof(lengths[0]);++n) {short version=-111;ctx->name="invalid_signed_wire_extent";++ctx->cases;CHECK(!packet_array_decode(&plan,o->wire+base,lengths[n],o->native+base,3,&version,&result) && result.error==PACKET_ARRAY_LENGTH && result.wire_used==0,"decode_signed_length_range_before_io");CHECK(!packet_array_encode(&plan,1,o->native+base,3,o->wire+base,lengths[n],&result) && result.error==PACKET_ARRAY_LENGTH && result.wire_used==0,"encode_signed_capacity_range_before_io");CHECK(memcmp(o->native,o->expected_native,sizeof(o->native))==0 && memcmp(o->wire,o->expected_wire,sizeof(o->wire))==0,"signed_length_whole_objects");}
        o->wire[base]=2;memcpy(o->expected_wire,o->wire,sizeof(o->wire));short version=-111;ctx->name="decode_version_above_own";++ctx->cases;
        CHECK(!packet_array_decode(&plan,o->wire+base,1,o->native+base,3,&version,&result) && result.error==PACKET_ARRAY_VERSION && result.wire_used==1,"decode_rejects_future_version_after_prefix");CHECK(memcmp(o->native,o->expected_native,sizeof(o->native))==0 && memcmp(o->wire,o->expected_wire,sizeof(o->wire))==0,"future_version_whole_objects");
        ctx->name="negative_native_capacity";++ctx->cases;
        CHECK(!packet_array_decode(&plan,o->wire+base,1,o->native+base,-1,&version,&result) && result.error==PACKET_ARRAY_CAPACITY && result.wire_used==0,"negative_decode_native_capacity_before_io");
        CHECK(!packet_array_encode(&plan,1,o->native+base,-1,o->wire+base,8,&result) && result.error==PACKET_ARRAY_CAPACITY && result.wire_used==0,"negative_encode_native_capacity_before_io");
        CHECK(memcmp(o->native,o->expected_native,sizeof(o->native))==0 && memcmp(o->wire,o->expected_wire,sizeof(o->wire))==0,"negative_native_capacity_whole_objects");
        short zero_count=0;memcpy(o->native+base,&zero_count,2);memcpy(o->expected_native,o->native,sizeof(o->native));memset(o->wire,0xcc,sizeof(o->wire));memcpy(o->expected_wire,o->wire,sizeof(o->wire));o->expected_wire[base]=1;o->expected_wire[base+1]=0;ctx->name="none_version_uses_definition";++ctx->cases;
        CHECK(packet_array_encode(&plan,NONE,o->native+base,3,o->wire+base,8,&result) && result.error==PACKET_ARRAY_OK && result.wire_used==2,"none_version_default");
        CHECK(memcmp(o->native,o->expected_native,sizeof(o->native))==0 && memcmp(o->wire,o->expected_wire,sizeof(o->wire))==0,"none_version_full_objects");
    }
    fixture_destroy(ctx,&plan);
    const struct data_packet_field raw_max[]={F(_data_packet_field_raw,32767,0,0),E};
    struct data_packet_field max_fields[2];memcpy(max_fields,raw_max,sizeof(max_fields));definition(&d,max_fields,32767,1);
    CHECK(fixture_compile(ctx,&d,2,&plan,&result),"maximum_native_wire_overflow_compile");
    unsigned char *large_native=fixture_allocate(ctx,32767,"maximum_native"),*large_wire=fixture_allocate(ctx,32769,"maximum_wire_or_snapshot"),*large_snapshot=fixture_allocate(ctx,32769,"maximum_wire_or_snapshot");
    if(!large_native || !large_wire || !large_snapshot) {fixture_release(ctx,large_native);fixture_release(ctx,large_wire);fixture_release(ctx,large_snapshot);fixture_destroy(ctx,&plan);return 1;}
    memset(large_native,0x41,32767);memset(large_wire,0xcc,32769);memcpy(large_snapshot,large_wire,32769);large_snapshot[1]=1;ctx->name="native32767_plus_version_wire_overflow";++ctx->cases;
    CHECK(!packet_array_encode(&plan,1,large_native,32767,large_wire+1,32767,&result) && result.error==PACKET_ARRAY_WIRE && result.wire_used==1,"wire_limit_without_signed_wrap");
    CHECK(memcmp(large_wire,large_snapshot,32769)==0,"maximum_wire_atomic_raw_and_canaries");
    fixture_release(ctx,large_native);fixture_release(ctx,large_wire);fixture_release(ctx,large_snapshot);fixture_destroy(ctx,&plan);
    const struct data_packet_field string_schema[]={F(_data_packet_field_array,1,0,0),F(_data_packet_field_string,3,0,0),E,E};
    struct data_packet_field string_fields[4];memcpy(string_fields,string_schema,sizeof(string_fields));definition(&d,string_fields,6,1);CHECK(fixture_compile(ctx,&d,4,&plan,&result),"string_invalid_runtime_compile");
    for(unsigned offset=0;offset<8;++offset) {
        unsigned base=GUARD+offset;short one=1;memset(o->native,0xc3,sizeof(o->native));memcpy(o->native+base,&one,2);memcpy(o->expected_native,o->native,sizeof(o->native));memset(o->wire,0xcc,sizeof(o->wire));memcpy(o->expected_wire,o->wire,sizeof(o->wire));o->expected_wire[base]=1;o->expected_wire[base+1]=1;ctx->name="encode_string_missing_nul";++ctx->cases;
        CHECK(!packet_array_encode(&plan,1,o->native+base,6,o->wire+base,8,&result) && result.error==PACKET_ARRAY_STRING && result.wire_used==2,"encode_string_bounds_and_no_string_write");
        CHECK(memcmp(o->native,o->expected_native,sizeof(o->native))==0 && memcmp(o->wire,o->expected_wire,sizeof(o->wire))==0,"encode_bad_string_full_objects");
        memset(o->native,0xc3,sizeof(o->native));memcpy(o->expected_native,o->native,sizeof(o->native));memcpy(o->expected_native+base,&one,2);memset(o->wire,0xcc,sizeof(o->wire));o->wire[base]=1;o->wire[base+1]=1;memcpy(o->expected_wire,o->wire,sizeof(o->wire));short version=-111;ctx->name="decode_string_no_nul_in_full_reserve";++ctx->cases;
        CHECK(!packet_array_decode(&plan,o->wire+base,6,o->native+base,6,&version,&result) && result.error==PACKET_ARRAY_STRING && result.wire_used==2,"decode_string_full_bound_rejection");
        CHECK(memcmp(o->native,o->expected_native,sizeof(o->native))==0 && memcmp(o->wire,o->expected_wire,sizeof(o->wire))==0,"decode_bad_string_count_only_and_input_preserved");
    }
    fixture_destroy(ctx,&plan);
    return 0;
}

static int representation_limits(struct array_context *ctx)
{
    enum { LARGE_OBJECT=33024 };
    struct large_objects { unsigned char native[LARGE_OBJECT],wire[LARGE_OBJECT],expected_native[LARGE_OBJECT],expected_wire[LARGE_OBJECT]; };
    struct data_packet_field *fields=fixture_allocate(ctx,(size_t)SHRT_MAX*sizeof(*fields),"maximum_schema");
    struct data_packet_field *snapshot=fixture_allocate(ctx,(size_t)SHRT_MAX*sizeof(*snapshot),"maximum_schema_snapshot");
    struct large_objects *o=fixture_allocate(ctx,sizeof(*o),"maximum_guarded_objects");
    struct packet_array_plan plan={0};struct packet_array_result result;
    int aborted=0;
    if(!fields || !snapshot || !o) {
        if(fprintf(ctx->report,"ARRAY ABORT representation_limit_allocation_failed\n")<0 || fflush(ctx->report)!=0) aborted=1;
        aborted=1;goto cleanup;
    }
#define LIMIT_CHECK(c,n) do {if(!array_check(ctx,(c),(n))) {aborted=1;goto cleanup;}} while(0)
    for(unsigned shape=0;shape<2;++shape) {
        size_t field_count=shape?4:(size_t)SHRT_MAX;
        short native_size=shape?SHRT_MAX:32766;
        unsigned wire_size=shape?(unsigned)SHRT_MAX:16383;
        const struct data_packet_field header=F(_data_packet_field_array,1,0,0),end=E;
        if(!shape) {
            for(unsigned n=0;n<16383;++n)fields[n]=header;
            for(unsigned n=16383;n<(unsigned)SHRT_MAX;++n)fields[n]=end;
        } else {
            fields[0]=(struct data_packet_field)F(_data_packet_field_array,32765,0,0);
            fields[1]=(struct data_packet_field)F(_data_packet_field_bytes,1,0,0);
            fields[2]=end;fields[3]=end;
        }
        memcpy(snapshot,fields,field_count*sizeof(*fields));struct data_packet_definition d,before;
        definition(&d,fields,native_size,0);memcpy(&before,&d,sizeof(d));
        ctx->name=shape?"maximum_native_visits_with_repeated_end_control":"maximum_truthful_schema_depth";++ctx->cases;
        boolean accepted=fixture_compile(ctx,&d,field_count,&plan,&result);
        LIMIT_CHECK(accepted && result.error==PACKET_ARRAY_OK,"representation_limit_compile");
        if(!accepted){aborted=1;goto cleanup;}
        LIMIT_CHECK(plan.count==field_count && plan.depth==(shape?2:16384) && plan.extent==(unsigned short)native_size,"representation_limit_bound_depth_extent");
        LIMIT_CHECK(memcmp(fields,snapshot,field_count*sizeof(*fields))==0 && memcmp(&d,&before,sizeof(d))==0,"representation_limit_full_source_snapshot");
        if(fprintf(ctx->report,"ARRAY LIMIT name=%s fields=%zu depth=%zu native=%u wire=%u offsets=0-7 valid_and_one_byte_truncated=1\n",ctx->name,field_count,plan.depth,(unsigned)plan.extent,wire_size)<0 || fflush(ctx->report)!=0){aborted=1;goto cleanup;}
        for(unsigned offset=0;offset<8;++offset) {
            unsigned base=GUARD+offset;ctx->name=shape?"maximum_native_visit_runtime":"maximum_depth_runtime";++ctx->cases;
            for(unsigned truncated=0;truncated<2;++truncated) {
                unsigned capacity=wire_size-truncated;
                memset(o->native,0xc3,sizeof(o->native));memset(o->wire,0xcc,sizeof(o->wire));
                if(!shape) {
                    short one=1;
                    for(unsigned n=0;n<16383;++n)memcpy(o->native+base+2*n,&one,2);
                } else {
                    short maximum=32765;memcpy(o->native+base,&maximum,2);
                    for(unsigned n=0;n<32765;++n)o->native[base+2+n]=(unsigned char)(n+0x31);
                }
                memcpy(o->expected_native,o->native,sizeof(o->native));memcpy(o->expected_wire,o->wire,sizeof(o->wire));
                if(!shape) memset(o->expected_wire+base,1,capacity);
                else {o->expected_wire[base]=0x7f;o->expected_wire[base+1]=0xfd;for(unsigned n=0;n<capacity-2;++n)o->expected_wire[base+2+n]=(unsigned char)(n+0x31);}
                boolean encoded=packet_array_encode(&plan,0,o->native+base,native_size,o->wire+base,capacity,&result);
                LIMIT_CHECK(encoded==(truncated==0) && result.error==(truncated?PACKET_ARRAY_WIRE:PACKET_ARRAY_OK) && result.wire_used==(long)capacity,"representation_limit_encode_status_and_cursor");
                LIMIT_CHECK(memcmp(o->native,o->expected_native,sizeof(o->native))==0,"representation_limit_encode_input_and_full_canaries");
                LIMIT_CHECK(memcmp(o->wire,o->expected_wire,sizeof(o->wire))==0,"representation_limit_independent_wire_golden");
                memset(o->native,0xc3,sizeof(o->native));memset(o->native+base,0x5a,(unsigned short)native_size);memcpy(o->expected_native,o->native,sizeof(o->native));
                memset(o->wire,0xcc,sizeof(o->wire));
                if(!shape) {
                    memset(o->wire+base,1,wire_size);short one=1;for(unsigned n=0;n<capacity;++n)memcpy(o->expected_native+base+2*n,&one,2);
                } else {
                    o->wire[base]=0x7f;o->wire[base+1]=0xfd;short maximum=32765;memcpy(o->expected_native+base,&maximum,2);
                    for(unsigned n=0;n<32765;++n)o->wire[base+2+n]=(unsigned char)(n+0x31);
                    for(unsigned n=0;n<capacity-2;++n)o->expected_native[base+2+n]=(unsigned char)(n+0x31);
                }
                memcpy(o->expected_wire,o->wire,sizeof(o->wire));
                if(shape){short maximum=32765;memcpy(o->expected_wire+base,&maximum,2);}
                short version=-111;boolean decoded=packet_array_decode(&plan,o->wire+base,capacity,o->native+base,native_size,&version,&result);
                LIMIT_CHECK(decoded==(truncated==0) && result.error==(truncated?PACKET_ARRAY_WIRE:PACKET_ARRAY_OK) && result.wire_used==(long)capacity && version==0,"representation_limit_decode_status_and_cursor");
                LIMIT_CHECK(memcmp(o->native,o->expected_native,sizeof(o->native))==0,"representation_limit_partial_native_prefix_and_canaries");
                LIMIT_CHECK(memcmp(o->wire,o->expected_wire,sizeof(o->wire))==0,"representation_limit_input_prefix_mutation_and_canaries");
            }
        }
        LIMIT_CHECK(memcmp(fields,snapshot,field_count*sizeof(*fields))==0 && memcmp(&d,&before,sizeof(d))==0,"representation_limit_source_unchanged_after_runtime");
        fixture_destroy(ctx,&plan);
    }
cleanup:
    fixture_destroy(ctx,&plan);fixture_release(ctx,fields);fixture_release(ctx,snapshot);fixture_release(ctx,o);
#undef LIMIT_CHECK
    return aborted;
}

int wii_packet_array_fixture(FILE *report,int collect)
{
    struct array_context ctx={.report=report,.name="startup",.collect=collect};
    struct vector *v=malloc(sizeof(*v));struct objects *o=malloc(sizeof(*o));
    if(!v || !o) {free(v);free(o);if(fprintf(report,"ARRAY ABORT allocation_failed\n")<0 || fflush(report)!=0)return 1;return 1;}
    if(fprintf(report,"ARRAY BEGIN policy=bounded_recursive_stable_full_reserve trailing_bytes=accepted arrays=paired_child_skip source_metadata=immutable\n")<0 || fflush(report)!=0){free(v);free(o);return 1;}
    int aborted=basic_vectors(&ctx,v,o) || gated_vectors(&ctx,v,o) || schema_vectors(&ctx) || engine_shape_vectors(&ctx,v,o) || rejected_runtime(&ctx,v,o) || representation_limits(&ctx);
    free(v);free(o);
    for(unsigned n=0;n<8;++n)free(ctx.owned[n]);
    if(fprintf(report,"ARRAY SUMMARY cases=%u checks=%u failures=%u aborted=%u\nARRAY END result=%u\n",ctx.cases,ctx.checks,ctx.failures,aborted!=0,aborted || ctx.failures!=0)<0 || fflush(report)!=0)return 1;
    return aborted || ctx.failures!=0;
}
