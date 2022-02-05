#include <julia.h>

#include "sop_julia.h"

#include <iostream>
#include <OP/OP_Operator.h>
#include <OP/OP_AutoLockInputs.h>
#include <OP/OP_OperatorTable.h>
#include <PRM/PRM_Include.h>
#include <PRM/PRM_SpareData.h>
#include <UT/UT_String.h>
#include <UT/UT_DSOVersion.h>
#include <UT/UT_Exit.h>
#include <UT/UT_Thread.h>
#include <UT/UT_Signal.h>
#include <GA/GA_Handle.h>
#include <GA/GA_AttributeFilter.h>
#include <GA/GA_AttributeDict.h>
#include <GA/GA_AIFTuple.h>

using namespace std;

class NullBuffer : public streambuf
{
public:
  int overflow(int c) { return c; }
};
class NullStream : public ostream {
public:
    NullStream() : ostream(&m_sb) {};
private:
    NullBuffer m_sb;
};
static NullStream null_stream;

static ostream *debug_stream = &null_stream;
static ostream &debug(){
    return *debug_stream;
}

void newSopOperator(OP_OperatorTable *table){
    const char *debug_env = getenv("YURIA_DEBUG");
    if(debug_env != NULL){
        debug_stream = &cout;
    }

    table->addOperator(new OP_Operator(
        "juliasnippet",
        "Julia Snippet",
        SOP_julia::make_me,
        SOP_julia::parmtemplates,
        1, 1
    ));
}

static PRM_Name prm_snippet_name=PRM_Name("snippet", "Code");
static PRM_Name prm_initsnippet_name=PRM_Name("initsnippet", "Initialization Code");
static PRM_Name prm_rattribs_name=PRM_Name("rattribs", "Attributes To Bind For Reading");
static PRM_Name prm_wattribs_name=PRM_Name("wattribs", "Attributes To Bind For Writing");
static PRM_Name prm_include_time_name=PRM_Name("dotime", "Include Time Binding");
static PRM_Default prm_rattribs_default = PRM_Default(0, "P Cd");
static PRM_Default prm_wattribs_default = PRM_Default(0, "*");
static PRM_SpareData prm_snippet_spare(PRM_SpareToken("editor", "1"));
static PRM_SpareData prm_initsnippet_spare(PRM_SpareToken("editor", "1"));


PRM_Template SOP_julia::parmtemplates[] = {
    PRM_Template(PRM_STRING, 1, &prm_initsnippet_name, 0, 0, 0, 0, &prm_initsnippet_spare),
    PRM_Template(PRM_STRING, 1, &prm_snippet_name, 0, 0, 0, 0, &prm_snippet_spare),
    PRM_Template(PRM_TOGGLE, 1, &prm_include_time_name),
    PRM_Template(PRM_STRING, 1, &prm_rattribs_name, &prm_rattribs_default),
    PRM_Template(PRM_STRING, 1, &prm_wattribs_name, &prm_wattribs_default),
    PRM_Template()
};

OP_Node* SOP_julia::make_me(OP_Network *net, const char *name, OP_Operator *op){
    return new SOP_julia(net, name, op);
}

void signal_ignorer(UTsignalHandlerArg){
    debug()<<"SIGSEGV LE CAUGHT !!"<<endl;
};

size_t SOP_julia::instance_count = 0;
bool SOP_julia::jl_initialized = false;
unique_ptr<thread> SOP_julia::julia_thread = nullptr;
julia_thread_input_data_t *SOP_julia::julia_thread_input_data = NULL;
bool SOP_julia::time_to_stop_julia_thread = false;
std::mutex SOP_julia::jt_input_ready_mutex;
std::condition_variable SOP_julia::jt_input_ready;
std::mutex SOP_julia::julia_access_mutex;

SOP_julia::SOP_julia(OP_Network *net, const char *name, OP_Operator *op):SOP_Node(net, name, op){
    ++instance_count;    
}

SOP_julia::~SOP_julia(){
    --instance_count;
    debug()<<"shark"<<endl;
    // TODO: delete created functions from main module maybe? is it even possible?
}

void SOP_julia::atExit(void*){
    if(julia_thread){
        unique_lock<mutex> access_lock(julia_access_mutex);
        debug()<<"stopping julia thread"<<endl;
        {
            lock_guard<mutex> data_lock(jt_input_ready_mutex);
            time_to_stop_julia_thread = true;
        }
        jt_input_ready.notify_one();
        
        debug()<<"waiting for julia thread to join"<<endl;
        julia_thread->join();
        debug()<<"julia thread has joined!"<<endl;
    }
}

typedef struct _entryAIFtuple{
    GA_StorageClass type;
    vector<double>* buffer_f64;
    vector<int64>* buffer_i64;
    GA_Attribute* attr;
    int tuple_size;
    GA_Size buffer_elem_count;
    GA_Range buffer_range;
} entryAIFtuple;

static map<GA_StorageClass, const char*> h2jFloatTypeMapping = {
        {GA_StorageClass::GA_STORECLASS_FLOAT, "Array{Float64,2}"},
        {GA_StorageClass::GA_STORECLASS_INT, "Array{Int64,2}"}
    };


enum JULIA_THREAD_PROCESSING_PROBLEMS{
    JULIA_THREAD_PROCESSING_ALLGOOD,
    JULIA_THREAD_PROCESSING_SIMPLE_ERROR
};

typedef struct julia_thread_input_data_t{
    const bool updating_definitions;
    const UT_String &nodeFuncName, &initcode, &code;
    const string &codeFuncAttrs, &compileTypes;
    vector<entryAIFtuple> &r_bind_entries3f;
    const bool do_time;
    const double time=0.0;

    bool done=false;
    JULIA_THREAD_PROCESSING_PROBLEMS processing_errors=JULIA_THREAD_PROCESSING_ALLGOOD;
    string error_text;

    mutex datamod_mutex;
    condition_variable notifier;
} julia_thread_input_data_t;

static struct sigaction julia_sigsegv_action;

int SOP_julia::julia_inner_function(){
    // assume all is locked properly

     if(!jl_initialized){
        debug()<<"initializing julia in thread " << this_thread::get_id() << endl;
        jl_init();
        sigaction(SIGSEGV, NULL, &julia_sigsegv_action);
        //jl_eval_string("println(Threads.nthreads())");
        //jl_eval_string("x=[1]; @time Threads.@threads for i in 1:100 global x=hcat(x,size(rand(10000,1000))); end");
        jl_initialized=true;
    }
    sigaction(SIGSEGV, &julia_sigsegv_action, NULL);

    if(julia_thread_input_data->updating_definitions){
        UT_String signature;
        UT_String final_code = julia_thread_input_data->code;
        signature.sprintf("module %s\n%s\nfunction _hou_do_my_stuff(%s)\n", julia_thread_input_data->nodeFuncName.c_str(),
                                                                            julia_thread_input_data->initcode.c_str(),
                                                                            julia_thread_input_data->codeFuncAttrs.c_str());
        final_code.prepend(signature);
        final_code.append("\nend\nend");
        debug()<<"applying new julia module"<<endl<<final_code<<endl;
        jl_value_t *ret = jl_eval_string(final_code.c_str());
        if(ret==NULL){
            if(jl_exception_occurred()){
                jl_value_t *exc = jl_exception_occurred();
                jl_value_t *sprint_fun = jl_get_function(jl_base_module, "sprint");
                jl_value_t *showerror_fun = jl_get_function(jl_base_module, "showerror");
            
                const char* exc_details = jl_string_ptr(jl_call2(sprint_fun, showerror_fun, exc));
                julia_thread_input_data->error_text = exc_details;
            }else
                julia_thread_input_data->error_text = "something went wrong";
            return 1;
        }

        UT_String precomp;
        precomp.sprintf("precompile(%s._hou_do_my_stuff, (%s))", julia_thread_input_data->nodeFuncName.c_str(), julia_thread_input_data->compileTypes.c_str());
        debug()<<"precompiling "<<precomp<<endl;
        ret = jl_eval_string(precomp.c_str());
        if(ret!=NULL) debug()<<jl_unbox_bool(ret)<<endl;
        debug()<<"precompiling done"<<endl;
    }

    // init jl variables
    //jl_gc_enable(0);
    jl_value_t **gc_allvals;
    const size_t gc_allvals_count = julia_thread_input_data->r_bind_entries3f.size()*2 + julia_thread_input_data->do_time + 4;
    size_t gc_allvals_counter = 0;
    JL_GC_PUSHARGS(gc_allvals, gc_allvals_count);
    vector<jl_value_t*> jl_values;
    // reference from here: https://discourse.julialang.org/t/api-reference-for-julia-embedding-in-c/3963/3
    jl_value_t *array_type2d = jl_apply_array_type((jl_value_t*)jl_float64_type, 2);
    gc_allvals[gc_allvals_counter++] = array_type2d;
    jl_value_t *array_type2l = jl_apply_array_type((jl_value_t*)jl_int64_type, 2);
    gc_allvals[gc_allvals_counter++] = array_type2l;
    jl_value_t *t2types[] = {(jl_value_t*)jl_long_type, (jl_value_t*)jl_long_type};
    gc_allvals[gc_allvals_counter++] = t2types[0];
    gc_allvals[gc_allvals_counter++] = t2types[1];
    jl_tupletype_t *t2t = jl_apply_tuple_type_v(t2types, 2);

    // vector3 attributes
    //((ssize_t*)v2size)[0] = 3;
    for(entryAIFtuple& entry: julia_thread_input_data->r_bind_entries3f){
        jl_value_t* v2size = jl_new_struct_uninit(t2t);
        gc_allvals[gc_allvals_counter++] = v2size;
        ((ssize_t*)v2size)[1] = entry.buffer_elem_count;
        ((ssize_t*)v2size)[0] = entry.tuple_size;
        jl_value_t* val;
        switch(entry.type){
            case GA_StorageClass::GA_STORECLASS_FLOAT:
                val = (jl_value_t*)jl_ptr_to_array(array_type2d, entry.buffer_f64->data(), v2size, 0);
                jl_values.push_back(val);
                gc_allvals[gc_allvals_counter++] = val;
                break;
            case GA_StorageClass::GA_STORECLASS_INT:
                val = (jl_value_t*)jl_ptr_to_array(array_type2l, entry.buffer_i64->data(), v2size, 0);
                jl_values.push_back(val);
                gc_allvals[gc_allvals_counter++] = val;
                break;
            default:
                julia_thread_input_data->error_text = "internal type binding error!";
                return 1;
        }
    }
    if(julia_thread_input_data->do_time){
        jl_value_t* val = jl_box_float64(julia_thread_input_data->time);
        jl_values.push_back(val);
        gc_allvals[gc_allvals_counter++] = val;
    }


    jl_function_t *jfunc = jl_get_function((jl_module_t*)jl_get_global(jl_main_module, jl_symbol(julia_thread_input_data->nodeFuncName.c_str())), "_hou_do_my_stuff");
    if(jfunc==NULL){
        julia_thread_input_data->error_text = "couldn't get da function";
        return 1;
    }
    //jl_gc_enable(1);
    // ROOTing is not needed cuz this is exactly what jl_call does itself - see jlapi.c
    if(gc_allvals_count != gc_allvals_counter)debug()<<"GC ALLOC MISMATCH"<<gc_allvals_count<<"/"<<gc_allvals_counter<<endl;
    jl_call(jfunc, jl_values.data(), jl_values.size());
    JL_GC_POP();

    // note: we dont care aboug GC here, as even if arrays were collected - we do not reuse them, and we own the buffers.
    if(jl_exception_occurred()){
        jl_value_t *exc = jl_exception_occurred();
        jl_value_t *sprint_fun = jl_get_function(jl_base_module, "sprint");
        jl_value_t *showerror_fun = jl_get_function(jl_base_module, "showerror");
    
        const char* exc_details = jl_string_ptr(jl_call2(sprint_fun, showerror_fun, exc));
        julia_thread_input_data->error_text = exc_details;  // TODO: houdini only knows ascii, sanitize the string
        return 1;
    }
    return 0;
}


void SOP_julia::julia_dedicated_thread_func(){
    debug()<<"julia worker thread started in thread"<<endl;
    
    while(1){
        unique_lock<mutex> jt_input_lock(jt_input_ready_mutex);
        {
            jt_input_ready.wait(jt_input_lock, [&]{return julia_thread_input_data || time_to_stop_julia_thread;});
            if(time_to_stop_julia_thread)break;

            {
                lock_guard<mutex> data_lock(julia_thread_input_data->datamod_mutex);
                julia_thread_input_data->processing_errors = (julia_inner_function() == 0) ? JULIA_THREAD_PROCESSING_ALLGOOD : JULIA_THREAD_PROCESSING_SIMPLE_ERROR;
                julia_thread_input_data->done = true;
            }
            julia_thread_input_data->notifier.notify_all();
            julia_thread_input_data = NULL;
        }
    }

    if(jl_initialized){
        debug()<<"running julia exit hooks in thread "<<this_thread::get_id()<<endl;
        jl_atexit_hook(0);
    }
}


OP_ERROR SOP_julia::cookMySop(OP_Context &context){
    //UT_Signal sigsegv_lock(SIGSEGV, &signal_ignorer, true);
    //UT_Signal sigsegv_lock(SIGSEGV, SIG_DFL, true);
    //UT_Signal::disableCantReturn(true);
    if(!jl_initialized){
        time_to_stop_julia_thread = false;
        UT_Exit::addExitCallback(SOP_julia::atExit);
        julia_thread = make_unique<thread>(julia_dedicated_thread_func);
    }

    OP_AutoLockInputs inputlock(this);
    if(inputlock.lock(context) >= UT_ERROR_ABORT)
        return error();

    duplicateSource(0, context);
    UT_String initcode, code, rattribs_pattern, wattribs_pattern;
    fpreal curtime = context.getTime();
    evalString(code, prm_snippet_name.getTokenRef(), 0, curtime);
    evalString(initcode, prm_initsnippet_name.getTokenRef(), 0, curtime);
    evalString(rattribs_pattern, prm_rattribs_name.getTokenRef(), 0, curtime);
    evalString(wattribs_pattern, prm_wattribs_name.getTokenRef(), 0, curtime);
    const bool dotime = evalInt(prm_include_time_name.getTokenRef(),0, curtime);
    GA_AttributeFilter rattribs_filter(GA_AttributeFilter::selectByPattern(rattribs_pattern));
    GA_AttributeFilter wattribs_filter(GA_AttributeFilter::selectByPattern(wattribs_pattern));

    string codeFuncAttrs, compileTypes;
    codeFuncAttrs.reserve(256); //   cuz lazy
    compileTypes.reserve(256);  //        why not  TODO: no point recreating every cook - keep in class instance
    vector<entryAIFtuple> r_bind_entries3f, w_bind_entries3f;
    {
        set<uint32> already_used;
        const map<GA_AttributeOwner, GA_Size> owner_to_size = {{GA_ATTRIB_VERTEX, gdp->getNumVertices()}, 
                                                               {GA_ATTRIB_POINT, gdp->getNumPoints()},
                                                               {GA_ATTRIB_PRIMITIVE, gdp->getNumPrimitives()},
                                                               {GA_ATTRIB_DETAIL, 1}};
        const map<GA_AttributeOwner, GA_Range> owner_to_range = {{GA_ATTRIB_VERTEX, gdp->getVertexRange()}, 
                                                                 {GA_ATTRIB_POINT, gdp->getPointRange()},
                                                                 {GA_ATTRIB_PRIMITIVE, gdp->getPrimitiveRange()},
                                                                 {GA_ATTRIB_DETAIL, gdp->getGlobalRange()}};                                                       
        for(const auto &owner_iter: owner_to_size){
            for(GA_AttributeDict::iterator it=gdp->getAttributeDict(owner_iter.first).begin(GA_SCOPE_PUBLIC);
                                        !it.atEnd();
                                        ++it){
                if(!rattribs_filter.match(it.attrib()))continue;

                GA_Attribute *attr = it.attrib();

                // for now only float based attribs
                const GA_StorageClass attrClass = attr->getStorageClass();
                if(attrClass!=GA_StorageClass::GA_STORECLASS_FLOAT &&
                   attrClass!=GA_StorageClass::GA_STORECLASS_INT)continue;

                const UT_String attr_name = UT_String(attr->getName());
                uint32 attr_name_hash = attr_name.hash();
                if(already_used.find(attr_name_hash) != already_used.end())
                    continue;
                already_used.insert(attr_name_hash);

                if(attr->getAIFTuple()){
                    if(attrClass==GA_StorageClass::GA_STORECLASS_FLOAT && cachedBuffersf64.find(attr_name)==cachedBuffersf64.end())
                        cachedBuffersf64[attr_name] = vector<double>();
                    else if(attrClass==GA_StorageClass::GA_STORECLASS_INT && cachedBuffersi64.find(attr_name)==cachedBuffersi64.end())
                        cachedBuffersi64[attr_name] = vector<int64>();

                    if(codeFuncAttrs.length()>0){
                        codeFuncAttrs += ", ";
                        compileTypes += ", ";
                    }
                    codeFuncAttrs += attr_name;
                    codeFuncAttrs += "::";
                    codeFuncAttrs += h2jFloatTypeMapping[attrClass];
                    compileTypes += h2jFloatTypeMapping[attrClass];

                    r_bind_entries3f.push_back({attrClass,
                                                attrClass==GA_StorageClass::GA_STORECLASS_FLOAT?&cachedBuffersf64[attr_name]:NULL,
                                                attrClass==GA_StorageClass::GA_STORECLASS_INT?&cachedBuffersi64[attr_name]:NULL,
                                                attr,
                                                attr->getTupleSize(),
                                                owner_iter.second,
                                                owner_to_range.at(owner_iter.first)
                                                });
                    if(wattribs_filter.match(attr))
                        w_bind_entries3f.push_back({attrClass,
                                                    attrClass==GA_StorageClass::GA_STORECLASS_FLOAT?&cachedBuffersf64[attr_name]:NULL,
                                                    attrClass==GA_StorageClass::GA_STORECLASS_INT?&cachedBuffersi64[attr_name]:NULL,
                                                    attr,
                                                    attr->getTupleSize(),
                                                    owner_iter.second,
                                                    owner_to_range.at(owner_iter.first)
                                                    });
                    
                }
            }
        }
    }
    if(codeFuncAttrs.length() == 0){
        addWarning(SOP_MESSAGE, "no attributes binded, code not executed");
        return error();
    }
    if(dotime){
        codeFuncAttrs += ", Time::Float64";
        compileTypes += ", Float64";
        flags().setTimeDep(true);
    }

    for(const PRM_Template* spareParmTemp=getSpareParmTemplates(); spareParmTemp!=NULL && spareParmTemp->getType()!=PRM_LIST_TERMINATOR; ++spareParmTemp){
        debug()<<spareParmTemp->getToken()<<"::"<<spareParmTemp->getLabel()<<"||"<<spareParmTemp->getType()<<endl;
    }

    // store attrib data in vectors
    for(entryAIFtuple& entry: r_bind_entries3f){
        switch(entry.type){
            case GA_StorageClass::GA_STORECLASS_FLOAT:
                entry.attr->getAIFTuple()->getRangeInContainer(entry.attr, entry.buffer_range, *entry.buffer_f64);
                break;
            case GA_StorageClass::GA_STORECLASS_INT:
                entry.attr->getAIFTuple()->getRangeInContainer(entry.attr, entry.buffer_range, *entry.buffer_i64);
                break;
            default:
                addError(SOP_MESSAGE, "internal type binding error!");
                return error();
        }
    }

    // init julia function
    UT_String nodeFuncName;
    getFullPath(nodeFuncName);
    nodeFuncName.substitute('/', '_');
    const bool updatingDefinitions = prevCode!=code || prevInitCode!=initcode || prevFuncName!=nodeFuncName || codeFuncAttrs!=prevAttrs;
    if(updatingDefinitions){
        prevCode = code;
        prevInitCode = initcode;
        prevFuncName = nodeFuncName;
        prevAttrs = codeFuncAttrs;
        prevCode.hardenIfNeeded();
        prevInitCode.hardenIfNeeded();
        prevFuncName.hardenIfNeeded();
    }
    
    ///////////////////////////////////////////////////////////////
    
    julia_thread_input_data_t my_jdata{.updating_definitions = updatingDefinitions,
                                    .nodeFuncName = nodeFuncName,
                                    .initcode = initcode,
                                    .code = code,
                                    .codeFuncAttrs = codeFuncAttrs,
                                    .compileTypes = compileTypes,
                                    .r_bind_entries3f = r_bind_entries3f,
                                    .do_time = dotime,
                                    .time = context.getTime()
                                    };
    {   // access julia thread in a locked section
        unique_lock<mutex> access_lock(julia_access_mutex);
        {
            lock_guard<mutex> input_lock(jt_input_ready_mutex);
            julia_thread_input_data = &my_jdata;
        }
        jt_input_ready.notify_one();
        {
            unique_lock<mutex> result_lock(my_jdata.datamod_mutex);
            my_jdata.notifier.wait(result_lock, [&]{return my_jdata.done;});
        }
    }


    if(my_jdata.processing_errors == JULIA_THREAD_PROCESSING_SIMPLE_ERROR){
        addError(SOP_MESSAGE, my_jdata.error_text.c_str());
        return error();
    }

    // save results back to dgp
    for(entryAIFtuple& entry: w_bind_entries3f){  // TODO: check if buffer was not resized somehow!
        switch(entry.type){
            case GA_StorageClass::GA_STORECLASS_FLOAT:
                entry.attr->getAIFTuple()->setRange(entry.attr, entry.buffer_range, entry.buffer_f64->data());
                break;
            case GA_StorageClass::GA_STORECLASS_INT:
                entry.attr->getAIFTuple()->setRange(entry.attr, entry.buffer_range, entry.buffer_i64->data());
                break;
            default:
                addError(SOP_MESSAGE, "internal type binding error!");
                return error();
        }
    }
    // --

    return error();
}
