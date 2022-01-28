#include <julia.h>

#include "sop_juila.h"

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
#include <GA/GA_Handle.h>
#include <GA/GA_AttributeFilter.h>
#include <GA/GA_AttributeDict.h>
#include <GA/GA_AIFTuple.h>


static std::ostream& debug(){
    return std::cout;
}

void newSopOperator(OP_OperatorTable *table){
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

size_t SOP_julia::instance_count=0;
bool SOP_julia::jl_initialized=false;

SOP_julia::SOP_julia(OP_Network *net, const char *name, OP_Operator *op):SOP_Node(net, name, op){
    if(!jl_initialized){
        jl_options.handle_signals = JL_OPTIONS_HANDLE_SIGNALS_OFF;
        jl_init();
        UT_Exit::addExitCallback(SOP_julia::atExit);
        jl_initialized=true;
    }
    ++instance_count;    
}

SOP_julia::~SOP_julia(){
    --instance_count;
    debug()<<"shark"<<std::endl;
    // TODO: delete created functions from main module maybe? is it even possible?
}

void SOP_julia::atExit(void*){
    if(SOP_julia::jl_initialized){
        debug()<<"running julia exit hooks"<<std::endl;
        jl_atexit_hook(0);
    }
}

typedef struct _entryAIFtuple{
    GA_StorageClass type;
    std::vector<double>* buffer_f64;
    std::vector<int64>* buffer_i64;
    GA_Attribute* attr;
    int tuple_size;
} entryAIFtuple;

static std::map<GA_StorageClass, const char*> h2jFloatTypeMapping = {
        {GA_StorageClass::GA_STORECLASS_FLOAT, "Array{Float64,2}"},
        {GA_StorageClass::GA_STORECLASS_INT, "Array{Int64,2}"}
    };

OP_ERROR SOP_julia::cookMySop(OP_Context &context){
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

    std::string codeFuncAttrs, compileTypes;
    codeFuncAttrs.reserve(256); //   cuz lazy
    compileTypes.reserve(256);  //        why not  TODO: no point recreating every cook - keep in class instance
    std::vector<entryAIFtuple> r_bind_entries3f, w_bind_entries3f;
    for(GA_AttributeDict::iterator it=gdp->getAttributeDict(GA_ATTRIB_POINT).begin(GA_SCOPE_PUBLIC);
                                   !it.atEnd();
                                   ++it){
        if(!rattribs_filter.match(it.attrib()))continue;
        GA_Attribute *attr = it.attrib();
        // for now only float based attribs
        const GA_StorageClass attrClass = attr->getStorageClass();
        if(attrClass!=GA_StorageClass::GA_STORECLASS_FLOAT &&
           attrClass!=GA_StorageClass::GA_STORECLASS_INT)continue;

        //if(attr->getTupleSize()==3){  // vector3 and shit
        if(attr->getAIFTuple()){
            const UT_String attr_name = UT_String(attr->getName());
            if(attrClass==GA_StorageClass::GA_STORECLASS_FLOAT && cachedBuffersf64.find(attr_name)==cachedBuffersf64.end())
                cachedBuffersf64[attr_name] = std::vector<double>();
            else if(attrClass==GA_StorageClass::GA_STORECLASS_INT && cachedBuffersi64.find(attr_name)==cachedBuffersi64.end())
                cachedBuffersi64[attr_name] = std::vector<int64>();

            //cachedBuffers[attr_name].resize(gdp->getNumPoints()*3);  // ensure size
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
                                        attr->getTupleSize()
                                        });
            if(wattribs_filter.match(attr))
                w_bind_entries3f.push_back({attrClass,
                                            attrClass==GA_StorageClass::GA_STORECLASS_FLOAT?&cachedBuffersf64[attr_name]:NULL,
                                            attrClass==GA_StorageClass::GA_STORECLASS_INT?&cachedBuffersi64[attr_name]:NULL,
                                            attr,
                                            attr->getTupleSize()
                                            });
            
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
        debug()<<spareParmTemp->getToken()<<"::"<<spareParmTemp->getLabel()<<"||"<<spareParmTemp->getType()<<std::endl;
    }

    // store attrib data in vectors
    for(entryAIFtuple& entry: r_bind_entries3f){
        switch(entry.type){
            case GA_StorageClass::GA_STORECLASS_FLOAT:
                entry.attr->getAIFTuple()->getRangeInContainer(entry.attr, gdp->getPointRange(), *entry.buffer_f64);
                break;
            case GA_StorageClass::GA_STORECLASS_INT:
                entry.attr->getAIFTuple()->getRangeInContainer(entry.attr, gdp->getPointRange(), *entry.buffer_i64);
                break;
            default:
                addError(SOP_MESSAGE, "internal type binding error!");
                return error();
        }
    }

    // init julia function
    //jl_gc_enable(0);
    UT_String nodeFuncName;
    getFullPath(nodeFuncName);
    nodeFuncName.substitute('/', '_');
    const bool updatingDefinitions = prevCode!=code || prevInitCode!=initcode || prevFuncName!=nodeFuncName || codeFuncAttrs!=prevAttrs;
    const bool needNoGcRun = updatingDefinitions && ( code.findString("Threads.", false, false) || initcode.findString("Threads.", false, false));  // CHEATS !!!
    if(updatingDefinitions){
        prevCode = code;
        prevInitCode = initcode;
        prevFuncName = nodeFuncName;
        prevAttrs = codeFuncAttrs;
        prevCode.hardenIfNeeded();
        prevInitCode.hardenIfNeeded();
        prevFuncName.hardenIfNeeded();

        UT_String signature;
        signature.sprintf("module %s\n%s\nfunction _hou_do_my_stuff(%s)\n", nodeFuncName.c_str(), initcode.c_str(), codeFuncAttrs.c_str());
        code.prepend(signature);
        code.append("\nend\nend");
        debug()<<"applying new julia module"<<std::endl<<code<<std::endl;
        jl_value_t *ret = jl_eval_string(code.c_str());
        if(ret==NULL){
            if(jl_exception_occurred()){
                jl_value_t *exc = jl_exception_occurred();
                jl_value_t *sprint_fun = jl_get_function(jl_base_module, "sprint");
                jl_value_t *showerror_fun = jl_get_function(jl_base_module, "showerror");
            
                const char* exc_details = jl_string_ptr(jl_call2(sprint_fun, showerror_fun, exc));
                addError(SOP_MESSAGE, exc_details);
            }else
                addError(SOP_MESSAGE, "something went wrong");
            return error();
        }

        UT_String precomp;
        precomp.sprintf("precompile(%s._hou_do_my_stuff, (%s))", nodeFuncName.c_str(), compileTypes.c_str());
        debug()<<"precompiling "<<precomp<<std::endl;
        ret = jl_eval_string(precomp.c_str());
        if(ret!=NULL) debug()<<jl_unbox_bool(ret)<<std::endl;
        debug()<<"precompiling done"<<std::endl;
    }
    if(needNoGcRun){
        // even with precompile julia has observed ~75% to crash on first execution of threaded code AFTER executing certain amount of nonthreaded code
        // hard to understand, hard to pinpoint wtf is happening, i'm blaming jit+gc+houdini's threads, 
        // cuz it crashes definetely somewhere during compilation and garbage collection in it's own julia threads...
        // anyway, disabling gc for this moment of first compilation seem to fix the observed issues
        // hence here it is
        //debug()<<"first run with no GC"<<std::endl;
        //jl_gc_enable(0);
    }

    // init jl variables
    std::vector<jl_value_t*> jl_values;
    // reference from here: https://discourse.julialang.org/t/api-reference-for-julia-embedding-in-c/3963/3
    jl_value_t *array_type2d = jl_apply_array_type((jl_value_t*)jl_float64_type, 2);
    jl_value_t *array_type2l = jl_apply_array_type((jl_value_t*)jl_int64_type, 2);
    jl_value_t *t2types[] = {(jl_value_t*)jl_long_type, (jl_value_t*)jl_long_type};
    jl_tupletype_t *t2t = jl_apply_tuple_type_v(t2types, 2);

    // vector3 attributes
    //((ssize_t*)v2size)[0] = 3;
    for(entryAIFtuple& entry: r_bind_entries3f){
        jl_value_t* v2size = jl_new_struct_uninit(t2t);
        ((ssize_t*)v2size)[1] = gdp->getNumPoints();
        ((ssize_t*)v2size)[0] = entry.tuple_size;
        switch(entry.type){
            case GA_StorageClass::GA_STORECLASS_FLOAT:
                jl_values.push_back((jl_value_t*)jl_ptr_to_array(array_type2d, entry.buffer_f64->data(), v2size, 0));
                break;
            case GA_StorageClass::GA_STORECLASS_INT:
                jl_values.push_back((jl_value_t*)jl_ptr_to_array(array_type2l, entry.buffer_i64->data(), v2size, 0));
                break;
            default:
                addError(SOP_MESSAGE, "internal type binding error!");
                return error();
        }
    }
    if(dotime){
        jl_values.push_back(jl_box_float64(curtime));
    }


    jl_function_t *jfunc = jl_get_function((jl_module_t*)jl_get_global(jl_main_module, jl_symbol(nodeFuncName.c_str())), "_hou_do_my_stuff");
    if(jfunc==NULL){
        addError(SOP_MESSAGE, "couldn't get da function");
        return error();
    }
    // ROOTing is not needed cuz this is exactly what jl_call does itself - see jlapi.c
    jl_gc_enable(1);
    jl_call(jfunc, jl_values.data(), jl_values.size());

    if(needNoGcRun){
        //jl_gc_enable(1);
        //debug()<<"no gc run: running gc manually"<<std::endl;
        //jl_gc_collect(JL_GC_FULL);
    }
    
    // note: we dont care aboug GC here, as even if arrays were collected - we do not reuse them, and we own the buffers.
    if(jl_exception_occurred()){
        jl_value_t *exc = jl_exception_occurred();
        jl_value_t *sprint_fun = jl_get_function(jl_base_module, "sprint");
        jl_value_t *showerror_fun = jl_get_function(jl_base_module, "showerror");
    
        const char* exc_details = jl_string_ptr(jl_call2(sprint_fun, showerror_fun, exc));
        addError(SOP_MESSAGE, exc_details);
        return error();
    }

    // save results back to dgp
    for(entryAIFtuple& entry: w_bind_entries3f){  // TODO: check if buffer was not resized somehow!
        switch(entry.type){
            case GA_StorageClass::GA_STORECLASS_FLOAT:
                entry.attr->getAIFTuple()->setRange(entry.attr, gdp->getPointRange(), entry.buffer_f64->data());
                break;
            case GA_StorageClass::GA_STORECLASS_INT:
                entry.attr->getAIFTuple()->setRange(entry.attr, gdp->getPointRange(), entry.buffer_i64->data());
                break;
            default:
                addError(SOP_MESSAGE, "internal type binding error!");
                return error();
        }
    }
    // --

    return error();
}
