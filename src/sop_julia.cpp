
#include "sop_juila.h"

#include <iostream>
#include <OP/OP_Operator.h>
#include <OP/OP_AutoLockInputs.h>
#include <OP/OP_OperatorTable.h>
#include <PRM/PRM_Include.h>
#include <UT/UT_String.h>
#include <UT/UT_DSOVersion.h>
#include <UT/UT_Exit.h>
#include <GA/GA_Handle.h>
#include <GA/GA_AttributeFilter.h>
#include <GA/GA_AttributeDict.h>
#include <GA/GA_AIFTuple.h>

#include <julia.h>

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
static PRM_Name prm_rattribs_name=PRM_Name("rattribs", "Attributes To Bind For Reading");
static PRM_Name prm_wattribs_name=PRM_Name("wattribs", "Attributes To Bind For Writing");
static PRM_Default prm_rattribs_default = PRM_Default(0, "P Cd");
static PRM_Default prm_wattribs_default = PRM_Default(0, "*");


PRM_Template SOP_julia::parmtemplates[] = {
    PRM_Template(PRM_STRING, 1, &prm_snippet_name),
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
        jl_init();
        UT_Exit::addExitCallback(SOP_julia::atExit);
        jl_initialized=true;
    }
    ++instance_count;    
}

SOP_julia::~SOP_julia(){
    --instance_count;
    debug()<<"shark"<<std::endl;
    // TODO: delete created functions from main module maybe?
}

void SOP_julia::atExit(void*){
    if(SOP_julia::jl_initialized){
        debug()<<"running julia exit hooks"<<std::endl;
        jl_atexit_hook(0);
    }
}

typedef struct _entry3f{
    std::vector<double>* buffer;
    GA_Attribute* attr;
    int tuple_size;
} entry3f;

OP_ERROR SOP_julia::cookMySop(OP_Context &context){
    OP_AutoLockInputs inputlock(this);
    if(inputlock.lock(context) >= UT_ERROR_ABORT)
        return error();

    duplicateSource(0, context);
    UT_String code, rattribs_pattern, wattribs_pattern;
    evalString(code, prm_snippet_name.getToken(), 0, context.getTime());
    evalString(rattribs_pattern, prm_rattribs_name.getToken(), 0, context.getTime());
    evalString(wattribs_pattern, prm_wattribs_name.getToken(), 0, context.getTime());
    GA_AttributeFilter rattribs_filter(GA_AttributeFilter::selectByPattern(rattribs_pattern));
    GA_AttributeFilter wattribs_filter(GA_AttributeFilter::selectByPattern(wattribs_pattern));

    UT_String codeFuncAttrs;
    std::vector<entry3f> r_bind_entries3f, w_bind_entries3f;
    for(GA_AttributeDict::iterator it=gdp->getAttributeDict(GA_ATTRIB_POINT).begin(GA_SCOPE_PUBLIC);
                                   !it.atEnd();
                                   ++it){
        if(!rattribs_filter.match(it.attrib()))continue;
        GA_Attribute *attr = it.attrib();
        // for now only float based attribs
        if(attr->getStorageClass()!=GA_StorageClass::GA_STORECLASS_FLOAT)continue;

        //if(attr->getTupleSize()==3){  // vector3 and shit
        if(attr->getAIFTuple()){
            const UT_String attr_name = UT_String(attr->getName());
            if(cachedBuffers.find(attr_name)==cachedBuffers.end())
                cachedBuffers[attr_name] = std::vector<double>();

            //cachedBuffers[attr_name].resize(gdp->getNumPoints()*3);  // ensure size
            
            if(codeFuncAttrs.length()==0)codeFuncAttrs+=attr_name;
            else {
                codeFuncAttrs += ", ";
                codeFuncAttrs += attr_name;
            }

            r_bind_entries3f.push_back({&cachedBuffers[attr_name],
                                        attr,
                                        attr->getTupleSize()
                                        });
            if(wattribs_filter.match(attr))
                w_bind_entries3f.push_back({&cachedBuffers[attr_name],
                                            attr,
                                            attr->getTupleSize()
                                            });
        }
    }
    debug()<<codeFuncAttrs<<std::endl;

    /*
    GA_RWHandleV3 pos_handle(gdp, GA_ATTRIB_POINT, "P");
    /GA_RWHandleV3 cd_handle(gdp, GA_ATTRIB_POINT, "Cd");
    if(cachedBuffers.find("P")==cachedBuffers.end())
        cachedBuffers["P"] = std::vector<double>();
    if(cachedBuffers.find("Cd")==cachedBuffers.end())
        cachedBuffers["Cd"] = std::vector<double>();
    */
    /*
    std::vector<std::vector<double>*> used_buffers;
    used_buffers.resize(2);
    used_buffers[0]=&cachedBuffers["P"];
    used_buffers[1]=&cachedBuffers["Cd"];
    used_buffers[0]->resize(gdp->getNumPoints()*3);
    used_buffers[1]->resize(gdp->getNumPoints()*3);  //hm.....
    */

    // store attrib data in vectors
    for(entry3f& entry: r_bind_entries3f){
        entry.attr->getAIFTuple()->getRangeInContainer(entry.attr, gdp->getPointRange(), *entry.buffer);
    }
    /*
    GA_Offset block_start, block_end;
    size_t linid = 0;
    for(GA_Iterator it(gdp->getPointRange());it.blockAdvance(block_start, block_end);){
        for(GA_Offset off=block_start; off<block_end; ++off){
            // vector3 attributes
            for(entry3f& entry: r_bind_entries3f){
                UT_Vector3F val = entry.att_handle.get(off);
                for(int i=0;i<3;++i){
                    (*entry.buffer)[3*linid+i] = val[i];
                }
            }
            ++linid;
        }
    }*/
    // --

    // init jl variables
    std::vector<jl_array_t*> jl_values;
    // reference from here: https://discourse.julialang.org/t/api-reference-for-julia-embedding-in-c/3963/3
    jl_value_t *array_type2d = jl_apply_array_type((jl_value_t*)jl_float64_type, 2);
    jl_value_t *t2types[] = {(jl_value_t*)jl_long_type, (jl_value_t*)jl_long_type};
    jl_tupletype_t *t2t = jl_apply_tuple_type_v(t2types, 2);
    jl_value_t* v2size = jl_new_struct_uninit(t2t);
    ((ssize_t*)v2size)[1] = gdp->getNumPoints();

    // vector3 attributes
    //((ssize_t*)v2size)[0] = 3;
    for(entry3f& entry: r_bind_entries3f){
        ((ssize_t*)v2size)[0] = entry.tuple_size;
        jl_values.push_back(jl_ptr_to_array(array_type2d, entry.buffer->data(), v2size, 0));
    }

    //JL_GC_PUSH3(&v2size, &pos_jl, &cd_jl);  // so it's not deleted during next allocations
    /*
    jl_array_t *pos_jl = jl_ptr_to_array(array_type2d, cachedBuffers["P"].data(), v2size, 0);
    jl_array_t *cd_jl = jl_ptr_to_array(array_type2d, cachedBuffers["Cd"].data(), v2size, 0);
    */
    //jl_array_t *pos_jl = jl_ptr_to_array_1d(array_type, pos_data.data(), pos_data.size(), 0);
    //jl_array_t *cd_jl = jl_ptr_to_array_1d(array_type, cd_data.data(), cd_data.size(), 0);
    //JL_GC_POP();
    // --

    UT_String nodeFuncName;
    getFullPath(nodeFuncName);
    nodeFuncName.substitute('/', '_');
    if(prevCode!=code || prevFuncName!=nodeFuncName || codeFuncAttrs!=prevAttrs){
        prevCode = code;
        prevFuncName = nodeFuncName;
        prevAttrs = codeFuncAttrs;
        prevCode.hardenIfNeeded();
        prevFuncName.hardenIfNeeded();
        prevAttrs.hardenIfNeeded();

        UT_String signature;
        signature.sprintf("function %s(%s)\n", nodeFuncName.c_str(), codeFuncAttrs.c_str());
        code.prepend(signature);
        code.append("\nend");
        debug()<<"applying new julia function"<<std::endl<<code<<std::endl;
        //code.prepend(nodeFuncName);
        //code.prepend("function ");
        //code.append("\nend");
        jl_array_t *ret = (jl_array_t*)jl_eval_string(code.c_str());
        if(ret==NULL){
            if(jl_exception_occurred())
                addError(SOP_MESSAGE, jl_typeof_str(jl_exception_occurred()));
            else
                addError(SOP_MESSAGE, "something went wrong");
            return error();
        }
    }
    jl_function_t *jfunc = jl_get_function(jl_main_module, nodeFuncName.c_str());
    if(jfunc==NULL){
        addError(SOP_MESSAGE, "couldn't get da function");
        return error();
    }
    jl_call(jfunc, (jl_value_t**)(jl_values.data()), jl_values.size());
    /*
    jl_call2(jfunc, (jl_value_t*)pos_jl, (jl_value_t*)cd_jl);
    */
    // note: we dont care aboug GC here, as even if arrays were collected - we do not reuse them, and we own the buffers.
    if(jl_exception_occurred()){
        addError(SOP_MESSAGE, jl_typeof_str(jl_exception_occurred()));
        return error();
    }

    /*
    double *data = (double*)jl_array_data(ret);
    size_t array_size = jl_array_len(ret);
    std::cout<<"res size is: "<<array_size<<std::endl;
    for(size_t i=0;i<array_size;++i){
        std::cout<<data[i]<<"  ";
    }
    std::cout<<std::endl;
    */

    // save results back to dgp
    for(entry3f& entry: r_bind_entries3f){  // TODO: check if buffer was not resized somehow!
        entry.attr->getAIFTuple()->setRange(entry.attr, gdp->getPointRange(), entry.buffer->data());
    }
    /*
    linid = 0;
    for(GA_Iterator it(gdp->getPointRange());it.blockAdvance(block_start, block_end);){
        for(GA_Offset off=block_start; off<block_end; ++off){
            // vector3 attributes
            for(entry3f& entry: w_bind_entries3f){
                UT_Vector3F val;
                for(int i=0;i<3;++i){
                    val[i] = (*entry.buffer)[3*linid+i];
                }
                entry.att_handle.set(off, val);
            }
            
            ++linid;
        }
    }*/
    // --

    return error();
}
