#include "packet_caller_policy.h"
#include "packet_caller_fixture.h"
#include "caller_reference.h"
#include "bungie_net/common/message_header.h"
#include <limits.h>

enum { CALLER_OBJECT=33024, CALLER_GUARD=16, CALLER_PLANS=4 };
struct caller_context {FILE *report;const char *name;unsigned cases,checks,failures;int collect;};
union caller_object {uint64_t alignment;byte bytes[CALLER_OBJECT];};
struct caller_storage {
    union caller_object native,workspace,frame,expected_native,expected_workspace,expected_frame;
    struct data_packet_field fields[CALLER_PLANS][4];
    struct data_packet_definition definitions[CALLER_PLANS];
    struct packet_array_plan plans[CALLER_PLANS];
    struct packet_group_entry entries[3],entries_before[3];
    void *reference_heap;
};
static int caller_check(struct caller_context *ctx,int condition,const char *assertion)
{
    ++ctx->checks;if(condition)return 1;++ctx->failures;
    if(fprintf(ctx->report,"CALLER FAIL case=%s assertion=%s check=%u\n",ctx->name,assertion,ctx->checks)<0 || fflush(ctx->report)!=0)return 0;
    return ctx->collect;
}
#define CHECK(c,n) do {if(!caller_check(ctx,(c),(n)))return 1;} while(0)
static struct packet_group_plan caller_group(struct caller_storage *s)
{
    return (struct packet_group_plan){3,2,4352,4352,s->entries};
}
static void caller_objects(struct caller_storage *s,unsigned base,const byte *frame,size_t length)
{
    memset(s->native.bytes,0x5a,CALLER_OBJECT);memset(s->workspace.bytes,0xcc,CALLER_OBJECT);memset(s->frame.bytes,0xdd,CALLER_OBJECT);
    if(length)memcpy(s->frame.bytes+base,frame,length);
    memcpy(s->expected_native.bytes,s->native.bytes,CALLER_OBJECT);memcpy(s->expected_workspace.bytes,s->workspace.bytes,CALLER_OBJECT);memcpy(s->expected_frame.bytes,s->frame.bytes,CALLER_OBJECT);
}
static int caller_whole_objects(struct caller_context *ctx,const struct caller_storage *s)
{
    CHECK(memcmp(s->native.bytes,s->expected_native.bytes,CALLER_OBJECT)==0,"whole_native_canaries_and_partial_payload");
    CHECK(memcmp(s->workspace.bytes,s->expected_workspace.bytes,CALLER_OBJECT)==0,"whole_workspace_canaries_and_retained_group_output");
    CHECK(memcmp(s->frame.bytes,s->expected_frame.bytes,CALLER_OBJECT)==0,"whole_frame_canaries_header_and_payload");
    return 0;
}
static int ordinary_encode(struct caller_context *ctx,struct caller_storage *s)
{
    struct packet_group_plan g=caller_group(s);const byte group_wire[]={1,0x21,0x32,0};
    for(unsigned offset=0;offset<8;++offset) {
        unsigned base=CALLER_GUARD+offset;
        for(unsigned flags=0;flags<4;++flags) {
            for(long workcap=0;workcap<=5;++workcap) {
                ctx->name="ordinary_group_workspace_capacity";++ctx->cases;caller_objects(s,base,NULL,0);s->native.bytes[base]=0x21;s->native.bytes[base+1]=0x32;memcpy(s->expected_native.bytes,s->native.bytes,CALLER_OBJECT);
                unsigned used=workcap==0?0:workcap<3?1:workcap==3?3:4;memcpy(s->expected_workspace.bytes+base,group_wire,used);
                if(workcap>=4){s->expected_frame.bytes[base]=0;s->expected_frame.bytes[base+1]=(byte)(0x6c+flags);memcpy(s->expected_frame.bytes+base+2,group_wire,4);}
                struct packet_caller_result result;boolean accepted=packet_caller_encode(&g,3,s->native.bytes+base,2,s->workspace.bytes+base,workcap,s->frame.bytes+base,6,0,NONE,(byte)flags,&result);
                CHECK(accepted==(workcap>=4) && result.error==(workcap>=4?PACKET_CALLER_OK:PACKET_CALLER_GROUP),"encode_workspace_capacity_and_caller_error");
                CHECK(result.group.wire_used==(long)used && result.group.error==(workcap>=4?PACKET_GROUP_OK:workcap==3?PACKET_GROUP_APPEND:PACKET_GROUP_PAYLOAD),"encode_group_partial_cursor_and_append_failure");
                if(workcap>=4)CHECK(result.frame_size==6,"encode_exact_frame_size");
                if(caller_whole_objects(ctx,s))return 1;
            }
            for(long framecap=0;framecap<=7;++framecap) {
                ctx->name="ordinary_frame_capacity_after_completed_group";++ctx->cases;caller_objects(s,base,NULL,0);s->native.bytes[base]=0x21;s->native.bytes[base+1]=0x32;memcpy(s->expected_native.bytes,s->native.bytes,CALLER_OBJECT);memcpy(s->expected_workspace.bytes+base,group_wire,4);
                if(framecap>=6){s->expected_frame.bytes[base]=0;s->expected_frame.bytes[base+1]=(byte)(0x6c+flags);memcpy(s->expected_frame.bytes+base+2,group_wire,4);}
                struct packet_caller_result result;boolean accepted=packet_caller_encode(&g,3,s->native.bytes+base,2,s->workspace.bytes+base,8,s->frame.bytes+base,framecap,0,1,(byte)flags,&result);
                CHECK(accepted==(framecap>=6) && result.error==(framecap>=6?PACKET_CALLER_OK:PACKET_CALLER_CAPACITY),"encode_frame_capacity_after_group_completion");CHECK(result.group.error==PACKET_GROUP_OK && result.group.wire_used==4 && result.group.payload_length==3,"frame_failure_retains_completed_workspace");if(framecap>=6)CHECK(result.frame_size==6,"encode_flags_and_numeric_frame_length");if(caller_whole_objects(ctx,s))return 1;
            }
        }
    }
    if(fprintf(ctx->report,"CALLER CASE ordinary_encode flags=0-3 offsets=0-7 workspace_capacity=0-5 frame_capacity=0-7 network_golden=006c01213200 key_golden=006e01213200\n")<0 || fflush(ctx->report)!=0)return 1;
    return 0;
}
static int decode_case(struct caller_context *ctx,struct caller_storage *s,const char *name,const byte *frame,size_t bytes,long length,long capacity,long native_capacity,short expected_type,short expected_class,byte flags,
                       enum packet_caller_error error,enum packet_group_error group_error,enum packet_array_error payload_error,short version_after,long group_supplied,long payload_length,long used,int writes)
{
    struct packet_group_plan g=caller_group(s);ctx->name=name;++ctx->cases;
    for(unsigned offset=0;offset<8;++offset) {
        unsigned base=CALLER_GUARD+offset;caller_objects(s,base,frame,bytes);
        if(writes==1){s->expected_native.bytes[base]=0x21;s->expected_native.bytes[base+1]=0x32;}else if(writes==2){short zero=0;memcpy(s->expected_native.bytes+base,&zero,2);}
        short version=-112;struct packet_caller_result result;boolean accepted=packet_caller_decode(&g,3,s->frame.bytes+base,length,capacity,s->native.bytes+base,native_capacity,expected_type,expected_class,flags,&version,&result);
        CHECK(accepted==(error==PACKET_CALLER_OK) && result.error==error,"decode_caller_acceptance_and_error_order");CHECK(version==version_after,"decode_version_preserved_or_partial_payload_written");
        CHECK(result.group.error==group_error && result.group.payload.error==payload_error && result.group.supplied_length==group_supplied && result.group.payload_length==payload_length && result.group.wire_used==used,"decode_selected_group_reason_supplied_and_consumed");
        if(error==PACKET_CALLER_OK || error==PACKET_CALLER_GROUP) CHECK(result.frame_size==length,"decode_outer_frame_size_separate_from_consumed_payload");
        if(caller_whole_objects(ctx,s))return 1;
    }
    if(fprintf(ctx->report,"CALLER CASE name=%s operation=decode supplied=%ld group_supplied=%ld payload=%ld consumed=%ld error=%s offsets=0-7\n",name,length,group_supplied,payload_length,used,packet_caller_error_name(error))<0 || fflush(ctx->report)!=0)return 1;
    return 0;
}
static int ordinary_decode(struct caller_context *ctx,struct caller_storage *s)
{
    const byte flat[]={0,0x6c,1,0x21,0x32,0},key[]={0,0x6e,1,0x21,0x32,0},array[]={0,0x5c,1,0,2},invalid_count[]={0,0x5c,1,3,2},future[]={0,0x6c,2,0x21,0x32,0},trailing[]={0,0x8c,1,0x21,0x32,0xe7,0xe8,0},null_frame[]={0,0x3c,1};
    if(decode_case(ctx,s,"network_flat_golden",flat,6,6,8,2,0,0,0,PACKET_CALLER_OK,PACKET_GROUP_OK,PACKET_ARRAY_OK,1,4,3,3,1) ||
       decode_case(ctx,s,"key_flags_two_golden",key,6,6,8,2,0,0,2,PACKET_CALLER_OK,PACKET_GROUP_OK,PACKET_ARRAY_OK,1,4,3,3,1) ||
       decode_case(ctx,s,"included_array_identity_count_zero",array,5,5,8,4,2,1,0,PACKET_CALLER_OK,PACKET_GROUP_OK,PACKET_ARRAY_OK,1,3,2,2,2) ||
       decode_case(ctx,s,"null_definition_type_only",null_frame,3,3,8,0,1,1,0,PACKET_CALLER_OK,PACKET_GROUP_OK,PACKET_ARRAY_OK,-112,1,0,0,0) ||
       decode_case(ctx,s,"inner_trailing_before_trailer_accepted",trailing,8,8,10,2,0,0,0,PACKET_CALLER_OK,PACKET_GROUP_OK,PACKET_ARRAY_OK,1,6,5,3,1) ||
       decode_case(ctx,s,"future_payload_version",future,6,6,8,2,0,0,0,PACKET_CALLER_GROUP,PACKET_GROUP_PAYLOAD,PACKET_ARRAY_VERSION,2,4,3,1,0) ||
       decode_case(ctx,s,"invalid_array_count",invalid_count,5,5,8,4,2,1,0,PACKET_CALLER_GROUP,PACKET_GROUP_PAYLOAD,PACKET_ARRAY_COUNT,1,3,2,2,0) ||
       decode_case(ctx,s,"selected_array_native_capacity_after_dispatch",array,5,5,8,3,2,1,0,PACKET_CALLER_GROUP,PACKET_GROUP_PAYLOAD,PACKET_ARRAY_CAPACITY,-112,3,2,0,0) ||
       decode_case(ctx,s,"selected_flat_native_capacity_after_dispatch",flat,6,6,8,1,0,0,0,PACKET_CALLER_GROUP,PACKET_GROUP_PAYLOAD,PACKET_ARRAY_CAPACITY,-112,4,3,0,0) ||
       decode_case(ctx,s,"identity_mismatch_precedes_native_array_capacity",array,5,5,8,2,0,0,0,PACKET_CALLER_IDENTITY,PACKET_GROUP_OK,PACKET_ARRAY_OK,-112,0,0,0,0) ||
       decode_case(ctx,s,"matching_identity_wrong_class",array,5,5,8,4,2,0,0,PACKET_CALLER_GROUP,PACKET_GROUP_CLASS,PACKET_ARRAY_OK,-112,3,0,0,0) ||
       decode_case(ctx,s,"invalid_expected_class",flat,6,6,8,2,0,2,0,PACKET_CALLER_GROUP,PACKET_GROUP_CLASS,PACKET_ARRAY_OK,-112,4,0,0,0))return 1;
    short saved_class=s->entries[2].packet_class;s->entries[2].packet_class=0;
    int identity_failed=decode_case(ctx,s,"same_class_identity_mismatch_precedes_native_capacity",array,5,5,8,2,0,0,0,PACKET_CALLER_IDENTITY,PACKET_GROUP_OK,PACKET_ARRAY_OK,-112,0,0,0,0);
    s->entries[2].packet_class=saved_class;if(identity_failed)return 1;
    for(unsigned n=0;n<3;++n){byte partial[5]={0,(byte)(((n+3)<<4)|12),1,0x21,0};partial[n+2]=0;char name[64];snprintf(name,sizeof(name),"valid_outer_truncated_inner_%u",n);
        if(decode_case(ctx,s,name,partial,n+3,n+3,8,2,0,0,0,PACKET_CALLER_GROUP,PACKET_GROUP_PAYLOAD,PACKET_ARRAY_WIRE,n?1:-112,n+1,n,n?1:0,0))return 1;
    }
    const byte wrong_length[]={0,0x5c,1,0x21,0x32,0},swapped[]={0x6c,0,1,0x21,0x32,0},wrong_type[]={0,0x68,1,0x21,0x32,0},wrong_flags[]={0,0x6e,1,0x21,0x32,0},after_frame[]={0,0x6c,1,0x21,0x32,0,0xe7},bad_trailer[]={0,0x3c,3};
    if(decode_case(ctx,s,"header_length_mismatch",wrong_length,6,6,8,2,0,0,0,PACKET_CALLER_LENGTH,PACKET_GROUP_OK,PACKET_ARRAY_OK,-112,0,0,0,0) ||
       decode_case(ctx,s,"double_swapped_header_rejected",swapped,6,6,8,2,0,0,0,PACKET_CALLER_LENGTH,PACKET_GROUP_OK,PACKET_ARRAY_OK,-112,0,0,0,0) ||
       decode_case(ctx,s,"outer_transport_type_mismatch",wrong_type,6,6,8,2,0,0,0,PACKET_CALLER_TYPE,PACKET_GROUP_OK,PACKET_ARRAY_OK,-112,0,0,0,0) ||
       decode_case(ctx,s,"outer_flags_mismatch",wrong_flags,6,6,8,2,0,0,0,PACKET_CALLER_FLAGS,PACKET_GROUP_OK,PACKET_ARRAY_OK,-112,0,0,0,0) ||
       decode_case(ctx,s,"extra_bytes_after_outer_frame_rejected",after_frame,7,7,8,2,0,0,0,PACKET_CALLER_LENGTH,PACKET_GROUP_OK,PACKET_ARRAY_OK,-112,0,0,0,0) ||
       decode_case(ctx,s,"invalid_actual_trailer_type",bad_trailer,3,3,8,2,0,0,0,PACKET_CALLER_TYPE,PACKET_GROUP_OK,PACKET_ARRAY_OK,-112,0,0,0,0) ||
       decode_case(ctx,s,"outer_below_minimum",flat,6,2,8,2,0,0,0,PACKET_CALLER_LENGTH,PACKET_GROUP_OK,PACKET_ARRAY_OK,-112,0,0,0,0) ||
       decode_case(ctx,s,"outer_zero_length",flat,6,0,8,2,0,0,0,PACKET_CALLER_LENGTH,PACKET_GROUP_OK,PACKET_ARRAY_OK,-112,0,0,0,0) ||
       decode_case(ctx,s,"outer_negative_length",flat,6,-1,8,2,0,0,0,PACKET_CALLER_LENGTH,PACKET_GROUP_OK,PACKET_ARRAY_OK,-112,0,0,0,0) ||
       decode_case(ctx,s,"outer_above4095",flat,6,4096,4096,2,0,0,0,PACKET_CALLER_LENGTH,PACKET_GROUP_OK,PACKET_ARRAY_OK,-112,0,0,0,0) ||
       decode_case(ctx,s,"outer_exceeds_actual_capacity",flat,6,6,5,2,0,0,0,PACKET_CALLER_CAPACITY,PACKET_GROUP_OK,PACKET_ARRAY_OK,-112,0,0,0,0) ||
       decode_case(ctx,s,"negative_actual_frame_capacity",flat,6,6,-1,2,0,0,0,PACKET_CALLER_CAPACITY,PACKET_GROUP_OK,PACKET_ARRAY_OK,-112,0,0,0,0) ||
       decode_case(ctx,s,"negative_native_capacity",flat,6,6,8,-1,0,0,0,PACKET_CALLER_CAPACITY,PACKET_GROUP_OK,PACKET_ARRAY_OK,-112,0,0,0,0) ||
       decode_case(ctx,s,"invalid_expected_type",flat,6,6,8,2,3,0,0,PACKET_CALLER_TYPE,PACKET_GROUP_OK,PACKET_ARRAY_OK,-112,0,0,0,0) ||
       decode_case(ctx,s,"invalid_expected_flags",flat,6,6,8,2,0,0,4,PACKET_CALLER_FLAGS,PACKET_GROUP_OK,PACKET_ARRAY_OK,-112,0,0,0,0))return 1;
    return 0;
}
static int preflight_cases(struct caller_context *ctx,struct caller_storage *s)
{
    struct packet_group_plan g=caller_group(s);
    for(unsigned offset=0;offset<8;++offset) {
        unsigned base=CALLER_GUARD+offset;
        for(unsigned n=0;n<11;++n) {
            caller_objects(s,base,NULL,0);long nativecap=2,workcap=8,framecap=8;short type=0;byte flags=0;size_t bound=3;const void *native=s->native.bytes+base;void *workspace=s->workspace.bytes+base,*frame=s->frame.bytes+base;
            enum packet_caller_error error=PACKET_CALLER_CAPACITY;enum packet_group_error group_error=PACKET_GROUP_OK;
            switch(n){case 0:nativecap=-1;break;case 1:workcap=-1;break;case 2:framecap=-1;break;case 3:workcap=32768;error=PACKET_CALLER_LENGTH;break;case 4:flags=4;error=PACKET_CALLER_FLAGS;break;case 5:type=-1;error=PACKET_CALLER_TYPE;break;case 6:type=3;error=PACKET_CALLER_TYPE;break;case 7:type=1;error=PACKET_CALLER_GROUP;group_error=PACKET_GROUP_NO_DEFINITION;break;case 8:bound=2;error=PACKET_CALLER_GROUP;group_error=PACKET_GROUP_SCHEMA;break;case 9:native=NULL;error=PACKET_CALLER_ARGUMENT;break;default:workspace=NULL;error=PACKET_CALLER_ARGUMENT;break;}
            ctx->name="encode_preflight_no_group_or_frame_io";++ctx->cases;struct packet_caller_result result;
            CHECK(!packet_caller_encode(&g,bound,native,nativecap,workspace,workcap,frame,framecap,type,1,flags,&result) && result.error==error && result.group.error==group_error && result.group.wire_used==0,"encode_preflight_argument_length_capacity_flags_type_or_definition");if(caller_whole_objects(ctx,s))return 1;
        }
        const byte valid[]={0,0x6c,1,0x21,0x32,0};caller_objects(s,base,valid,6);ctx->name="decode_group_bound_preflight";++ctx->cases;short version=-112;struct packet_caller_result result;
        CHECK(!packet_caller_decode(&g,2,s->frame.bytes+base,6,8,s->native.bytes+base,2,0,0,0,&version,&result) && result.error==PACKET_CALLER_GROUP && result.group.error==PACKET_GROUP_SCHEMA && version==-112,"decode_group_validation_before_header_or_payload");if(caller_whole_objects(ctx,s))return 1;
        caller_objects(s,base,NULL,0);short three=3;memcpy(s->native.bytes+base,&three,2);memcpy(s->expected_native.bytes,s->native.bytes,CALLER_OBJECT);s->expected_workspace.bytes[base]=1;ctx->name="encode_array_native_count_failure_retains_workspace_prefix";++ctx->cases;
        CHECK(!packet_caller_encode(&g,3,s->native.bytes+base,4,s->workspace.bytes+base,8,s->frame.bytes+base,8,2,1,0,&result) && result.error==PACKET_CALLER_GROUP && result.group.error==PACKET_GROUP_PAYLOAD && result.group.payload.error==PACKET_ARRAY_COUNT && result.group.wire_used==1,"encode_nested_array_count_reason_and_offset");if(caller_whole_objects(ctx,s))return 1;
        caller_objects(s,base,NULL,0);ctx->name="encode_future_version_has_no_workspace_or_frame_io";++ctx->cases;
        CHECK(!packet_caller_encode(&g,3,s->native.bytes+base,2,s->workspace.bytes+base,8,s->frame.bytes+base,8,0,2,0,&result) && result.error==PACKET_CALLER_GROUP && result.group.error==PACKET_GROUP_PAYLOAD && result.group.payload.error==PACKET_ARRAY_VERSION && result.group.wire_used==0,"encode_future_version_nested_policy");if(caller_whole_objects(ctx,s))return 1;
    }
    return 0;
}
static int alias_cases(struct caller_context *ctx,struct caller_storage *s)
{
    struct packet_group_plan g=caller_group(s);
    for(unsigned offset=0;offset<8;++offset) {
        unsigned base=CALLER_GUARD+offset;
        for(unsigned pair=0;pair<4;++pair) {
            caller_objects(s,base,NULL,0);const void *native=s->native.bytes+base;void *workspace=s->workspace.bytes+base,*frame=s->frame.bytes+base;
            if(pair==0)workspace=s->native.bytes+base+1;else if(pair==1)frame=s->native.bytes+base+1;else if(pair==2)frame=s->workspace.bytes+base+1;else {workspace=s->native.bytes+base;frame=s->native.bytes+base;}
            ctx->name="encode_native_workspace_frame_disjoint_ranges";++ctx->cases;struct packet_caller_result result;
            CHECK(!packet_caller_encode(&g,3,native,2,workspace,8,frame,8,0,1,0,&result) && result.error==PACKET_CALLER_OVERLAP && result.group.wire_used==0,"encode_each_pair_overlap_before_group_io");if(caller_whole_objects(ctx,s))return 1;
        }
        const byte valid[]={0,0x6c,1,0x21,0x32,0};
        for(unsigned location=0;location<2;++location) {
            caller_objects(s,base,valid,6);void *native=s->frame.bytes+base+(location?5:0);short version=-112;ctx->name=location?"decode_native_overlaps_only_outer_trailer":"decode_native_overlaps_only_outer_header";++ctx->cases;struct packet_caller_result result;
            CHECK(!packet_caller_decode(&g,3,s->frame.bytes+base,6,8,native,2,0,0,0,&version,&result) && result.error==PACKET_CALLER_OVERLAP && result.group.wire_used==0 && version==-112,"outer_frame_overlap_precedes_inner_group_io");if(caller_whole_objects(ctx,s))return 1;
        }
        caller_objects(s,base,valid,6);memcpy(s->expected_frame.bytes+base+6,valid+3,2);short version=-112;ctx->name="decode_touching_outer_frame_native_objects";++ctx->cases;struct packet_caller_result result;
        CHECK(packet_caller_decode(&g,3,s->frame.bytes+base,6,6,s->frame.bytes+base+6,2,0,0,0,&version,&result) && result.error==PACKET_CALLER_OK && version==1,"decode_outer_boundary_touching_ranges_allowed");if(caller_whole_objects(ctx,s))return 1;
    }
    return 0;
}
static int maximum_frame_cases(struct caller_context *ctx,struct caller_storage *s)
{
    for(unsigned one_over=0;one_over<2;++one_over) {
        struct packet_group_entry entry={0,&s->plans[2+one_over]};struct packet_group_plan g={1,1,4352,4352,&entry};unsigned native_size=4091+one_over,group_size=native_size+2;
        for(unsigned offset=0;offset<8;++offset) {
            unsigned base=CALLER_GUARD+offset;ctx->name=one_over?"completed_group_outer_one_over4095":"exact_outer4095_numeric_boundary";++ctx->cases;
            for(unsigned cap_choice=0;cap_choice<(one_over?1u:2u);++cap_choice) {
                long frame_capacity=cap_choice?4094:4096;caller_objects(s,base,NULL,0);for(unsigned n=0;n<native_size;++n)s->native.bytes[base+n]=(byte)(0x31+n);memcpy(s->expected_native.bytes,s->native.bytes,CALLER_OBJECT);s->expected_workspace.bytes[base]=1;memcpy(s->expected_workspace.bytes+base+1,s->native.bytes+base,native_size);s->expected_workspace.bytes[base+native_size+1]=0;
                if(!one_over && !cap_choice){s->expected_frame.bytes[base]=0xff;s->expected_frame.bytes[base+1]=0xfc;memcpy(s->expected_frame.bytes+base+2,s->expected_workspace.bytes+base,group_size);}
                struct packet_caller_result result;boolean accepted=packet_caller_encode(&g,1,s->native.bytes+base,native_size,s->workspace.bytes+base,4352,s->frame.bytes+base,frame_capacity,0,1,0,&result);
                enum packet_caller_error error=one_over?PACKET_CALLER_LENGTH:cap_choice?PACKET_CALLER_CAPACITY:PACKET_CALLER_OK;
                CHECK(accepted==(error==PACKET_CALLER_OK) && result.error==error,"outer_maximum_length_capacity_after_group_completion");CHECK(result.group.error==PACKET_GROUP_OK && result.group.wire_used==(long)group_size && result.group.payload_length==(long)native_size+1,"outer_maximum_preserves_checked_numeric_group_size");if(!one_over)CHECK(result.frame_size==4095,"outer_exact_maximum_frame_size");if(caller_whole_objects(ctx,s))return 1;
            }
            if(!one_over) {
                caller_objects(s,base,NULL,0);s->frame.bytes[base]=0xff;s->frame.bytes[base+1]=0xfc;s->frame.bytes[base+2]=1;for(unsigned n=0;n<native_size;++n){byte value=(byte)(0x31+n);s->frame.bytes[base+3+n]=value;s->expected_native.bytes[base+n]=value;}s->frame.bytes[base+4094]=0;memcpy(s->expected_frame.bytes,s->frame.bytes,CALLER_OBJECT);short version=-112;struct packet_caller_result result;
                CHECK(packet_caller_decode(&g,1,s->frame.bytes+base,4095,4095,s->native.bytes+base,native_size,0,0,0,&version,&result) && result.error==PACKET_CALLER_OK && result.frame_size==4095 && version==1,"outer_exact_maximum_decode");CHECK(result.group.supplied_length==4093 && result.group.payload_length==4092 && result.group.wire_used==4092,"outer_maximum_group_trailer_and_consumed_count");if(caller_whole_objects(ctx,s))return 1;
            }
        }
    }
    if(fprintf(ctx->report,"CALLER LIMIT outer_maximum=4095 exact_group=4093 raw_native=4091 one_over_group=4094 raw_native=4092 frame_on_rejection=unchanged offsets=0-7\n")<0 || fflush(ctx->report)!=0)return 1;
    return 0;
}
static int original_observations(struct caller_context *ctx,struct caller_storage *s)
{
    const uint32_t endian_probe=1;byte endian_bytes[4];memcpy(endian_bytes,&endian_probe,4);int little=endian_bytes[0]==1;
    word header=0xa5a5;ctx->name="original_native_header_and_unconditional_network_swap";++ctx->cases;build_message_header(&header,6,3,0);
    CHECK(header==0x006c && GET_MESSAGE_SIZE(header)==6 && GET_MESSAGE_TYPE(header)==3 && GET_MESSAGE_FLAGS(header)==0,"original_header_numeric_and_field_contract");
    byte native_header[2],wire_header[2];memcpy(native_header,&header,2);const byte expected_native[2]={(byte)(little?0x6c:0),(byte)(little?0:0x6c)};CHECK(memcmp(native_header,expected_native,2)==0,"original_header_native_bytes_by_compiler_endian");
    byte_swap_message_header(&header,_byte_order_network);memcpy(wire_header,&header,2);const byte expected_wire[2]={(byte)(little?0:0x6c),(byte)(little?0x6c:0)};CHECK(header==0x6c00 && memcmp(wire_header,expected_wire,2)==0,"original_unconditional_swap_platform_wire_bytes");byte_swap_message_header(&header,_byte_order_host);CHECK(header==0x006c,"original_network_host_double_swap_roundtrip");
    byte_swap_message_header(&header,_byte_order_host);CHECK(header==0x6c00,"original_host_order_also_unconditional_swap");byte_swap_message_header(&header,_byte_order_network);CHECK(header==0x006c,"original_both_order_roundtrip");
    if(fprintf(ctx->report,"CALLER ORIGINAL endian=%s native_header=%02x%02x network_header=%02x%02x numeric_header=006c original_both_orders=unconditional_swap\n",little?"little":"big",native_header[0],native_header[1],wire_header[0],wire_header[1])<0 || fflush(ctx->report)!=0)return 1;
    for(unsigned kind=0;kind<2;++kind) {
        struct packet_caller_union_observation observation;long initialized=kind?128:4352;ctx->name=kind?"actual_key_union_member_operations":"actual_network_union_member_operations";++ctx->cases;
        if(kind)packet_caller_reference_key_union(4,&observation);else packet_caller_reference_network_union(4,&observation);
        long written_value=little?4:(4L<<16)|initialized;const byte expected[4]={(byte)(little?4:0),(byte)(little?0:4),(byte)(little?0:(initialized>>8)),(byte)(little?0:initialized)};
        CHECK(observation.initialized_value==initialized && observation.initial_encoded==(little?(short)initialized:0),"actual_union_initialize_long_read_initial_short");CHECK(observation.after_write_encoded==4 && observation.after_write_value==written_value && memcmp(observation.bytes,expected,4)==0,"actual_union_short_write_long_read_and_all_bytes");
        if(fprintf(ctx->report,"CALLER ORIGINAL union=%s initialized=%ld initial_encoded=%d written_short=%d after_long=%ld bytes=%02x%02x%02x%02x positive_initial_short_precondition=%u excerpt_wrapper=1\n",kind?"key":"network",observation.initialized_value,observation.initial_encoded,observation.after_write_encoded,observation.after_write_value,observation.bytes[0],observation.bytes[1],observation.bytes[2],observation.bytes[3],observation.initial_encoded>0)<0 || fflush(ctx->report)!=0)return 1;
    }
    const byte group_wire[]={1,0x21,0x32,0};ctx->name="original_create_message_aligned_initialized_disjoint";++ctx->cases;caller_objects(s,0,NULL,0);memcpy(s->expected_frame.bytes,expected_native,2);memcpy(s->expected_frame.bytes+2,group_wire,4);
    CHECK(create_message(3,group_wire,4,s->frame.bytes,CALLER_OBJECT)==s->frame.bytes,"original_create_message_returns_provided_buffer");if(caller_whole_objects(ctx,s))return 1;
    ctx->name="original_create_message_authored_initialized_allocator";++ctx->cases;s->reference_heap=create_message(3,group_wire,4,NULL,0);CHECK(s->reference_heap!=NULL,"original_create_message_heap_buffer");if(!s->reference_heap){if(fprintf(ctx->report,"CALLER ABORT original_initialized_allocator_failed\n")<0 || fflush(ctx->report)!=0)return 1;return 1;}
    byte expected_heap[6];memcpy(expected_heap,expected_native,2);memcpy(expected_heap+2,group_wire,4);CHECK(memcmp(s->reference_heap,expected_heap,6)==0,"original_allocated_message_native_header_and_data");free(s->reference_heap);s->reference_heap=NULL;
    const unsigned lengths[]={0,1,4,4093};
    for(unsigned n=0;n<sizeof(lengths)/sizeof(lengths[0]);++n)for(long type=1;type<=3;++type){unsigned length=lengths[n];ctx->name="original_create_message_safe_type_length_boundaries";++ctx->cases;caller_objects(s,0,NULL,0);for(unsigned k=0;k<length;++k)s->workspace.bytes[k]=(byte)(0x31+k);memcpy(s->expected_workspace.bytes,s->workspace.bytes,CALLER_OBJECT);word numeric=(word)(((length+2)<<4)|((unsigned)type<<2));memcpy(s->expected_frame.bytes,&numeric,2);memcpy(s->expected_frame.bytes+2,s->workspace.bytes,length);
        CHECK(create_message(type,s->workspace.bytes,length,s->frame.bytes,CALLER_OBJECT)==s->frame.bytes,"original_safe_create_message_boundary_return");if(caller_whole_objects(ctx,s))return 1;
    }
    if(fprintf(ctx->report,"CALLER ORIGINAL create_message actual_bodies=3 aligned_offsets=0 supplied_storage=initialized data_sizes=0,1,4,4093 types=1,2,3 allocator=authored_initialized_service full_network_and_crypto_callers=unexecuted\n")<0 || fflush(ctx->report)!=0)return 1;
    return 0;
}
int wii_packet_caller_fixture(FILE *report,int collect)
{
    struct caller_context ctx={report,"startup",0,0,0,collect};struct caller_storage *s=calloc(1,sizeof(*s));int aborted=0;
    if(!s){if(fprintf(report,"CALLER ABORT fixture_allocation_failed\n")<0 || fflush(report)!=0)return 1;return 1;}
    for(unsigned n=0;n<CALLER_PLANS;++n) {
        const struct data_packet_field end={_data_packet_field_end,-7,-8,-9,117};short size=n==0?2:n==1?4:n==2?4091:4092;size_t bound=n==1?4:2;
        s->fields[n][0]=(struct data_packet_field){n==1?_data_packet_field_array:n>=2?_data_packet_field_raw:_data_packet_field_bytes,n==1?2:size,0,0,113};s->fields[n][1]=end;
        if(n==1){s->fields[n][1]=(struct data_packet_field){_data_packet_field_bytes,1,0,0,113};s->fields[n][2]=end;s->fields[n][3]=end;}
        struct data_packet_definition *d=&s->definitions[n];memset(d,0xa5,sizeof(*d));d->name="authored_caller_payload";d->flags=0x1234;d->size=size;d->version=1;d->fields=s->fields[n];d->initialized=FALSE;
        struct packet_array_result result;if(!packet_array_compile(d,bound,&s->plans[n],&result)){if(fprintf(report,"CALLER ABORT payload_plan=%u error=%s\n",n,packet_array_error_name(result.error))<0 || fflush(report)!=0)aborted=1;aborted=1;goto cleanup;}
    }
    s->entries[0]=(struct packet_group_entry){0,&s->plans[0]};s->entries[1]=(struct packet_group_entry){1,NULL};s->entries[2]=(struct packet_group_entry){1,&s->plans[1]};memcpy(s->entries_before,s->entries,sizeof(s->entries));
    struct data_packet_field fields_before[CALLER_PLANS][4];struct data_packet_definition definitions_before[CALLER_PLANS];struct packet_array_plan plans_before[CALLER_PLANS];struct packet_array_node nodes_before[CALLER_PLANS][4];
    memcpy(fields_before,s->fields,sizeof(fields_before));memcpy(definitions_before,s->definitions,sizeof(definitions_before));memcpy(plans_before,s->plans,sizeof(plans_before));for(unsigned n=0;n<CALLER_PLANS;++n)memcpy(nodes_before[n],s->plans[n].nodes,s->plans[n].count*sizeof(*s->plans[n].nodes));
    if(fprintf(report,"CALLER BEGIN scope=bounded_framing_composition native_identity=expected_type_binds_reserve outer_length=exact inner_trailing=accepted original_header_and_union=separate\n")<0 || fflush(report)!=0){aborted=1;goto cleanup;}
    aborted=ordinary_encode(&ctx,s) ||ordinary_decode(&ctx,s) ||preflight_cases(&ctx,s) ||alias_cases(&ctx,s) ||maximum_frame_cases(&ctx,s) ||original_observations(&ctx,s);
    ctx.name="caller_sources_entries_and_borrowed_plans_immutable";
    if(!caller_check(&ctx,memcmp(fields_before,s->fields,sizeof(fields_before))==0 && memcmp(definitions_before,s->definitions,sizeof(definitions_before))==0 && memcmp(s->entries,s->entries_before,sizeof(s->entries))==0,"whole_source_cache_and_group_entries_unchanged"))aborted=1;
    if(!caller_check(&ctx,memcmp(plans_before,s->plans,sizeof(plans_before))==0,"borrowed_plan_controls_unchanged"))aborted=1;
    for(unsigned n=0;n<CALLER_PLANS;++n)if(!caller_check(&ctx,memcmp(nodes_before[n],s->plans[n].nodes,s->plans[n].count*sizeof(*s->plans[n].nodes))==0,"borrowed_plan_nodes_unchanged"))aborted=1;
cleanup:
    for(unsigned n=0;n<CALLER_PLANS;++n) packet_array_destroy(&s->plans[n]);
    free(s->reference_heap);free(s);
    if(fprintf(report,"CALLER SUMMARY cases=%u checks=%u failures=%u aborted=%u\nCALLER END result=%u\n",ctx.cases,ctx.checks,ctx.failures,aborted!=0,aborted ||ctx.failures!=0)<0 || fflush(report)!=0)return 1;
    return aborted ||ctx.failures!=0;
}
