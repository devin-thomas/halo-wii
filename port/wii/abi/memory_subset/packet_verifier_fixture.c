#include "packet_verifier_policy.h"
#include "packet_verifier_fixture.h"
#include <limits.h>

enum { TABLE_CAPACITY=16, NULL_DEFINITION=1, MISSING_NAME=2, MISSING_FIELDS=4 };
struct guarded_fields {
    struct data_packet_field before[2], fields[TABLE_CAPACITY], after[2];
};
struct verifier_context { FILE *report; const char *name; unsigned int checks, failures, cases; int collect; };
static const char *const expected_errors[]={
    "ok", "null_definition", "negative_native_size", "version_outside_byte_range",
    "missing_name", "missing_fields", "invalid_field_bound", "missing_end_within_bound",
    "invalid_field_type", "nonpositive_field_count", "invalid_version_gate",
    "unsupported_array_schema", "field_native_extent_overflow", "total_native_extent_overflow",
    "declared_native_size_mismatch"
};
#define FIELD(type,count,min,max) {type,count,min,max,113}
#define END_FIELD {_data_packet_field_end,-7,-8,-9,117}
static int verifier_check(struct verifier_context *ctx,int condition,const char *assertion)
{
    ++ctx->checks;
    if(condition) return 1;
    ++ctx->failures;
    if(fprintf(ctx->report,"VERIFIER FAIL case=%s assertion=%s check=%u\n",ctx->name,assertion,ctx->checks)<0 ||
       fflush(ctx->report)!=0) return 0;
    return ctx->collect;
}
#define CHECK(condition,name) do {if(!verifier_check(ctx,(condition),name)) return 1;} while(0)

static void fill_guarded(struct guarded_fields *storage)
{
    const struct data_packet_field sentinel={-12,-13,-14,-15,0x5555};
    for(unsigned int n=0;n<2;++n) {storage->before[n]=sentinel;storage->after[n]=sentinel;}
    for(unsigned int n=0;n<TABLE_CAPACITY;++n) storage->fields[n]=sentinel;
}
static void fill_definition(struct data_packet_definition *definition,struct data_packet_field *fields,
                            short size,short version,boolean cached,unsigned int flags)
{
    /* Initialize padding too, for full object-representation transaction checks. */
    memset(definition,0xa5,sizeof(*definition));
    definition->name=flags&MISSING_NAME ? NULL : "authored_verifier_metadata";
    definition->flags=0x1234; definition->size=size; definition->version=version;
    definition->fields=flags&MISSING_FIELDS ? NULL : fields; definition->initialized=cached;
}

static int run_schema(struct verifier_context *ctx,const char *name,
                      const struct data_packet_field *fields,size_t count,size_t bound,
                      short size,short version,boolean cached,unsigned int flags,
                      enum packet_verifier_error error,size_t index,size_t consumed,
                      uint32_t total,uint32_t extent,const short *sizes,int baseline)
{
    struct guarded_fields actual,expected,original;
    struct data_packet_definition definition,definition_expected,reference;
    struct packet_verifier_result result;
    ctx->name=name; ++ctx->cases;
    fill_guarded(&actual); memcpy(actual.fields,fields,count*sizeof(*fields));
    memcpy(&expected,&actual,sizeof(expected));
    fill_definition(&definition,actual.fields,size,version,cached,flags);
    memcpy(&definition_expected,&definition,sizeof(definition_expected));
    if(error==PACKET_VERIFY_OK) {
        for(size_t n=0;n<index;++n) expected.fields[n].size=sizes[n];
        definition_expected.initialized=TRUE;
    }
    boolean accepted=packet_verifier_diagnostic(flags&NULL_DEFINITION ? NULL : &definition,bound,&result);
    const char *error_name=packet_verifier_error_name(result.error);
    if(fprintf(ctx->report,
               "VERIFIER CASE name=%s accepted=%u error=%s index=%zu consumed=%zu native_extent=%u field_extent=%u baseline=%s\n",
               name,(unsigned int)accepted,error_name,result.field_index,result.fields_consumed,
               (unsigned int)result.native_extent,(unsigned int)result.field_extent,
               baseline ? "actual_flat_executed" : "not_executed")<0 || fflush(ctx->report)!=0) return 1;
    CHECK(accepted==(error==PACKET_VERIFY_OK),"acceptance");
    CHECK(result.error==error && result.field_index==index && result.fields_consumed==consumed &&
          result.native_extent==total && result.field_extent==extent,"structured_reason_index_extents");
    CHECK(error_name && strcmp(error_name,expected_errors[error])==0,"structured_error_name");
    CHECK(memcmp(&definition,&definition_expected,sizeof(definition))==0,"whole_definition_transaction");
    CHECK(memcmp(&actual,&expected,sizeof(actual))==0,"whole_schema_guards_and_metadata_transaction");
    if(baseline) {
        fill_guarded(&original); memcpy(original.fields,fields,count*sizeof(*fields));
        fill_definition(&reference,original.fields,size,version,cached,0);
        candidate_packet_verify(&reference);
        CHECK(reference.initialized==definition.initialized &&
              memcmp(&original,&actual,sizeof(original))==0,"actual_flat_verifier_metadata_equivalence");
    }
    return 0;
}

static int good(struct verifier_context *ctx,const char *name,const struct data_packet_field *fields,
                size_t count,short size,short version,const short *sizes,int baseline,boolean cached)
{
    return run_schema(ctx,name,fields,count,count,size,version,cached,0,
                      PACKET_VERIFY_OK,count-1,count,(uint32_t)size,0,sizes,baseline);
}
static int bad(struct verifier_context *ctx,const char *name,const struct data_packet_field *fields,
               size_t count,size_t bound,short size,short version,boolean cached,unsigned int flags,
               enum packet_verifier_error error,size_t index,size_t consumed,uint32_t total,uint32_t extent)
{
    return run_schema(ctx,name,fields,count,bound,size,version,cached,flags,error,index,consumed,total,extent,NULL,0);
}

static int flat_and_limits(struct verifier_context *ctx)
{
    static const struct {
        const char *name,*limit_name,*overflow_name;
        short type,count,extent,limit_count,limit_extent,over_count;
        uint32_t over_extent;
    } selectors[]={
        {"pad","pad_limit",NULL,_data_packet_field_pad,3,3,32767,32767,0,0},
        {"bytes","bytes_limit",NULL,_data_packet_field_bytes,3,3,32767,32767,0,0},
        {"shorts","shorts_limit","shorts_one_over",_data_packet_field_shorts,3,6,16383,32766,16384,32768},
        {"longs","longs_limit","longs_one_over",_data_packet_field_longs,3,12,8191,32764,8192,32768},
        {"int64s","int64s_limit","int64s_one_over",_data_packet_field_int64s,3,24,4095,32760,4096,32768},
        {"string","string_limit","string_one_over",_data_packet_field_string,4,5,32766,32767,32767,32768},
        {"data","data_limit","data_one_over",_data_packet_field_data,4,6,32765,32767,32766,32768},
        {"raw","raw_limit",NULL,_data_packet_field_raw,3,3,32767,32767,0,0}
    };
    for(unsigned int n=0;n<sizeof(selectors)/sizeof(selectors[0]);++n) {
        struct data_packet_field fields[]={FIELD(selectors[n].type,selectors[n].count,0,0),END_FIELD};
        short sizes[]={selectors[n].extent};
        if(good(ctx,selectors[n].name,fields,2,selectors[n].extent,1,sizes,1,FALSE)) return 1;
        fields[0].count=selectors[n].limit_count;sizes[0]=selectors[n].limit_extent;
        if(good(ctx,selectors[n].limit_name,fields,2,selectors[n].limit_extent,1,sizes,1,FALSE)) return 1;
        if(selectors[n].over_count) {
            fields[0].count=selectors[n].over_count;
            if(bad(ctx,selectors[n].overflow_name,fields,2,2,0,1,FALSE,0,
                   PACKET_VERIFY_FIELD_EXTENT,0,0,0,selectors[n].over_extent)) return 1;
        }
    }
    const struct data_packet_field mixed[]={
        FIELD(_data_packet_field_bytes,2,0,0),FIELD(_data_packet_field_shorts,2,0,0),
        FIELD(_data_packet_field_longs,2,0,0),FIELD(_data_packet_field_int64s,2,0,0),
        FIELD(_data_packet_field_raw,3,0,0),FIELD(_data_packet_field_string,4,0,0),
        FIELD(_data_packet_field_data,4,0,0),FIELD(_data_packet_field_pad,3,0,0),END_FIELD
    };
    const short mixed_sizes[]={2,4,8,16,3,5,6,3};
    if(good(ctx,"mixed_all_flat_selectors",mixed,9,47,1,mixed_sizes,1,FALSE)) return 1;
    const struct data_packet_field exact[]={FIELD(_data_packet_field_raw,32766,0,0),FIELD(_data_packet_field_bytes,1,0,0),END_FIELD};
    const short exact_sizes[]={32766,1};
    if(good(ctx,"total_exact_32767",exact,3,32767,1,exact_sizes,1,FALSE)) return 1;
    const struct data_packet_field overflow[]={FIELD(_data_packet_field_raw,32767,0,0),FIELD(_data_packet_field_bytes,1,0,0),END_FIELD};
    if(bad(ctx,"total_one_over_32767",overflow,3,3,32767,1,FALSE,0,
           PACKET_VERIFY_TOTAL_EXTENT,1,0,32767,1)) return 1;
    return 0;
}

static int gates_and_cache(struct verifier_context *ctx)
{
    struct data_packet_field fields[]={FIELD(_data_packet_field_bytes,3,2,2),END_FIELD};
    const short included[]={3},excluded[]={0};
    if(good(ctx,"gate_inclusive_same_endpoint",fields,2,3,2,included,1,FALSE)) return 1;
    fields[0].maximum_version=4;
    if(good(ctx,"gate_inclusive_minimum",fields,2,3,2,included,1,FALSE) ||
       good(ctx,"gate_inclusive_maximum",fields,2,3,4,included,1,FALSE) ||
       good(ctx,"gate_below_minimum",fields,2,0,1,excluded,0,FALSE) ||
       good(ctx,"gate_above_maximum",fields,2,0,5,excluded,0,FALSE)) return 1;
    fields[0].minimum_version=0;fields[0].maximum_version=0;
    if(good(ctx,"definition_version_zero",fields,2,3,0,included,1,FALSE) ||
       good(ctx,"maximum_zero_unbounded_version255",fields,2,3,255,included,1,FALSE)) return 1;
    fields[0].minimum_version=255;fields[0].maximum_version=255;
    if(good(ctx,"gate_endpoint255",fields,2,3,255,included,1,FALSE)) return 1;
    fields[0].size=3;
    if(good(ctx,"cached_initialized_correct",fields,2,3,255,included,1,TRUE)) return 1;
    fields[0].size=-123;
    if(good(ctx,"cached_negative_stored_size_recomputed",fields,2,3,255,included,0,TRUE)) return 1;
    fields[0].size=27;
    if(good(ctx,"cached_stale_stored_size_recomputed",fields,2,3,255,included,0,TRUE)) return 1;
    const struct data_packet_field multiple[]={
        FIELD(_data_packet_field_shorts,2,2,0),FIELD(_data_packet_field_longs,3,3,0),
        FIELD(_data_packet_field_raw,2,0,0),FIELD(_data_packet_field_string,4,2,0),END_FIELD
    };
    const short multiple_sizes[]={0,0,2,0};
    if(good(ctx,"first_and_multiple_excluded_deterministic_zero",multiple,5,2,1,multiple_sizes,0,FALSE)) return 1;
    const struct data_packet_field latent[]={FIELD(_data_packet_field_int64s,4096,2,0),END_FIELD};
    if(bad(ctx,"excluded_latent_extent_overflow",latent,2,2,0,1,FALSE,0,
           PACKET_VERIFY_FIELD_EXTENT,0,0,0,32768)) return 1;
    return 0;
}

static int rejected_inputs(struct verifier_context *ctx)
{
    struct data_packet_field fields[]={FIELD(_data_packet_field_bytes,1,0,0),END_FIELD};
    static const struct {const char *name;short size,version;unsigned int flags;size_t bound;enum packet_verifier_error error;} headers[]={
        {"null_definition",1,1,NULL_DEFINITION,2,PACKET_VERIFY_NULL_DEFINITION},
        {"negative_declared_size",-1,1,0,2,PACKET_VERIFY_NEGATIVE_SIZE},
        {"negative_definition_version",1,-1,0,2,PACKET_VERIFY_VERSION_RANGE},
        {"definition_version256",1,256,0,2,PACKET_VERIFY_VERSION_RANGE},
        {"missing_name",1,1,MISSING_NAME,2,PACKET_VERIFY_MISSING_NAME},
        {"missing_fields",1,1,MISSING_FIELDS,2,PACKET_VERIFY_MISSING_FIELDS},
        {"field_bound_zero",1,1,0,0,PACKET_VERIFY_FIELD_BOUND},
        {"field_bound_one_over",1,1,0,32768,PACKET_VERIFY_FIELD_BOUND}
    };
    for(unsigned int n=0;n<sizeof(headers)/sizeof(headers[0]);++n)
        if(bad(ctx,headers[n].name,fields,2,headers[n].bound,headers[n].size,headers[n].version,TRUE,
               headers[n].flags,headers[n].error,0,0,0,0)) return 1;
    if(bad(ctx,"missing_end_short_bound",fields,2,1,1,1,FALSE,0,PACKET_VERIFY_MISSING_END,1,0,1,1)) return 1;
    fields[1].type=_data_packet_field_raw;fields[1].count=2;fields[1].minimum_version=0;fields[1].maximum_version=0;
    if(bad(ctx,"missing_end_two_fields",fields,2,2,3,1,FALSE,0,PACKET_VERIFY_MISSING_END,2,0,3,2)) return 1;
    fields[1]=(struct data_packet_field)END_FIELD;
    fields[0].type=-1;
    if(bad(ctx,"negative_field_type",fields,2,2,0,1,FALSE,0,PACKET_VERIFY_FIELD_TYPE,0,0,0,0)) return 1;
    fields[0].type=_data_packet_field_type_count;
    if(bad(ctx,"field_type_past_enum",fields,2,2,0,1,FALSE,0,PACKET_VERIFY_FIELD_TYPE,0,0,0,0)) return 1;
    fields[0].type=_data_packet_field_bytes;fields[0].count=0;
    if(bad(ctx,"zero_field_count",fields,2,2,0,1,FALSE,0,PACKET_VERIFY_FIELD_COUNT,0,0,0,0)) return 1;
    fields[0].count=-1;
    if(bad(ctx,"negative_field_count",fields,2,2,0,1,FALSE,0,PACKET_VERIFY_FIELD_COUNT,0,0,0,0)) return 1;
    fields[0].minimum_version=2;
    if(bad(ctx,"excluded_negative_count",fields,2,2,0,1,FALSE,0,PACKET_VERIFY_FIELD_COUNT,0,0,0,0)) return 1;
    fields[0].count=1;fields[0].minimum_version=0;
    static const struct {const char *name;short min,max;} gates[]={
        {"negative_minimum_gate",-1,0},{"minimum_gate256",256,0},
        {"negative_maximum_gate",0,-1},{"maximum_gate256",0,256},{"reversed_gate",2,1}
    };
    for(unsigned int n=0;n<sizeof(gates)/sizeof(gates[0]);++n) {
        fields[0].minimum_version=gates[n].min;fields[0].maximum_version=gates[n].max;
        if(bad(ctx,gates[n].name,fields,2,2,1,1,FALSE,0,PACKET_VERIFY_FIELD_GATE,0,0,0,0)) return 1;
    }
    fields[0].minimum_version=0;fields[0].maximum_version=0;
    if(bad(ctx,"declared_size_too_small",fields,2,2,0,1,FALSE,0,PACKET_VERIFY_SIZE_MISMATCH,1,2,1,0) ||
       bad(ctx,"declared_size_too_large",fields,2,2,2,1,FALSE,0,PACKET_VERIFY_SIZE_MISMATCH,1,2,1,0) ||
       bad(ctx,"cached_invalid_declared_size",fields,2,2,2,1,TRUE,0,PACKET_VERIFY_SIZE_MISMATCH,1,2,1,0)) return 1;
    const struct data_packet_field late[]={FIELD(_data_packet_field_raw,3,0,0),FIELD(_data_packet_field_shorts,0,0,0),END_FIELD};
    if(bad(ctx,"late_error_prior_metadata_uncommitted",late,3,3,3,1,FALSE,0,PACKET_VERIFY_FIELD_COUNT,1,0,3,0)) return 1;
    return 0;
}

static int arrays_and_end(struct verifier_context *ctx)
{
    const struct data_packet_field arrays[][4]={
        {FIELD(_data_packet_field_array,2,0,0),FIELD(_data_packet_field_bytes,1,0,0),END_FIELD,END_FIELD},
        {FIELD(_data_packet_field_array,2,0,0),FIELD(-1,-1,-1,-1),END_FIELD,END_FIELD},
        {FIELD(_data_packet_field_array,2,0,0),END_FIELD,END_FIELD,END_FIELD},
        {FIELD(_data_packet_field_array,2,2,0),FIELD(-1,-1,-1,-1),END_FIELD,END_FIELD}
    };
    static const char *const names[]={"array_valid_children_unsupported","array_malformed_children_unread",
                                      "array_truncated_span_unread","excluded_array_unsupported"};
    for(unsigned int n=0;n<4;++n)
        if(bad(ctx,names[n],arrays[n],n==2 ? 1U : 4U,n==2 ? 1U : 4U,0,1,FALSE,0,
               PACKET_VERIFY_UNSUPPORTED_ARRAY,0,0,0,0)) return 1;
    const struct data_packet_field empty[]={END_FIELD,FIELD(-1,-1,-1,-1)};
    const short unused_sizes[]={0};
    /* END metadata and every unused suffix byte are ignored and preserved. */
    if(run_schema(ctx,"first_end_metadata_and_suffix_ignored",empty,2,2,0,0,FALSE,0,
                  PACKET_VERIFY_OK,0,1,0,0,unused_sizes,1) ||
       run_schema(ctx,"minimum_bound_one_end",empty,1,1,0,255,FALSE,0,
                  PACKET_VERIFY_OK,0,1,0,0,unused_sizes,1)) return 1;
    return 0;
}

static int original_stale_and_cache(struct verifier_context *ctx)
{
    const struct data_packet_field later[]={FIELD(_data_packet_field_raw,3,0,0),
                                            FIELD(_data_packet_field_shorts,2,2,0),
                                            FIELD(_data_packet_field_bytes,1,0,0),END_FIELD};
    struct guarded_fields original,expected;
    struct data_packet_definition definition,snapshot;
    fill_guarded(&original);memcpy(original.fields,later,sizeof(later));memcpy(&expected,&original,sizeof(expected));
    expected.fields[0].size=3;expected.fields[1].size=3;expected.fields[2].size=1;
    fill_definition(&definition,original.fields,7,1,FALSE,0);memcpy(&snapshot,&definition,sizeof(snapshot));snapshot.initialized=TRUE;
    ctx->name="original_later_excluded_defined_stale_size";++ctx->cases;
    candidate_packet_verify(&definition);
    if(fprintf(ctx->report,"VERIFIER CASE name=%s baseline=actual_executed native_extent=7 excluded_size=3 diagnostic_expected_extent=4\n",ctx->name)<0 || fflush(ctx->report)!=0) return 1;
    CHECK(memcmp(&original,&expected,sizeof(original))==0,"original_defined_stale_metadata_and_guards");
    CHECK(memcmp(&definition,&snapshot,sizeof(definition))==0,"original_defined_stale_definition");
    const short correct[]={3,0,1};
    if(good(ctx,"later_excluded_correct_diagnostic_size",later,4,4,1,correct,0,FALSE) ||
       bad(ctx,"later_excluded_stale_declared_size_rejected",later,4,4,7,1,FALSE,0,
           PACKET_VERIFY_SIZE_MISMATCH,3,4,4,0)) return 1;
    const struct data_packet_field invalid[]={FIELD(_data_packet_field_bytes,-1,0,0),END_FIELD};
    fill_guarded(&original);memcpy(original.fields,invalid,sizeof(invalid));memcpy(&expected,&original,sizeof(expected));
    fill_definition(&definition,original.fields,0,1,TRUE,0);memcpy(&snapshot,&definition,sizeof(snapshot));
    ctx->name="original_initialized_shortcut_skips_negative_count";++ctx->cases;
    candidate_packet_verify(&definition);
    if(fprintf(ctx->report,"VERIFIER CASE name=%s baseline=actual_executed initialized_shortcut=unchanged\n",ctx->name)<0 || fflush(ctx->report)!=0) return 1;
    CHECK(memcmp(&original,&expected,sizeof(original))==0 && memcmp(&definition,&snapshot,sizeof(definition))==0,
          "original_cached_invalid_schema_defined_noop");
    if(bad(ctx,"diagnostic_cached_negative_count_rejected",invalid,2,2,0,1,TRUE,0,
           PACKET_VERIFY_FIELD_COUNT,0,0,0,0)) return 1;
    return 0;
}

static int maximum_bound(struct verifier_context *ctx)
{
    size_t count=(size_t)SHRT_MAX+2;
    struct data_packet_field *actual=malloc(count*sizeof(*actual)),*snapshot=malloc(count*sizeof(*snapshot));
    struct data_packet_definition definition,expected;
    struct packet_verifier_result result;
    ctx->name="maximum_truthful_bound_32767";++ctx->cases;
    if(!actual || !snapshot) {
        free(actual);free(snapshot);
        if(fprintf(ctx->report,"VERIFIER ABORT case=%s reason=fixture_allocation_failed\n",ctx->name)<0) return 1;
        fflush(ctx->report);return 1;
    }
    const struct data_packet_field guard={-12,-13,-14,-15,0x5555};
    for(size_t n=0;n<count;++n) actual[n]=guard;
    actual[1]=(struct data_packet_field)END_FIELD;memcpy(snapshot,actual,count*sizeof(*actual));
    fill_definition(&definition,actual+1,0,1,FALSE,0);memcpy(&expected,&definition,sizeof(expected));expected.initialized=TRUE;
    boolean accepted=packet_verifier_diagnostic(&definition,(size_t)SHRT_MAX,&result);
    int allocation_case_ok=1;
    /* Free before returning, including a fail-fast assertion or report error. */
    if(fprintf(ctx->report,"VERIFIER CASE name=%s accepted=%u error=%s index=%zu consumed=%zu native_extent=%u field_extent=%u\n",
               ctx->name,(unsigned int)accepted,packet_verifier_error_name(result.error),result.field_index,result.fields_consumed,
               (unsigned int)result.native_extent,(unsigned int)result.field_extent)<0 || fflush(ctx->report)!=0) allocation_case_ok=0;
    if(allocation_case_ok) {
        const int conditions[]={accepted && result.error==PACKET_VERIFY_OK && result.field_index==0 && result.fields_consumed==1 && result.native_extent==0 && result.field_extent==0,
                                memcmp(&definition,&expected,sizeof(definition))==0,
                                memcmp(actual,snapshot,count*sizeof(*actual))==0};
        const char *const assertions[]={"maximum_bound_structured_success","maximum_bound_definition_commit","maximum_bound_unused_suffix_and_guards"};
        for(unsigned int n=0;n<3;++n)
            if(!verifier_check(ctx,conditions[n],assertions[n])) {allocation_case_ok=0;break;}
    }
    free(actual);free(snapshot);return !allocation_case_ok;
}

int wii_packet_verifier_fixture(FILE *report,int collect_failures)
{
    struct verifier_context context={report,"",0,0,0,collect_failures};
    struct verifier_context *ctx=&context;
    int aborted=0;
    if(fprintf(report,"VERIFIER BEGIN policy=bounded_flat_own_version_metadata_only arrays=unsupported\nVERIFIER BASELINE first_excluded_indeterminate_size=unexecuted byte_raw_pad_positive_count_ceiling=32767 one_over_positive_short=unrepresentable\n")<0 || fflush(report)!=0) return 1;
    if(flat_and_limits(ctx) || gates_and_cache(ctx) || rejected_inputs(ctx) || arrays_and_end(ctx) ||
       original_stale_and_cache(ctx) || maximum_bound(ctx)) aborted=1;
    if(fprintf(report,"VERIFIER SUMMARY cases=%u checks=%u failures=%u aborted=%d\nVERIFIER END result=%d\n",
               ctx->cases,ctx->checks,ctx->failures,aborted,aborted || ctx->failures!=0)<0 || fflush(report)!=0) return 1;
    return aborted || ctx->failures!=0;
}
#undef CHECK
#undef FIELD
#undef END_FIELD
