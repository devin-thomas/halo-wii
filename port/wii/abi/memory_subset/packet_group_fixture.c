#include "packet_group_policy.h"
#include "packet_group_fixture.h"
#include <limits.h>

/* The original public body has no declaration in its production header. */
boolean data_packet_group_append_packet_header(struct data_packet_group_definition *,void *,short *,short);
const char *packet_group_reference_peek_error(void);

enum { GROUP_OBJECT=512, GROUP_GUARD=16, GROUP_ENTRIES=256 };
struct group_context { FILE *report;const char *name;unsigned checks,failures,cases;int collect; };
union aligned_group_object { uint64_t alignment;byte bytes[GROUP_OBJECT]; };
struct group_storage {
    union aligned_group_object native,wire,expected_native,expected_wire;
    struct data_packet_field fields[2];struct data_packet_definition definition;
    struct packet_array_plan payload;
    struct data_packet_field array_fields[4];struct data_packet_definition array_definition;
    struct packet_array_plan array_payload;
    struct packet_group_entry entries[GROUP_ENTRIES],entries_before[GROUP_ENTRIES];
    struct data_packet_entry original_entries[GROUP_ENTRIES];
};
static int group_check(struct group_context *ctx,int condition,const char *assertion)
{
    ++ctx->checks;if(condition)return 1;++ctx->failures;
    if(fprintf(ctx->report,"GROUP FAIL case=%s assertion=%s check=%u\n",ctx->name,assertion,ctx->checks)<0 || fflush(ctx->report)!=0)return 0;
    return ctx->collect;
}
#define CHECK(c,n) do {if(!group_check(ctx,(c),(n)))return 1;} while(0)
static struct packet_group_plan group_plan(struct group_storage *s)
{
    return (struct packet_group_plan){3,2,4,8,s->entries};
}
static void objects(struct group_storage *s,unsigned base,const byte *wire,size_t length)
{
    memset(s->native.bytes,0x5a,GROUP_OBJECT);memset(s->wire.bytes,0xcc,GROUP_OBJECT);
    if(length)memcpy(s->wire.bytes+base,wire,length);
    memcpy(s->expected_native.bytes,s->native.bytes,GROUP_OBJECT);memcpy(s->expected_wire.bytes,s->wire.bytes,GROUP_OBJECT);
}
static int decode_case(struct group_context *ctx,struct group_storage *s,const struct packet_group_plan *g,size_t bound,
                       const char *name,const byte *wire,short supplied,long capacity,long native_capacity,short expected_class,
                       enum packet_group_error error,short size_after,short type_after,short version_after,long used,long payload_length,int writes_payload)
{
    ctx->name=name;++ctx->cases;
    for(unsigned offset=0;offset<8;++offset) {
        unsigned base=GROUP_GUARD+offset;objects(s,base,wire,supplied>0?(size_t)supplied:0);
        if(writes_payload){s->expected_native.bytes[base]=0x21;s->expected_native.bytes[base+1]=0x32;}
        short size=supplied,type=-111,version=-112;struct packet_group_result result;
        boolean accepted=packet_group_decode(g,bound,s->native.bytes+base,native_capacity,s->wire.bytes+base,capacity,&size,&type,&version,expected_class,&result);
        CHECK(accepted==(error==PACKET_GROUP_OK) && result.error==error,"candidate_decode_acceptance_and_error");
        CHECK(size==size_after && type==type_after && version==version_after,"candidate_decode_output_order_and_trailer_removal");
        CHECK(result.wire_used==used && result.payload_length==payload_length && result.supplied_length==(error==PACKET_GROUP_SCHEMA?0:supplied),"candidate_decode_supplied_vs_consumed_payload");
        if(error==PACKET_GROUP_PAYLOAD) {
            enum packet_array_error nested=native_capacity<2?PACKET_ARRAY_CAPACITY:wire[0]>1?PACKET_ARRAY_VERSION:PACKET_ARRAY_WIRE;
            CHECK(result.payload.error==nested,"candidate_decode_nested_payload_reason");
        }
        CHECK(memcmp(s->native.bytes,s->expected_native.bytes,GROUP_OBJECT)==0,"candidate_decode_full_native_canaries");
        CHECK(memcmp(s->wire.bytes,s->expected_wire.bytes,GROUP_OBJECT)==0,"candidate_decode_full_wire_preservation");
    }
    if(fprintf(ctx->report,"GROUP CASE name=%s operation=decode supplied=%d after=%d consumed=%ld payload_length=%ld error=%s offsets=0-7\n",name,supplied,size_after,used,payload_length,packet_group_error_name(error))<0 || fflush(ctx->report)!=0)return 1;
    return 0;
}
static int candidate_decode_cases(struct group_context *ctx,struct group_storage *s)
{
    struct packet_group_plan g=group_plan(s);
    const byte valid[]={1,0x21,0x32,0},trailing[]={1,0x21,0x32,0xe7,0xe8,0},future[]={2,0x21,0x32,0};
    if(decode_case(ctx,s,&g,3,"exact_type_trailer",valid,4,8,2,0,PACKET_GROUP_OK,3,0,1,3,3,1) ||
       decode_case(ctx,s,&g,3,"unconsumed_before_trailer",trailing,6,8,2,0,PACKET_GROUP_OK,5,0,1,3,5,1) ||
       decode_case(ctx,s,&g,3,"future_payload_version",future,4,8,2,0,PACKET_GROUP_PAYLOAD,3,-111,2,1,3,0) ||
       decode_case(ctx,s,&g,3,"native_capacity_failure_after_trailer",valid,4,8,1,0,PACKET_GROUP_PAYLOAD,3,-111,-112,0,3,0))return 1;
    for(unsigned n=0;n<3;++n){byte truncated[4]={1,0x21,0x32,0};truncated[n]=0;char name[64];snprintf(name,sizeof(name),"payload_truncated_%u",n);
        if(decode_case(ctx,s,&g,3,name,truncated,(short)(n+1),8,2,0,PACKET_GROUP_PAYLOAD,(short)n,-111,n?1:-112,n?1:0,n,0))return 1;
    }
    const byte null_definition[]={0xe7,0xe8,1},bad_type[]={3},bad_class[]={1},high80[]={0x80},highff[]={0xff};
    if(decode_case(ctx,s,&g,3,"null_definition_type_only",null_definition,3,8,0,1,PACKET_GROUP_OK,2,1,-112,0,2,0) ||
       decode_case(ctx,s,&g,3,"invalid_trailer_type",bad_type,1,8,2,0,PACKET_GROUP_TYPE,1,-111,-112,0,0,0) ||
       decode_case(ctx,s,&g,3,"mismatched_class_retains_trailer",bad_class,1,8,2,0,PACKET_GROUP_CLASS,1,-111,-112,0,0,0) ||
       decode_case(ctx,s,&g,3,"portable_rejects_trailer80",high80,1,8,2,0,PACKET_GROUP_TYPE,1,-111,-112,0,0,0) ||
       decode_case(ctx,s,&g,3,"portable_rejects_trailerff",highff,1,8,2,0,PACKET_GROUP_TYPE,1,-111,-112,0,0,0) ||
       decode_case(ctx,s,&g,3,"no_header",valid,0,8,2,0,PACKET_GROUP_NO_HEADER,0,-111,-112,0,0,0) ||
       decode_case(ctx,s,&g,3,"negative_supplied_length",valid,-1,8,2,0,PACKET_GROUP_LENGTH,-1,-111,-112,0,0,0) ||
       decode_case(ctx,s,&g,3,"supplied_exceeds_actual_capacity",valid,4,3,2,0,PACKET_GROUP_CAPACITY,4,-111,-112,0,0,0) ||
       decode_case(ctx,s,&g,3,"negative_actual_wire_capacity",valid,4,-1,2,0,PACKET_GROUP_CAPACITY,4,-111,-112,0,0,0) ||
       decode_case(ctx,s,&g,3,"actual_wire_capacity_above_short",valid,4,32768,2,0,PACKET_GROUP_LENGTH,4,-111,-112,0,0,0) ||
       decode_case(ctx,s,&g,3,"negative_actual_native_capacity",valid,4,8,-1,0,PACKET_GROUP_CAPACITY,4,-111,-112,0,0,0) ||
       decode_case(ctx,s,&g,3,"invalid_expected_class_negative",valid,4,8,2,-1,PACKET_GROUP_CLASS,4,-111,-112,0,0,0) ||
       decode_case(ctx,s,&g,3,"invalid_expected_class_ceiling",valid,4,8,2,2,PACKET_GROUP_CLASS,4,-111,-112,0,0,0) ||
       decode_case(ctx,s,&g,2,"truthful_table_bound_too_short",valid,4,8,2,0,PACKET_GROUP_SCHEMA,4,-111,-112,0,0,0))return 1;
    g.maximum_encoded_size=4;g.type_count=1;
    if(decode_case(ctx,s,&g,3,"incoming_above_configured_max_retains_legacy_semantics",trailing,6,8,2,0,PACKET_GROUP_OK,5,0,1,3,5,1))return 1;
    g=group_plan(s);g.type_count=128;const byte highest[]={1,0x21,0x32,127};
    if(decode_case(ctx,s,&g,128,"portable_type127",highest,4,8,2,0,PACKET_GROUP_OK,3,127,1,3,3,1))return 1;
    return 0;
}
static int candidate_encode_cases(struct group_context *ctx,struct group_storage *s)
{
    const byte payload[]={1,0x21,0x32};struct packet_group_plan g=group_plan(s);
    for(unsigned offset=0;offset<8;++offset) {
        unsigned base=GROUP_GUARD+offset;
        for(long capacity=0;capacity<=8;++capacity) {
            ctx->name="encode_actual_capacity_boundary";++ctx->cases;objects(s,base,NULL,0);s->native.bytes[base]=0x21;s->native.bytes[base+1]=0x32;memcpy(s->expected_native.bytes,s->native.bytes,GROUP_OBJECT);
            unsigned used=capacity==0?0:capacity<3?1:capacity==3?3:4;enum packet_group_error error=capacity<3?PACKET_GROUP_PAYLOAD:capacity==3?PACKET_GROUP_APPEND:PACKET_GROUP_OK;
            unsigned payload_used=used==4?3:used;memcpy(s->expected_wire.bytes+base,payload,payload_used);if(used==4)s->expected_wire.bytes[base+3]=0;
            short size=-113;struct packet_group_result result;boolean accepted=packet_group_encode(&g,3,s->native.bytes+base,2,s->wire.bytes+base,capacity,&size,0,NONE,&result);
            CHECK(accepted==(error==PACKET_GROUP_OK) && result.error==error,"encode_capacity_error");CHECK(size==(short)used && result.wire_used==(long)used && result.payload_length==(long)payload_used,"encode_retained_payload_offset_and_trailer_size");
            CHECK(memcmp(s->wire.bytes,s->expected_wire.bytes,GROUP_OBJECT)==0 && memcmp(s->native.bytes,s->expected_native.bytes,GROUP_OBJECT)==0,"encode_whole_objects_and_exact_trailer");
            if(error==PACKET_GROUP_PAYLOAD)CHECK(result.payload.error==PACKET_ARRAY_WIRE,"encode_nested_payload_capacity_reason");
        }
        for(long maximum=3;maximum<=5;++maximum) {
            ctx->name="strict_configured_append_equality_and_slack";++ctx->cases;g.maximum_encoded_size=maximum;g.type_count=1;objects(s,base,NULL,0);s->native.bytes[base]=0x21;s->native.bytes[base+1]=0x32;memcpy(s->expected_native.bytes,s->native.bytes,GROUP_OBJECT);memcpy(s->expected_wire.bytes+base,payload,3);if(maximum==5)s->expected_wire.bytes[base+3]=0;
            short size=-113;struct packet_group_result result;boolean accepted=packet_group_encode(&g,3,s->native.bytes+base,2,s->wire.bytes+base,8,&size,0,1,&result);
            CHECK(accepted==(maximum==5) && result.error==(maximum==5?PACKET_GROUP_OK:PACKET_GROUP_APPEND),"strict_append_equality_not_slack");CHECK(size==(maximum==5?4:3),"strict_append_size_retains_payload");CHECK(memcmp(s->wire.bytes,s->expected_wire.bytes,GROUP_OBJECT)==0 && memcmp(s->native.bytes,s->expected_native.bytes,GROUP_OBJECT)==0,"strict_append_full_objects");
        }
        g=group_plan(s);
        const short types[]={-1,3,1};const enum packet_group_error errors[]={PACKET_GROUP_TYPE,PACKET_GROUP_TYPE,PACKET_GROUP_NO_DEFINITION};
        for(unsigned n=0;n<3;++n){ctx->name="encode_invalid_type_or_null_definition";++ctx->cases;objects(s,base,NULL,0);short size=-113;struct packet_group_result result;
            CHECK(!packet_group_encode(&g,3,s->native.bytes+base,2,s->wire.bytes+base,8,&size,types[n],1,&result) && result.error==errors[n] && size==-113,"encode_preflight_preserves_size");CHECK(memcmp(s->native.bytes,s->expected_native.bytes,GROUP_OBJECT)==0 && memcmp(s->wire.bytes,s->expected_wire.bytes,GROUP_OBJECT)==0,"encode_preflight_full_objects");}
        ctx->name="encode_future_version_payload_failure";++ctx->cases;objects(s,base,NULL,0);short size=-113;struct packet_group_result result;
        CHECK(!packet_group_encode(&g,3,s->native.bytes+base,2,s->wire.bytes+base,8,&size,0,2,&result) && result.error==PACKET_GROUP_PAYLOAD && result.payload.error==PACKET_ARRAY_VERSION && size==0 && result.wire_used==0,"encode_version_policy_and_size_order");CHECK(memcmp(s->native.bytes,s->expected_native.bytes,GROUP_OBJECT)==0 && memcmp(s->wire.bytes,s->expected_wire.bytes,GROUP_OBJECT)==0,"encode_future_version_whole_objects");
    }
    return 0;
}
static int validation_cases(struct group_context *ctx,struct group_storage *s)
{
    struct packet_group_plan base=group_plan(s),g;struct packet_group_result result;
    for(unsigned n=0;n<13;++n) {
        g=base;size_t bound=3;enum packet_group_error error=PACKET_GROUP_SCHEMA;ctx->name="group_schema_bounds_and_limits";++ctx->cases;
        switch(n){case 0:g.type_count=0;break;case 1:g.type_count=-1;break;case 2:g.type_count=129;bound=129;break;case 3:bound=2;break;case 4:g.class_count=0;break;case 5:g.class_count=-1;break;case 6:g.maximum_decoded_size=-1;break;case 7:g.maximum_decoded_size=32768;break;case 8:g.maximum_encoded_size=0;break;case 9:g.maximum_encoded_size=32768;break;case 10:g.maximum_decoded_size=1;break;case 11:g.maximum_encoded_size=2;break;default:g.entries=NULL;error=PACKET_GROUP_ARGUMENT;break;}
        struct packet_group_plan before;memcpy(&before,&g,sizeof(g));CHECK(!packet_group_validate(&g,bound,&result) && result.error==error,"validation_limit_reason");CHECK(memcmp(&before,&g,sizeof(g))==0,"validation_group_metadata_unchanged");
    }
    ctx->name="null_group_argument";++ctx->cases;CHECK(!packet_group_validate(NULL,3,&result) && result.error==PACKET_GROUP_ARGUMENT,"null_group_reason");
    for(unsigned n=0;n<3;++n){g=base;short saved=s->entries[n].packet_class;s->entries[n].packet_class=n==0?-1:2;ctx->name="all_entry_classes_including_null_validated";++ctx->cases;boolean accepted=packet_group_validate(&g,3,&result);s->entries[n].packet_class=saved;CHECK(!accepted && result.error==PACKET_GROUP_SCHEMA,"invalid_entry_class_including_null_definition");}
    for(unsigned n=0;n<4;++n){g=base;struct packet_array_plan bad=s->payload;const struct packet_array_plan *saved=s->entries[0].plan;
        switch(n){case 0:bad.nodes=NULL;break;case 1:bad.count=0;break;case 2:bad.depth=0;break;default:bad.version=256;break;}
        s->entries[0].plan=&bad;ctx->name="invalid_entry_plan_control";++ctx->cases;boolean accepted=packet_group_validate(&g,3,&result);s->entries[0].plan=saved;CHECK(!accepted && result.error==PACKET_GROUP_SCHEMA,"invalid_entry_plan_rejected");}
    const struct packet_group_plan shapes[]={{35,8,4352,4352,s->entries},{2,1,96,128,s->entries},{128,32767,32767,32767,s->entries}};
    for(unsigned n=0;n<3;++n){g=shapes[n];short saved1=s->entries[1].packet_class,saved2=s->entries[2].packet_class;s->entries[1].packet_class=0;s->entries[2].packet_class=0;ctx->name=n==0?"synthetic_real_network_group35_8_4352":n==1?"synthetic_real_key_group2_1_96_128":"maximum_portable_type_and_class_limits";++ctx->cases;boolean accepted=packet_group_validate(&g,(size_t)g.type_count,&result);s->entries[1].packet_class=saved1;s->entries[2].packet_class=saved2;CHECK(accepted && result.error==PACKET_GROUP_OK,"real_shape_or_representation_limit_valid");
        if(fprintf(ctx->report,"GROUP VALIDATE name=%s types=%d classes=%d decoded=%ld encoded=%ld payload=authored_synthetic\n",ctx->name,g.type_count,g.class_count,g.maximum_decoded_size,g.maximum_encoded_size)<0 || fflush(ctx->report)!=0)return 1;
    }
    return 0;
}
static int alias_cases(struct group_context *ctx,struct group_storage *s)
{
    struct packet_group_plan g=group_plan(s);
    for(unsigned offset=0;offset<8;++offset) {
        unsigned base=GROUP_GUARD+offset;
        for(unsigned direction=0;direction<3;++direction){unsigned native=base+(direction==2?1:0),wire=base+(direction==1?1:0);objects(s,base,NULL,0);s->native.bytes[wire+3]=0;memcpy(s->expected_native.bytes,s->native.bytes,GROUP_OBJECT);short size=4,type=-111,version=-112;struct packet_group_result result;ctx->name="overlap_precedes_trailer_removal";++ctx->cases;
            CHECK(!packet_group_decode(&g,3,s->native.bytes+native,2,s->native.bytes+wire,4,&size,&type,&version,0,&result) && result.error==PACKET_GROUP_OVERLAP && size==4 && type==-111 && version==-112,"decode_overlap_preflight_order");CHECK(memcmp(s->native.bytes,s->expected_native.bytes,GROUP_OBJECT)==0,"decode_overlap_single_object_unchanged");
            size=-113;CHECK(!packet_group_encode(&g,3,s->native.bytes+native,2,s->native.bytes+wire,4,&size,0,1,&result) && result.error==PACKET_GROUP_OVERLAP && size==-113,"encode_overlap_preflight_order");CHECK(memcmp(s->native.bytes,s->expected_native.bytes,GROUP_OBJECT)==0,"encode_overlap_single_object_unchanged");
        }
        objects(s,base,NULL,0);s->native.bytes[base+3]=0;memcpy(s->expected_native.bytes,s->native.bytes,GROUP_OBJECT);short trailer_size=4,trailer_type=-111,trailer_version=-112;struct packet_group_result trailer_result;ctx->name="native_overlaps_only_group_trailer";++ctx->cases;
        CHECK(!packet_group_decode(&g,3,s->native.bytes+base+3,2,s->native.bytes+base,4,&trailer_size,&trailer_type,&trailer_version,0,&trailer_result) && trailer_result.error==PACKET_GROUP_OVERLAP && trailer_result.wire_used==0 && trailer_result.payload_length==0 && trailer_size==4 && trailer_type==-111 && trailer_version==-112,"group_trailer_overlap_rejected_before_size_or_outputs_change");
        CHECK(memcmp(s->native.bytes,s->expected_native.bytes,GROUP_OBJECT)==0,"group_trailer_only_overlap_full_single_object_unchanged");
        objects(s,base,NULL,0);s->native.bytes[base+4]=0x21;s->native.bytes[base+5]=0x32;memcpy(s->expected_native.bytes,s->native.bytes,GROUP_OBJECT);const byte golden[]={1,0x21,0x32,0};memcpy(s->expected_native.bytes+base,golden,4);short size=-113;struct packet_group_result result;ctx->name="touching_disjoint_encode_objects";++ctx->cases;
        CHECK(packet_group_encode(&g,3,s->native.bytes+base+4,2,s->native.bytes+base,4,&size,0,1,&result) && size==4 && result.error==PACKET_GROUP_OK,"encode_disjoint_touching_boundary");CHECK(memcmp(s->native.bytes,s->expected_native.bytes,GROUP_OBJECT)==0,"encode_disjoint_full_single_object");
        objects(s,base,NULL,0);memcpy(s->native.bytes+base,golden,4);memcpy(s->expected_native.bytes,s->native.bytes,GROUP_OBJECT);s->expected_native.bytes[base+4]=0x21;s->expected_native.bytes[base+5]=0x32;size=4;short type=-111,version=-112;ctx->name="touching_disjoint_decode_objects";++ctx->cases;
        CHECK(packet_group_decode(&g,3,s->native.bytes+base+4,2,s->native.bytes+base,4,&size,&type,&version,0,&result) && size==3 && type==0 && version==1 && result.error==PACKET_GROUP_OK,"decode_disjoint_touching_boundary");CHECK(memcmp(s->native.bytes,s->expected_native.bytes,GROUP_OBJECT)==0,"decode_disjoint_full_single_object");
    }
    return 0;
}
static int array_composition(struct group_context *ctx,struct group_storage *s)
{
    struct packet_group_plan g=group_plan(s);const byte golden[]={1,0,2};
    for(unsigned offset=0;offset<8;++offset) {
        unsigned base=GROUP_GUARD+offset;short zero=0;struct packet_group_result result;short size=-113,type=-111,version=-112;ctx->name="selected_array_full_reserve_count_zero";++ctx->cases;objects(s,base,NULL,0);memcpy(s->native.bytes+base,&zero,2);memcpy(s->expected_native.bytes,s->native.bytes,GROUP_OBJECT);memcpy(s->expected_wire.bytes+base,golden,3);
        CHECK(packet_group_encode(&g,3,s->native.bytes+base,4,s->wire.bytes+base,8,&size,2,1,&result) && result.error==PACKET_GROUP_OK && size==3 && result.payload_length==2 && result.wire_used==3,"array_group_encode_selected_plan_and_trailer");CHECK(memcmp(s->native.bytes,s->expected_native.bytes,GROUP_OBJECT)==0 && memcmp(s->wire.bytes,s->expected_wire.bytes,GROUP_OBJECT)==0,"array_group_encode_full_objects");
        objects(s,base,golden,3);memcpy(s->expected_native.bytes+base,&zero,2);size=3;
        CHECK(packet_group_decode(&g,3,s->native.bytes+base,4,s->wire.bytes+base,8,&size,&type,&version,1,&result) && result.error==PACKET_GROUP_OK && size==2 && type==2 && version==1 && result.supplied_length==3 && result.payload_length==2 && result.wire_used==2,"array_group_decode_count_zero_and_selected_capacity");CHECK(memcmp(s->native.bytes,s->expected_native.bytes,GROUP_OBJECT)==0 && memcmp(s->wire.bytes,s->expected_wire.bytes,GROUP_OBJECT)==0,"array_group_decode_unused_reserve_unchanged");
        ctx->name="selected_array_native_capacity_after_trailer";++ctx->cases;objects(s,base,golden,3);size=3;type=-111;version=-112;
        CHECK(!packet_group_decode(&g,3,s->native.bytes+base,3,s->wire.bytes+base,8,&size,&type,&version,1,&result) && result.error==PACKET_GROUP_PAYLOAD && result.payload.error==PACKET_ARRAY_CAPACITY && size==2 && type==-111 && version==-112 && result.wire_used==0,"array_capacity_failure_after_trailer_removal");CHECK(memcmp(s->native.bytes,s->expected_native.bytes,GROUP_OBJECT)==0 && memcmp(s->wire.bytes,s->expected_wire.bytes,GROUP_OBJECT)==0,"array_capacity_full_objects_unchanged");
        ctx->name="selected_array_invalid_native_count";++ctx->cases;objects(s,base,NULL,0);short three=3;memcpy(s->native.bytes+base,&three,2);memcpy(s->expected_native.bytes,s->native.bytes,GROUP_OBJECT);s->expected_wire.bytes[base]=1;size=-113;
        CHECK(!packet_group_encode(&g,3,s->native.bytes+base,4,s->wire.bytes+base,8,&size,2,1,&result) && result.error==PACKET_GROUP_PAYLOAD && result.payload.error==PACKET_ARRAY_COUNT && size==1 && result.payload_length==1 && result.wire_used==1,"array_native_count_failure_retains_version_prefix");CHECK(memcmp(s->native.bytes,s->expected_native.bytes,GROUP_OBJECT)==0 && memcmp(s->wire.bytes,s->expected_wire.bytes,GROUP_OBJECT)==0,"array_native_count_full_objects");
        ctx->name="selected_array_invalid_wire_count";++ctx->cases;const byte invalid[]={1,3,2};objects(s,base,invalid,3);size=3;type=-111;version=-112;
        CHECK(!packet_group_decode(&g,3,s->native.bytes+base,4,s->wire.bytes+base,8,&size,&type,&version,1,&result) && result.error==PACKET_GROUP_PAYLOAD && result.payload.error==PACKET_ARRAY_COUNT && size==2 && type==-111 && version==1 && result.payload_length==2 && result.wire_used==2,"array_wire_count_failure_after_trailer_prefix");CHECK(memcmp(s->native.bytes,s->expected_native.bytes,GROUP_OBJECT)==0 && memcmp(s->wire.bytes,s->expected_wire.bytes,GROUP_OBJECT)==0,"array_wire_count_does_not_store_invalid_native_count");
    }
    return 0;
}
static struct data_packet_group_definition original_group(struct group_storage *s)
{
    return (struct data_packet_group_definition){"authored_original_group",3,2,2,8,s->original_entries};
}
static int consume_original_error(struct group_context *ctx,const char *expected)
{
    const char *pending=packet_group_reference_peek_error();
    CHECK(pending && strcmp(pending,expected)==0,"original_shared_error_pending");
    if(!pending){if(fprintf(ctx->report,"GROUP ABORT case=%s missing_original_error_before_getter\n",ctx->name)<0 || fflush(ctx->report)!=0)return 1;return 1;}
    CHECK(strcmp(data_packet_groups_get_error(),expected)==0,"original_shared_error_consumed_once");
    CHECK(packet_group_reference_peek_error()==NULL,"original_getter_clears_shared_error");
    return 0;
}
static int original_decode_case(struct group_context *ctx,struct group_storage *s,struct data_packet_group_definition *g,const char *name,const byte *wire,short length,short expected_class,const char *error,short size_after,short type_after,short version_after,int writes_payload)
{
    ctx->name=name;++ctx->cases;objects(s,0,wire,(size_t)length);if(writes_payload){s->expected_native.bytes[0]=0x21;s->expected_native.bytes[1]=0x32;}
    short size=length,type=-111,version=-112;boolean accepted=data_packet_group_decode_packet(g,s->native.bytes,s->wire.bytes,&size,&type,&version,expected_class);
    CHECK(accepted==(error==NULL),"original_safe_decode_acceptance");CHECK(size==size_after && type==type_after && version==version_after,"original_safe_decode_output_order");CHECK(memcmp(s->native.bytes,s->expected_native.bytes,GROUP_OBJECT)==0 && memcmp(s->wire.bytes,s->expected_wire.bytes,GROUP_OBJECT)==0,"original_safe_decode_full_objects");
    if(error && consume_original_error(ctx,error))return 1;
    if(fprintf(ctx->report,"GROUP ORIGINAL CASE name=%s accepted=%u supplied=%d after=%d type=%d version=%d error=%s getter_calls=%u\n",name,(unsigned)accepted,length,size,type,version,error?error:"none",error?1:0)<0 || fflush(ctx->report)!=0)return 1;
    return 0;
}
static int original_cases(struct group_context *ctx,struct group_storage *s)
{
    struct data_packet_group_definition g=original_group(s);ctx->name="original_initialize_cached_metadata";++ctx->cases;CHECK(!s->definition.initialized,"original_definition_begins_uncached");data_packet_group_initialize(&g);CHECK(s->definition.initialized && s->fields[0].size==2,"original_initialization_verifies_safe_definition");
    struct data_packet_definition saved;struct data_packet_field fields_saved[2];memcpy(&saved,&s->definition,sizeof(saved));memcpy(fields_saved,s->fields,sizeof(fields_saved));data_packet_group_initialize(&g);CHECK(memcmp(&saved,&s->definition,sizeof(saved))==0 && memcmp(fields_saved,s->fields,sizeof(fields_saved))==0,"original_second_initialize_cache_skip");
    objects(s,0,NULL,0);s->native.bytes[0]=0x21;s->native.bytes[1]=0x32;memcpy(s->expected_native.bytes,s->native.bytes,GROUP_OBJECT);const byte golden[]={1,0x21,0x32,0};memcpy(s->expected_wire.bytes,golden,sizeof(golden));short size=-113;
    CHECK(data_packet_group_encode_packet(&g,s->native.bytes,s->wire.bytes,&size,0,1) && size==4,"original_safe_encode");CHECK(memcmp(s->native.bytes,s->expected_native.bytes,GROUP_OBJECT)==0 && memcmp(s->wire.bytes,s->expected_wire.bytes,GROUP_OBJECT)==0,"original_independent_wire_golden_and_input");
    const byte trailing[]={1,0x21,0x32,0xe7,0xe8,0},null_wire[]={0xe7,0xe8,1},bad_type[]={3},bad_class[]={1},empty_payload[]={0},future[]={2,0x21,0x32,0};
    if(original_decode_case(ctx,s,&g,"original_exact_trailer",golden,4,0,NULL,3,0,1,1) ||original_decode_case(ctx,s,&g,"original_trailing_bytes_before_trailer",trailing,6,0,NULL,5,0,1,1) ||original_decode_case(ctx,s,&g,"original_null_definition",null_wire,3,1,NULL,2,1,-112,0) ||original_decode_case(ctx,s,&g,"original_invalid_type",bad_type,1,0,"got packet with bad type",1,-111,-112,0) ||original_decode_case(ctx,s,&g,"original_class_mismatch",bad_class,1,0,"got packet with mismatched class",1,-111,-112,0) ||original_decode_case(ctx,s,&g,"original_no_header",golden,0,0,"got packet with no header",0,-111,-112,0) ||original_decode_case(ctx,s,&g,"original_payload_empty",empty_payload,1,0,"got packet which wouldn't decode",0,-111,0,0) ||original_decode_case(ctx,s,&g,"original_future_version",future,4,0,"got packet which wouldn't decode",3,-111,2,0))return 1;
    ctx->name="original_append_equality_and_slack";++ctx->cases;objects(s,0,NULL,0);size=7;CHECK(!data_packet_group_append_packet_header(&g,s->wire.bytes,&size,0) && size==7,"original_strict_append_equality_failure");if(consume_original_error(ctx,"couldn't append header to encoded packet"))return 1;CHECK(memcmp(s->wire.bytes,s->expected_wire.bytes,GROUP_OBJECT)==0,"original_failed_append_whole_object");size=6;s->expected_wire.bytes[6]=2;CHECK(data_packet_group_append_packet_header(&g,s->wire.bytes,&size,2) && size==7,"original_append_slack_success");CHECK(memcmp(s->wire.bytes,s->expected_wire.bytes,GROUP_OBJECT)==0,"original_append_last_byte_golden");
    g.maximum_encoded_packet_size=4;objects(s,0,NULL,0);s->native.bytes[0]=0x21;s->native.bytes[1]=0x32;memcpy(s->expected_native.bytes,s->native.bytes,GROUP_OBJECT);memcpy(s->expected_wire.bytes,golden,3);size=-113;ctx->name="original_encode_completed_payload_append_failure";++ctx->cases;CHECK(!data_packet_group_encode_packet(&g,s->native.bytes,s->wire.bytes,&size,0,1) && size==3,"original_encode_append_failure_retains_payload_size");if(consume_original_error(ctx,"couldn't append header to encoded packet"))return 1;CHECK(memcmp(s->native.bytes,s->expected_native.bytes,GROUP_OBJECT)==0 && memcmp(s->wire.bytes,s->expected_wire.bytes,GROUP_OBJECT)==0,"original_encode_append_failure_full_objects");
    g=original_group(s);objects(s,0,bad_type,1);size=1;short type=-111,version=-112;ctx->name="original_shared_error_latest_failure";++ctx->cases;CHECK(!data_packet_group_decode_packet(&g,s->native.bytes,s->wire.bytes,&size,&type,&version,0),"original_first_error_pending");const char *pending=packet_group_reference_peek_error();CHECK(pending && strcmp(pending,"got packet with bad type")==0,"original_first_pending_error_observed");
    struct data_packet_group_definition other=original_group(s);other.name="authored_other_group";data_packet_group_initialize(&other);CHECK(packet_group_reference_peek_error()==pending,"initializing_other_group_preserves_pending_global");
    objects(s,0,bad_class,1);size=1;CHECK(!data_packet_group_decode_packet(&other,s->native.bytes,s->wire.bytes,&size,&type,&version,0),"original_other_group_error_overwrites");pending=packet_group_reference_peek_error();CHECK(pending && strcmp(pending,"got packet with mismatched class")==0,"original_other_group_latest_global_error");if(consume_original_error(ctx,"got packet with mismatched class"))return 1;CHECK(packet_group_reference_peek_error()==NULL,"cross_group_getter_clears_global");
    ctx->name="original_success_clears_pending_global";++ctx->cases;objects(s,0,bad_type,1);size=1;CHECK(!data_packet_group_decode_packet(&g,s->native.bytes,s->wire.bytes,&size,&type,&version,0) && packet_group_reference_peek_error()!=NULL,"pending_before_successful_append");objects(s,0,NULL,0);size=0;CHECK(data_packet_group_append_packet_header(&other,s->wire.bytes,&size,0) && size==1 && packet_group_reference_peek_error()==NULL,"successful_append_clears_global");
    objects(s,0,bad_type,1);size=1;CHECK(!data_packet_group_decode_packet(&g,s->native.bytes,s->wire.bytes,&size,&type,&version,0) && packet_group_reference_peek_error()!=NULL,"pending_before_successful_decode");objects(s,0,golden,4);size=4;CHECK(data_packet_group_decode_packet(&other,s->native.bytes,s->wire.bytes,&size,&type,&version,0) && size==3 && type==0 && version==1 && packet_group_reference_peek_error()==NULL,"successful_decode_clears_global");
    ctx->name="candidate_per_call_error_independent_of_original_global";++ctx->cases;objects(s,0,bad_type,1);size=1;CHECK(!data_packet_group_decode_packet(&g,s->native.bytes,s->wire.bytes,&size,&type,&version,0),"original_pending_before_candidate_calls");
    struct packet_group_plan candidate=group_plan(s);struct packet_group_result per_call;size=1;CHECK(!packet_group_decode(&candidate,3,s->native.bytes,2,s->wire.bytes,8,&size,&type,&version,0,&per_call) && per_call.error==PACKET_GROUP_TYPE,"candidate_first_per_call_error");objects(s,0,golden,4);size=4;CHECK(packet_group_decode(&candidate,3,s->native.bytes,2,s->wire.bytes,8,&size,&type,&version,0,&per_call) && per_call.error==PACKET_GROUP_OK && per_call.payload.error==PACKET_ARRAY_OK,"candidate_next_call_resets_result_error");pending=packet_group_reference_peek_error();CHECK(pending && strcmp(pending,"got packet with bad type")==0,"candidate_calls_preserve_original_pending_global");if(consume_original_error(ctx,"got packet with bad type"))return 1;
    g.packet_type_count=256;for(unsigned n=0;n<GROUP_ENTRIES;++n){s->original_entries[n].packet_class=0;s->original_entries[n].definition=NULL;}
    const byte high80[]={0x80},highff[]={0xff};int unsigned_char=CHAR_MIN==0;
    if(original_decode_case(ctx,s,&g,"original_char_trailer80",high80,1,0,unsigned_char?NULL:"got packet with bad type",unsigned_char?0:1,unsigned_char?128:-111,-112,0) ||original_decode_case(ctx,s,&g,"original_char_trailerff",highff,1,0,unsigned_char?NULL:"got packet with bad type",unsigned_char?0:1,unsigned_char?255:-111,-112,0))return 1;
    if(fprintf(ctx->report,"GROUP ORIGINAL platform_char_min=%d trailer80=%s trailerff=%s empty_error_getter=unexecuted_asserts negative_length=unexecuted_unsafe\n",CHAR_MIN,unsigned_char?"accepted128":"rejected_type",unsigned_char?"accepted255":"rejected_type")<0 || fflush(ctx->report)!=0)return 1;
    return 0;
}
int wii_packet_group_fixture(FILE *report,int collect)
{
    struct group_context ctx={report,"startup",0,0,0,collect};struct group_storage *s=calloc(1,sizeof(*s));
    if(!s){if(fprintf(report,"GROUP ABORT allocation_failed\n")<0 || fflush(report)!=0)return 1;return 1;}
    s->fields[0]=(struct data_packet_field){_data_packet_field_bytes,2,0,0,113};s->fields[1]=(struct data_packet_field){_data_packet_field_end,-7,-8,-9,117};
    memset(&s->definition,0xa5,sizeof(s->definition));s->definition.name="authored_group_payload";s->definition.flags=0x1234;s->definition.size=2;s->definition.version=1;s->definition.fields=s->fields;s->definition.initialized=FALSE;
    struct packet_array_result compile;boolean compiled=packet_array_compile(&s->definition,2,&s->payload,&compile);int aborted=0;
    if(!compiled){if(fprintf(report,"GROUP ABORT payload_compile_error=%s\n",packet_array_error_name(compile.error))<0 || fflush(report)!=0)aborted=1;aborted=1;goto cleanup;}
    s->array_fields[0]=(struct data_packet_field){_data_packet_field_array,2,0,0,113};s->array_fields[1]=(struct data_packet_field){_data_packet_field_bytes,1,0,0,113};s->array_fields[2]=s->fields[1];s->array_fields[3]=s->fields[1];memset(&s->array_definition,0xa5,sizeof(s->array_definition));s->array_definition.name="authored_group_array_payload";s->array_definition.flags=0x1234;s->array_definition.size=4;s->array_definition.version=1;s->array_definition.fields=s->array_fields;s->array_definition.initialized=FALSE;
    compiled=packet_array_compile(&s->array_definition,4,&s->array_payload,&compile);if(!compiled){if(fprintf(report,"GROUP ABORT array_payload_compile_error=%s\n",packet_array_error_name(compile.error))<0 || fflush(report)!=0)aborted=1;aborted=1;goto cleanup;}
    for(unsigned n=0;n<GROUP_ENTRIES;++n){s->entries[n]=(struct packet_group_entry){0,&s->payload};s->original_entries[n]=(struct data_packet_entry){0,0,&s->definition};}
    s->entries[1]=(struct packet_group_entry){1,NULL};s->entries[2]=(struct packet_group_entry){1,&s->array_payload};s->original_entries[1]=(struct data_packet_entry){1,0,NULL};s->original_entries[2].packet_class=1;memcpy(s->entries_before,s->entries,sizeof(s->entries));
    struct data_packet_definition definitions_before[2];struct data_packet_field fields_before[6];struct packet_array_plan plans_before[2];struct packet_array_node nodes_before[6];
    memcpy(&definitions_before[0],&s->definition,sizeof(s->definition));memcpy(&definitions_before[1],&s->array_definition,sizeof(s->array_definition));memcpy(fields_before,s->fields,sizeof(s->fields));memcpy(fields_before+2,s->array_fields,sizeof(s->array_fields));memcpy(&plans_before[0],&s->payload,sizeof(s->payload));memcpy(&plans_before[1],&s->array_payload,sizeof(s->array_payload));memcpy(nodes_before,s->payload.nodes,2*sizeof(*nodes_before));memcpy(nodes_before+2,s->array_payload.nodes,4*sizeof(*nodes_before));
    if(fprintf(report,"GROUP BEGIN policy=bounded_immutable_group portable_types=0-127 trailing_before_trailer=accepted original_groups=source_faithful_safe_cases\n")<0 || fflush(report)!=0){aborted=1;goto cleanup;}
    aborted=candidate_decode_cases(&ctx,s) ||candidate_encode_cases(&ctx,s) ||validation_cases(&ctx,s) ||alias_cases(&ctx,s) ||array_composition(&ctx,s);
    ctx.name="candidate_entry_table_immutable";if(!group_check(&ctx,memcmp(s->entries,s->entries_before,sizeof(s->entries))==0,"candidate_whole_entries_unchanged"))aborted=1;
    if(!group_check(&ctx,memcmp(&s->definition,&definitions_before[0],sizeof(s->definition))==0 && memcmp(&s->array_definition,&definitions_before[1],sizeof(s->array_definition))==0 && memcmp(s->fields,fields_before,sizeof(s->fields))==0 && memcmp(s->array_fields,fields_before+2,sizeof(s->array_fields))==0,"candidate_original_source_definitions_and_cache_unchanged"))aborted=1;
    if(!group_check(&ctx,memcmp(&s->payload,&plans_before[0],sizeof(s->payload))==0 && memcmp(&s->array_payload,&plans_before[1],sizeof(s->array_payload))==0 && memcmp(s->payload.nodes,nodes_before,2*sizeof(*nodes_before))==0 && memcmp(s->array_payload.nodes,nodes_before+2,4*sizeof(*nodes_before))==0,"candidate_borrowed_compiled_plans_and_nodes_unchanged"))aborted=1;
    if(!aborted)aborted=original_cases(&ctx,s);
cleanup:
    packet_array_destroy(&s->payload);packet_array_destroy(&s->array_payload);free(s);
    if(fprintf(report,"GROUP SUMMARY cases=%u checks=%u failures=%u aborted=%u\nGROUP END result=%u\n",ctx.cases,ctx.checks,ctx.failures,aborted!=0,aborted || ctx.failures!=0)<0 || fflush(report)!=0)return 1;
    return aborted || ctx.failures!=0;
}
