#include "native_packet_fixture.h"
#include "native_packet_binding.h"
#include "packet_caller_policy.h"
#include <limits.h>

enum { NATIVE_ROWS=37, NATIVE_FIELDS=8, NATIVE_OBJECT=4480, NATIVE_GUARD=16 };
struct native_context { FILE *report; const char *name; unsigned cases,checks,failures,gaps; int collect; };
struct native_storage {
    struct packet_array_plan plans[NATIVE_ROWS];
    struct native_packet_binding bindings[NATIVE_ROWS],lifetime;
    struct packet_group_entry network_entries[35],key_entries[2];
    struct data_packet_definition definitions_before[NATIVE_ROWS];
    struct data_packet_field fields_before[NATIVE_ROWS][NATIVE_FIELDS];
    struct data_packet_group_definition groups_before[2];
    struct data_packet_entry entries_before[NATIVE_ROWS];
    struct data_packet_definition *temporary_definition;
    struct data_packet_field *temporary_fields;
    byte native[NATIVE_OBJECT],workspace[NATIVE_OBJECT],frame[NATIVE_OBJECT];
    byte expected_native[NATIVE_OBJECT],expected_workspace[NATIVE_OBJECT],expected_frame[NATIVE_OBJECT];
    byte representation[4352],decoded[4352],payload[4352],decoded_wire[4352];
};
struct scalar_run { unsigned offset,width,count; };
struct native_shape {
    short type; unsigned owner,size; const char *name;
    struct scalar_run runs[3]; unsigned run_count,count_offset,children,maximum;
    int key;
};
static const struct native_shape shapes[] = {
    {0,1,12,"message_client_broadcast_game_search",{{0,2,2},{4,1,8}},2,0,0,0,0},
    {1,1,8,"message_client_ping",{{0,4,1},{4,2,1}},2,0,0,0,0},
    {3,0,4,"message_server_pong",{{0,4,1}},1,0,0,0,0},
    {4,0,8,"message_server_machine_accepted",{{0,4,1},{4,2,1}},2,0,0,0,0},
    {6,1,3592,"message_server_game_settings_update",{{0,2,3},{8,1,3584}},2,0,0,0,0},
    {12,1,112,"message_client_join_game_request",{{0,2,32},{64,1,48}},2,0,0,0,0},
    {15,1,68,"message_client_settings_request",{{0,2,32},{64,1,1}},2,0,0,0,0},
    {20,0,4112,"message_server_game_update",{{0,4,3}},1,14,16,128,0},
    {25,1,136,"message_client_game_update",{{0,4,1}},1,6,8,4,0},
    {13,5,32,"network_player",{{0,2,14},{28,1,4}},2,0,0,0,0},
    {21,5,32,"network_player",{{0,2,14},{28,1,4}},2,0,0,0,0},
    {0,4,24,"message_initiate_key_agreement",{{0,4,6}},1,0,0,0,1},
    {1,4,8,"message_finalize_key_agreement",{{0,4,2}},1,0,0,0,1},
};
static int native_check(struct native_context *ctx,int condition,const char *assertion)
{
    ++ctx->checks;
    if(condition)return 1;
    ++ctx->failures;
    if(fprintf(ctx->report,"NATIVE FAIL case=%s assertion=%s check=%u\n",ctx->name,assertion,ctx->checks)<0 || fflush(ctx->report)!=0)return 0;
    return ctx->collect;
}
#define CHECK(c,n) do { if(!native_check(ctx,(c),(n)))return 1; } while(0)
static int native_abort(struct native_context *ctx,const char *reason)
{
    (void)fprintf(ctx->report,"NATIVE ABORT case=%s reason=%s\n",ctx->name,reason);
    (void)fflush(ctx->report);return 1;
}
static const struct native_abi_layout *layout_at(unsigned owner,const char *name)
{
    size_t count=0;const struct native_abi_layout *rows=native_abi_owner_layouts(owner,&count);
    if(!rows)return NULL;
    for(size_t i=0;i<count;++i)if(strcmp(rows[i].name,name)==0)return &rows[i];
    return NULL;
}
static int exact_members(struct native_context *ctx,const struct native_abi_layout *row,size_t size,
                         const size_t *offsets,const size_t *sizes,size_t members)
{
    CHECK(row->size==size && row->member_count==members,"independent_size_member_count");
    if(row->member_count!=members)return native_abort(ctx,"measured_member_shape_changed");
    for(size_t i=0;i<members;++i)CHECK(row->members[i].offset==offsets[i] && row->members[i].size==sizes[i],"independent_member_offset_size");
    return 0;
}
static int layouts(struct native_context *ctx)
{
    size_t total=0,members=0;ctx->name="actual_owner_layouts";
    CHECK(native_abi_owner_count()==6,"six_source_owners");
    for(size_t owner=0;owner<native_abi_owner_count();++owner){
        size_t count=0;const struct native_abi_layout *rows=native_abi_owner_layouts(owner,&count);
        CHECK(rows!=NULL && count>0,"owner_layout_table_present");
        if(!rows || !count)return native_abort(ctx,"missing_owner_table");
        for(size_t i=0;i<count;++i){
            const struct native_abi_layout *r=&rows[i];++ctx->cases;++total;members+=r->member_count;
            CHECK(r->size>0 && r->alignment>0 && r->size%r->alignment==0,"actual_size_alignment");
            CHECK(r->members!=NULL && r->member_count>0,"actual_member_table");
            if(!r->members)return native_abort(ctx,"missing_member_table");
            if(fprintf(ctx->report,"NATIVE LAYOUT owner=%s name=%s size=%lu alignment=%lu members=%lu\n",r->owner,r->name,(unsigned long)r->size,(unsigned long)r->alignment,(unsigned long)r->member_count)<0)return 1;
            for(size_t j=0;j<r->member_count;++j){const struct native_abi_member *m=&r->members[j];
                CHECK(m->offset<=r->size && m->size<=r->size-m->offset,"actual_member_fits_object");
                if(fprintf(ctx->report,"NATIVE MEMBER owner=%s layout=%s name=%s offset=%lu size=%lu\n",r->owner,r->name,m->name,(unsigned long)m->offset,(unsigned long)m->size)<0)return 1;
            }
            if(strcmp(r->name,"player_action")==0){
                const size_t o[]={0,4,12,20,24,26,28,30},z[]={4,8,8,4,2,2,2,2};
                if(exact_members(ctx,r,32,o,z,8))return 1;
            }else if(strcmp(r->name,"network_player")==0){
                const size_t o[]={0,24,26,28,29,30,31},z[]={24,2,2,1,1,1,1};
                if(exact_members(ctx,r,32,o,z,7))return 1;
            }else if(strcmp(r->name,"public_key")==0){
                const size_t o[]={0},z[]={8};if(exact_members(ctx,r,8,o,z,1))return 1;
            }else if(strcmp(r->name,"message_initiate_key_agreement")==0){
                const size_t o[]={0,8,16},z[]={8,8,8};if(exact_members(ctx,r,24,o,z,3))return 1;
            }else if(strcmp(r->name,"message_finalize_key_agreement")==0){
                const size_t o[]={0},z[]={8};if(exact_members(ctx,r,8,o,z,1))return 1;
            }else if(strcmp(r->name,"message_server_game_update")==0){
                const size_t o[]={0,4,8,12,14,16},z[]={4,4,4,2,2,4096};if(exact_members(ctx,r,4112,o,z,6))return 1;
            }else if(strcmp(r->name,"message_client_game_update")==0){
                const size_t o[]={0,4,6,8},z[]={4,2,2,128};if(exact_members(ctx,r,136,o,z,4))return 1;
            }else if(strcmp(r->name,"message_client_broadcast_game_search")==0){
                const size_t o[]={0,2,4},z[]={2,2,8};if(exact_members(ctx,r,12,o,z,3))return 1;
            }else if(strcmp(r->name,"message_client_ping")==0){
                const size_t o[]={0,4,6},z[]={4,2,2};if(exact_members(ctx,r,8,o,z,3))return 1;
            }else if(strcmp(r->name,"message_server_pong")==0){
                const size_t o[]={0},z[]={4};if(exact_members(ctx,r,4,o,z,1))return 1;
            }else if(strcmp(r->name,"message_server_machine_accepted")==0){
                const size_t o[]={0,4,6},z[]={4,2,2};if(exact_members(ctx,r,8,o,z,3))return 1;
            }else if(strcmp(r->name,"message_server_machine_rejected")==0){
                const size_t o[]={0},z[]={2};if(exact_members(ctx,r,2,o,z,1))return 1;
            }else if(strcmp(r->name,"message_server_game_settings_update")==0){
                const size_t o[]={0,2,4,6,8},z[]={2,2,2,2,3584};if(exact_members(ctx,r,3592,o,z,5))return 1;
            }else if(strcmp(r->name,"message_client_join_game_request")==0){
                const size_t o[]={0,64,80},z[]={64,16,32};if(exact_members(ctx,r,112,o,z,3))return 1;
            }else if(strcmp(r->name,"message_client_settings_request")==0){
                const size_t o[]={0,64,65},z[]={64,1,3};if(exact_members(ctx,r,68,o,z,3))return 1;
            }else if(strcmp(r->name,"message_client_game_start_request")==0){
                CHECK(r->size==(owner==1?4U:2U),"receiver_long_sender_short_conflict_preserved");
            }
            for(size_t shape=0;shape<sizeof(shapes)/sizeof(shapes[0]);++shape)
                if(strcmp(shapes[shape].name,r->name)==0)CHECK(r->size==shapes[shape].size,"independent_real_message_size");
        }
    }
    CHECK(total==58 && members==127,"actual_owner_layout_and_member_totals");
    CHECK(native_abi_selected_wchar_width()==2,"explicit_selected_short_wchar");
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    CHECK(native_abi_default_wchar_width()==4,"default_ppc_wchar_four_separate_observation");
#else
    CHECK(native_abi_default_wchar_width()==2,"default_host_wchar_two_separate_observation");
#endif
    if(fprintf(ctx->report,"NATIVE WIDTH default_wchar=%lu selected_wchar=%lu selected_flag=-fshort-wchar default_unit_has_no_override char_min=%d char_none_is_negative=%d\n",(unsigned long)native_abi_default_wchar_width(),(unsigned long)native_abi_selected_wchar_width(),CHAR_MIN,((char)-1)<0)<0)return 1;
    return 0;
}
static const struct native_abi_catalog *catalog_row(size_t index)
{
    size_t count=0;const struct native_abi_catalog *rows=index<35?native_abi_network_catalog(&count):native_abi_key_catalog(&count);
    size_t position=index<35?index:index-35;
    return rows && position<count?&rows[position]:NULL;
}
static struct packet_group_plan group_at(struct native_storage *s,int key)
{
    const struct data_packet_group_definition *g=key?native_abi_key_group():native_abi_network_group();
    return (struct packet_group_plan){g->packet_type_count,g->packet_class_count,g->maximum_decoded_packet_size,g->maximum_encoded_packet_size,key?s->key_entries:s->network_entries};
}
static int catalogs(struct native_context *ctx,struct native_storage *s)
{
    size_t network_count=0,key_count=0,enum_count=0;
    const struct native_abi_catalog *network=native_abi_network_catalog(&network_count),*key=native_abi_key_catalog(&key_count);
    const struct native_abi_enum *values=native_abi_network_enum(&enum_count);
    const struct data_packet_group_definition *ng=native_abi_network_group(),*kg=native_abi_key_group();
    ctx->name="actual_catalog_source_snapshots";
    CHECK(network && key && values && ng && kg,"actual_catalog_tables_and_groups");
    if(!network || !key || !values || !ng || !kg)return native_abort(ctx,"catalog_accessor_missing");
    CHECK(network_count==35 && key_count==2 && enum_count==35,"actual_catalog_enum_counts");
    if(network_count!=35 || key_count!=2 || enum_count!=35)return native_abort(ctx,"catalog_shape_changed");
    CHECK(ng->packet_type_count==35 && ng->packet_class_count==8 && ng->maximum_decoded_packet_size==4352 && ng->maximum_encoded_packet_size==4352,"actual_network_limits");
    CHECK(kg->packet_type_count==2 && kg->packet_class_count==1 && kg->maximum_decoded_packet_size==96 && kg->maximum_encoded_packet_size==128,"actual_key_limits");
    memcpy(&s->groups_before[0],ng,sizeof(*ng));memcpy(&s->groups_before[1],kg,sizeof(*kg));
    memcpy(s->entries_before,ng->packets,35*sizeof(*ng->packets));memcpy(s->entries_before+35,kg->packets,2*sizeof(*kg->packets));
    unsigned conflicts=0;
    for(size_t i=0;i<NATIVE_ROWS;++i){
        const struct native_abi_catalog *r=catalog_row(i);++ctx->cases;
        CHECK(r && r->definition && r->definition->fields && r->field_bound>0 && r->field_bound<=NATIVE_FIELDS,"catalog_truthful_field_bound");
        if(!r || !r->definition || !r->definition->fields || !r->field_bound || r->field_bound>NATIVE_FIELDS)return native_abort(ctx,"catalog_fields_unavailable");
        memcpy(&s->definitions_before[i],r->definition,sizeof(*r->definition));memcpy(s->fields_before[i],r->definition->fields,r->field_bound*sizeof(*r->definition->fields));
        struct packet_array_result result;boolean compiled=packet_array_compile(r->definition,r->field_bound,&s->plans[i],&result);
        CHECK(compiled,"actual_catalog_schema_compiles_without_initialization");
        if(!compiled)return native_abort(ctx,"actual_schema_compile_failed");
        CHECK(s->plans[i].extent==(uint32_t)r->definition->size && s->plans[i].version==1,"stable_catalog_reserve_and_version_one");
        const struct data_packet_group_definition *g=i<35?ng:kg;size_t type=i<35?i:i-35;
        CHECK(r->type==(short)type && g->packets[type].definition==r->definition && g->packets[type].packet_class==r->packet_class && strcmp(r->name,r->definition->name)==0,"actual_table_numeric_class_name_definition_relationship");
        struct packet_group_entry *entry=i<35?&s->network_entries[type]:&s->key_entries[type];*entry=(struct packet_group_entry){r->packet_class,&s->plans[i]};
        if(i<35){char expected[160];int n=snprintf(expected,sizeof(expected),"%s_packet",values[i].name+1);
            CHECK(n>0 && (size_t)n<sizeof(expected) && values[i].value==(short)i,"actual_enum_name_numeric_order");
            if(n<0 || (size_t)n>=sizeof(expected))return native_abort(ctx,"enum_identity_name_overflow");
            if(strcmp(expected,r->name)!=0){++conflicts;++ctx->gaps;struct native_packet_binding before;memcpy(&before,&s->bindings[i],sizeof(before));struct native_packet_binding_result br;
                boolean accepted=native_packet_bind(r,(short)i,expected,(size_t)r->definition->size,&s->bindings[i],&br);
                CHECK(!accepted && br.error==NATIVE_PACKET_BINDING_IDENTITY && memcmp(&before,&s->bindings[i],sizeof(before))==0,"identity_conflict_atomic_binding_rejection");
                CHECK(i>=8 && i<=10,"only_known_catalog_identity_conflicts");
                if(fprintf(ctx->report,"NATIVE GAP kind=catalog_identity type=%lu enum=%s actual_packet=%s binding_rejected_before_io=1\n",(unsigned long)i,values[i].name,r->name)<0)return 1;
            }
        }
        if(fprintf(ctx->report,"NATIVE CATALOG kind=%s type=%d class=%d name=%s fields=%lu opaque_or_key_size=%d reserve=%lu version=%d cache=%u\n",i<35?"network":"key",r->type,r->packet_class,r->name,(unsigned long)r->field_bound,r->definition->size,(unsigned long)s->plans[i].extent,r->definition->version,r->definition->initialized)<0)return 1;
    }
    CHECK(conflicts==3,"exactly_three_original_catalog_identity_conflicts");
    struct native_packet_binding_result br;struct native_packet_binding before;memcpy(&before,&s->bindings[17],sizeof(before));
    const struct native_abi_layout *receiver=layout_at(1,"message_client_game_start_request"),*sender=layout_at(2,"message_client_game_start_request");
    CHECK(receiver && sender,"actual_game_start_caller_layouts_present");
    if(!receiver || !sender)return native_abort(ctx,"game_start_layout_missing");
    CHECK(receiver->size==4 && sender->size==2 && network[17].definition->size==2,"actual_game_start_layout_conflict");
    boolean accepted=native_packet_bind(&network[17],17,network[17].name,receiver->size,&s->bindings[17],&br);
    CHECK(!accepted && br.error==NATIVE_PACKET_BINDING_LAYOUT && memcmp(&before,&s->bindings[17],sizeof(before))==0,"receiver_four_atomic_binding_rejection");
    ++ctx->cases;struct native_abi_catalog tampered=network[10];tampered.name=network[9].name;
    memcpy(&before,&s->bindings[10],sizeof(before));accepted=native_packet_bind(&tampered,10,tampered.name,4,&s->bindings[10],&br);
    CHECK(!accepted && br.error==NATIVE_PACKET_BINDING_IDENTITY && memcmp(&before,&s->bindings[10],sizeof(before))==0,"same_size_class_definition_name_mismatch_atomic_binding_rejection");
    ++ctx->gaps;
    if(fprintf(ctx->report,"NATIVE GAP kind=caller_layout owner=network_server_message_handler message=client_game_start_request receiver_size=4 sender_size=2 schema_reserve=2 binding_rejected_before_io=1\n")<0)return 1;
    for(unsigned i=0;i<3;++i){++ctx->gaps;if(fprintf(ctx->report,"NATIVE GAP kind=unsupported_advertise owner=%u reason=XDK_transport_and_map_dependencies opaque_catalog_is_not_typed_owner_proof\n",i)<0)return 1;}
    if(native_abi_default_wchar_width()!=2){++ctx->gaps;if(fprintf(ctx->report,"NATIVE GAP kind=default_wchar default=4 selected=2 engine_default_not_qualified\n")<0)return 1;}
    if(CHAR_MIN==0){++ctx->gaps;if(fprintf(ctx->report,"NATIVE GAP kind=plain_char_none unsigned_char_minus_one_is_255 typed_index_comparisons_not_qualified\n")<0)return 1;}
    for(size_t i=0;i<sizeof(shapes)/sizeof(shapes[0]);++i){const struct native_shape *shape=&shapes[i];size_t index=(size_t)shape->type+(shape->key?35U:0U);
        const struct native_abi_layout *l=layout_at(shape->owner,shape->name);const struct native_abi_catalog *r=catalog_row(index);
        CHECK(l!=NULL,"selected_actual_owner_layout_present");if(!l)return native_abort(ctx,"selected_layout_missing");
        accepted=native_packet_bind(r,shape->type,r->name,l->size,&s->bindings[index],&br);
        CHECK(accepted && br.error==NATIVE_PACKET_BINDING_OK,"selected_actual_identity_size_and_schema_binding");if(!accepted)return native_abort(ctx,"selected_binding_failed");
        CHECK(s->bindings[index].native_size==shape->size && s->bindings[index].plan.extent==shape->size,"selected_binding_stable_native_reserve");
        struct packet_group_entry *entry=shape->key?&s->key_entries[shape->type]:&s->network_entries[shape->type];entry->plan=&s->bindings[index].plan;
    }
    struct packet_group_result gr;struct packet_group_plan g=group_at(s,0);CHECK(packet_group_validate(&g,35,&gr),"actual_network_group_bounded_composition");g=group_at(s,1);CHECK(packet_group_validate(&g,2,&gr),"actual_key_group_bounded_composition");
    return 0;
}
static void objects(struct native_storage *s)
{
    memset(s->native,0xa5,sizeof(s->native));memset(s->workspace,0xcc,sizeof(s->workspace));memset(s->frame,0xdd,sizeof(s->frame));
    memcpy(s->expected_native,s->native,sizeof(s->native));memcpy(s->expected_workspace,s->workspace,sizeof(s->workspace));memcpy(s->expected_frame,s->frame,sizeof(s->frame));
}
static int whole(struct native_context *ctx,const struct native_storage *s)
{
    CHECK(memcmp(s->native,s->expected_native,NATIVE_OBJECT)==0,"whole_native_guard_padding_unused_reserve");
    CHECK(memcmp(s->workspace,s->expected_workspace,NATIVE_OBJECT)==0,"whole_workspace_guard_and_partial_output");
    CHECK(memcmp(s->frame,s->expected_frame,NATIVE_OBJECT)==0,"whole_frame_guard_and_scalar_input_mutation");return 0;
}
static void scalar_oracle(struct native_storage *s,unsigned native_offset,unsigned width,unsigned serial,size_t *used)
{
    uint32_t value=width==4?UINT32_C(0x01234567)+serial:width==2?UINT32_C(0x1234)+serial:(serial*13U+0x43U)&255U;
    byte native[4];
    if(width==4)memcpy(native,&value,4);else if(width==2){uint16_t v=(uint16_t)value;memcpy(native,&v,2);}else native[0]=(byte)value;
    memcpy(s->representation+native_offset,native,width);memcpy(s->decoded+native_offset,native,width);
    for(unsigned i=0;i<width;++i)s->payload[*used+i]=(byte)(value>>((width-i-1)*8));
    memcpy(s->decoded_wire+*used,native,width);*used+=width;
}
static size_t oracle(struct native_storage *s,const struct native_shape *shape,short count)
{
    memset(s->representation,0xa5,sizeof(s->representation));memset(s->decoded,0xa5,sizeof(s->decoded));
    size_t used=1;s->payload[0]=1;s->decoded_wire[0]=1;unsigned serial=0;
    for(unsigned r=0;r<shape->run_count;++r)for(unsigned n=0;n<shape->runs[r].count;++n)
        scalar_oracle(s,shape->runs[r].offset+n*shape->runs[r].width,shape->runs[r].width,serial++,&used);
    if(shape->maximum){
        memcpy(s->representation+shape->count_offset,&count,2);memcpy(s->decoded+shape->count_offset,&count,2);
        s->payload[used]=s->decoded_wire[used]=(byte)count;++used;
        for(short child=0;child<count;++child){unsigned base=shape->children+(unsigned)child*32;
            for(unsigned n=0;n<6;++n)scalar_oracle(s,base+n*4,4,serial++,&used);
            for(unsigned n=0;n<3;++n)scalar_oracle(s,base+24+n*2,2,serial++,&used);
        }
    }
    return used;
}
static void expected_frame(struct native_storage *s,unsigned base,const byte *payload,size_t length,short type,byte flags)
{
    unsigned total=(unsigned)length+3,header=(total<<4)|12U|flags;
    s->expected_frame[base]=(byte)(header>>8);s->expected_frame[base+1]=(byte)header;
    memcpy(s->expected_frame+base+2,payload,length);s->expected_frame[base+2+length]=(byte)type;
}
static int real_vector(struct native_context *ctx,struct native_storage *s,const struct native_shape *shape,short count)
{
    struct packet_group_plan g=group_at(s,shape->key);size_t bound=shape->key?2:35;byte flags=shape->key?2:0;
    size_t payload=oracle(s,shape,count),total=payload+3;ctx->name=shape->name;
    CHECK(total<=4095 && shape->size<=4352,"independent_real_shape_within_frame_limits");
    if(total>4095 || shape->size>4352)return native_abort(ctx,"oracle_range_changed");
    if(shape->type==20 && count==128)CHECK(total==3857,"server_maximum_128_independent_wire_frame_3857");
    for(unsigned offset=0;offset<8;++offset){unsigned base=NATIVE_GUARD+offset;++ctx->cases;
        objects(s);memcpy(s->native+base,s->representation,shape->size);memcpy(s->expected_native,s->native,sizeof(s->native));
        memcpy(s->expected_workspace+base,s->payload,payload);s->expected_workspace[base+payload]=(byte)shape->type;expected_frame(s,base,s->payload,payload,shape->type,flags);
        struct packet_caller_result result;boolean accepted=packet_caller_encode(&g,bound,s->native+base,shape->size,s->workspace+base,4352,s->frame+base,4095,shape->type,NONE,flags,&result);
        CHECK(accepted && result.error==PACKET_CALLER_OK && result.frame_size==(long)total,"actual_schema_encode_independent_be_frame_golden");
        CHECK(result.group.wire_used==(long)payload+1 && result.group.payload.wire_used==(long)payload,"actual_schema_encode_numeric_consumption");if(whole(ctx,s))return 1;
        objects(s);expected_frame(s,base,s->payload,payload,shape->type,flags);memcpy(s->frame,s->expected_frame,sizeof(s->frame));
        memcpy(s->expected_native+base,s->decoded,shape->size);expected_frame(s,base,s->decoded_wire,payload,shape->type,flags);
        short version=-71;accepted=packet_caller_decode(&g,bound,s->frame+base,(long)total,4095,s->native+base,shape->size,shape->type,g.entries[shape->type].packet_class,flags,&version,&result);
        CHECK(accepted && version==1 && result.error==PACKET_CALLER_OK,"actual_schema_decode_bound_identity_and_version");
        CHECK(result.group.supplied_length==(long)payload+1 && result.group.payload_length==(long)payload && result.group.wire_used==(long)payload,"actual_schema_decode_trailer_and_payload_consumption");if(whole(ctx,s))return 1;
        ++ctx->cases;objects(s);memcpy(s->native+base,s->representation,shape->size);memcpy(s->expected_native,s->native,sizeof(s->native));
        accepted=packet_caller_encode(&g,bound,s->native+base,(long)shape->size-1,s->workspace+base,4352,s->frame+base,4095,shape->type,NONE,flags,&result);
        CHECK(!accepted && result.error==PACKET_CALLER_GROUP && result.group.error==PACKET_GROUP_PAYLOAD && result.group.payload.error==PACKET_ARRAY_CAPACITY && result.group.wire_used==0,"actual_stable_native_reserve_one_short_encode_no_io");if(whole(ctx,s))return 1;
        objects(s);expected_frame(s,base,s->payload,payload,shape->type,flags);memcpy(s->frame,s->expected_frame,sizeof(s->frame));version=-71;
        accepted=packet_caller_decode(&g,bound,s->frame+base,(long)total,4095,s->native+base,(long)shape->size-1,shape->type,g.entries[shape->type].packet_class,flags,&version,&result);
        CHECK(!accepted && version==-71 && result.error==PACKET_CALLER_GROUP && result.group.payload.error==PACKET_ARRAY_CAPACITY && result.group.payload_length==(long)payload && result.group.wire_used==0,"actual_stable_native_reserve_one_short_decode_trailer_only");if(whole(ctx,s))return 1;
    }
    if(fprintf(ctx->report,"NATIVE VECTOR kind=%s type=%d name=%s count=%d native=%u payload=%lu frame=%lu offsets=0-7 padding_and_unused_reserve_preserved=1 scope=inert_representation\n",shape->key?"key":"network",shape->type,shape->name,count,shape->size,(unsigned long)payload,(unsigned long)total)<0)return 1;
    return 0;
}
static int real_arrays(struct native_context *ctx,struct native_storage *s,const struct native_shape *shape)
{
    size_t index=(size_t)shape->type;const struct packet_array_plan *p=&s->bindings[index].plan;
    unsigned array_index=2;ctx->name="real_array_plan_count_stride";++ctx->cases;
    CHECK(p->depth==2 && p->count==8 && p->nodes[array_index].field.type==_data_packet_field_array && p->nodes[array_index].field.count==(short)shape->maximum && p->nodes[array_index].child_extent==32,"real_array_snapshot_child_stride_and_count");
    struct packet_group_plan g=group_at(s,0);
    for(unsigned offset=0;offset<8;++offset){unsigned base=NATIVE_GUARD+offset;
        for(unsigned which=0;which<2;++which){short invalid=which?(short)(shape->maximum+1):-1;++ctx->cases;
            size_t payload=oracle(s,shape,0),prefix=payload-1;objects(s);memcpy(s->native+base,s->representation,shape->size);memcpy(s->native+base+shape->count_offset,&invalid,2);memcpy(s->expected_native,s->native,sizeof(s->native));memcpy(s->expected_workspace+base,s->payload,prefix);
            struct packet_caller_result result;boolean accepted=packet_caller_encode(&g,35,s->native+base,shape->size,s->workspace+base,4352,s->frame+base,4095,shape->type,1,0,&result);
            CHECK(!accepted && result.error==PACKET_CALLER_GROUP && result.group.payload.error==PACKET_ARRAY_COUNT && result.group.wire_used==(long)prefix,"real_native_signed_count_reject_retains_prefix");if(whole(ctx,s))return 1;
        }
        ++ctx->cases;size_t payload=oracle(s,shape,0);s->payload[payload-1]=(byte)(shape->maximum+1);s->decoded_wire[payload-1]=s->payload[payload-1];
        objects(s);expected_frame(s,base,s->payload,payload,shape->type,0);memcpy(s->frame,s->expected_frame,sizeof(s->frame));
        memcpy(s->expected_native+base,s->decoded,shape->count_offset==14?12:4);expected_frame(s,base,s->decoded_wire,payload,shape->type,0);
        short version=-71;struct packet_caller_result result;boolean accepted=packet_caller_decode(&g,35,s->frame+base,(long)payload+3,4095,s->native+base,shape->size,shape->type,g.entries[shape->type].packet_class,0,&version,&result);
        CHECK(!accepted && version==1 && result.error==PACKET_CALLER_GROUP && result.group.payload.error==PACKET_ARRAY_COUNT && result.group.wire_used==(long)payload && result.group.payload_length==(long)payload,"real_wire_overmaximum_count_after_trailer_prior_scalar_writes");if(whole(ctx,s))return 1;
    }
    return 0;
}
static int lifetime(struct native_context *ctx,struct native_storage *s)
{
    ctx->name="owned_binding_outlives_mutable_schema_source";++ctx->cases;
    const struct native_abi_catalog *original=catalog_row(35);struct native_abi_catalog row=*original;
    s->temporary_definition=malloc(sizeof(*s->temporary_definition));s->temporary_fields=malloc(original->field_bound*sizeof(*s->temporary_fields));
    CHECK(s->temporary_definition && s->temporary_fields,"mutable_lifetime_source_allocation");
    if(!s->temporary_definition || !s->temporary_fields)return native_abort(ctx,"mutable_source_allocation_failed");
    *s->temporary_definition=*original->definition;memcpy(s->temporary_fields,original->definition->fields,original->field_bound*sizeof(*s->temporary_fields));s->temporary_definition->fields=s->temporary_fields;row.definition=s->temporary_definition;
    struct native_packet_binding_result br;boolean accepted=native_packet_bind(&row,0,original->name,24,&s->lifetime,&br);
    CHECK(accepted,"owned_binding_lifetime_compile");if(!accepted)return native_abort(ctx,"lifetime_binding_failed");
    memset(s->temporary_fields,0xee,original->field_bound*sizeof(*s->temporary_fields));s->temporary_definition->version=0;s->temporary_definition->initialized=TRUE;s->temporary_definition->size=0;
    free(s->temporary_fields);s->temporary_fields=NULL;free(s->temporary_definition);s->temporary_definition=NULL;
    const struct native_shape *shape=&shapes[sizeof(shapes)/sizeof(shapes[0])-2];size_t payload=oracle(s,shape,0);unsigned base=NATIVE_GUARD;
    objects(s);memcpy(s->native+base,s->representation,24);memcpy(s->expected_native,s->native,sizeof(s->native));memcpy(s->expected_workspace+base,s->payload,payload);s->expected_workspace[base+payload]=0;expected_frame(s,base,s->payload,payload,0,2);
    struct packet_group_plan g=group_at(s,1);const struct packet_array_plan *saved=s->key_entries[0].plan;s->key_entries[0].plan=&s->lifetime.plan;
    struct packet_caller_result result;accepted=packet_caller_encode(&g,2,s->native+base,24,s->workspace+base,128,s->frame+base,4095,0,NONE,2,&result);
    CHECK(accepted && result.frame_size==28 && s->lifetime.plan.version==1,"owned_plan_encodes_after_source_mutation_and_free");if(whole(ctx,s))return 1;
    native_packet_binding_destroy(&s->lifetime);
    CHECK(!s->lifetime.plan.nodes && !s->lifetime.plan.count && !s->lifetime.plan.depth && !s->lifetime.plan.extent && !s->lifetime.plan.version && !s->lifetime.type && !s->lifetime.packet_class && !s->lifetime.native_size,"destroyed_binding_control_remains_live_and_zero");
    objects(s);accepted=packet_caller_encode(&g,2,s->native+base,24,s->workspace+base,128,s->frame+base,4095,0,NONE,2,&result);
    CHECK(!accepted && result.error==PACKET_CALLER_GROUP && result.group.error==PACKET_GROUP_SCHEMA,"borrowed_live_cleared_plan_rejects_before_io");if(whole(ctx,s))return 1;
    s->key_entries[0].plan=saved;
    if(fprintf(ctx->report,"NATIVE LIFETIME owned_schema_snapshot_source_mutated_and_freed=1 live_destroyed_control_rejected=1 no_dangling_control_used=1\n")<0)return 1;
    return 0;
}
static int unchanged(struct native_context *ctx,const struct native_storage *s)
{
    ctx->name="original_catalog_metadata_preserved";++ctx->cases;
    const struct data_packet_group_definition *ng=native_abi_network_group(),*kg=native_abi_key_group();
    CHECK(memcmp(ng,&s->groups_before[0],sizeof(*ng))==0 && memcmp(kg,&s->groups_before[1],sizeof(*kg))==0,"original_group_limits_pointers_names_preserved");
    CHECK(memcmp(ng->packets,s->entries_before,35*sizeof(*ng->packets))==0 && memcmp(kg->packets,s->entries_before+35,2*sizeof(*kg->packets))==0,"original_entry_order_class_flags_definition_pointers_preserved");
    for(size_t i=0;i<NATIVE_ROWS;++i){const struct native_abi_catalog *r=catalog_row(i);
        CHECK(memcmp(r->definition,&s->definitions_before[i],sizeof(*r->definition))==0,"original_definition_version_size_flags_pointer_cache_preserved");
        CHECK(memcmp(r->definition->fields,s->fields_before[i],r->field_bound*sizeof(*r->definition->fields))==0,"all_original_field_counts_gates_cache_sizes_preserved");
    }
    return 0;
}
int wii_native_packet_fixture(FILE *report,int collect)
{
    struct native_context context={report,"initialization",0,0,0,0,collect};struct native_context *ctx=&context;
    struct native_storage *s=calloc(1,sizeof(*s));int aborted=0;
    if(!report){free(s);return 1;}
    if(!s){native_abort(ctx,"fixture_storage_allocation_failed");aborted=1;goto done;}
    if(layouts(ctx) || catalogs(ctx,s)){aborted=1;goto done;}
    for(size_t i=0;i<sizeof(shapes)/sizeof(shapes[0]);++i){const struct native_shape *shape=&shapes[i];
        if(shape->maximum){
            if(real_vector(ctx,s,shape,0) || real_vector(ctx,s,shape,2) || real_vector(ctx,s,shape,(short)shape->maximum) || real_arrays(ctx,s,shape)){aborted=1;goto done;}
        }else if(real_vector(ctx,s,shape,0)){aborted=1;goto done;}
    }
    if(lifetime(ctx,s) || unchanged(ctx,s))aborted=1;
done:
    if(s){
        for(size_t i=0;i<NATIVE_ROWS;++i){packet_array_destroy(&s->plans[i]);native_packet_binding_destroy(&s->bindings[i]);}
        native_packet_binding_destroy(&s->lifetime);free(s->temporary_fields);free(s->temporary_definition);free(s);
    }
    if(fprintf(report,"NATIVE SUMMARY cases=%u checks=%u failures=%u aborted=%d gaps=%u qualification=BLOCKED scope=isolated_actual_layout_and_inert_schema_representation\n",ctx->cases,ctx->checks,ctx->failures,aborted,ctx->gaps)<0 || fflush(report)!=0)return 1;
    return 1; /* Known compatibility gaps are expected evidence, never qualification. */
}
