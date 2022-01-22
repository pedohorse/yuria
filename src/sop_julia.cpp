
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

PRM_Template SOP_julia::parmtemplates[] = {
    PRM_Template(PRM_STRING, 1, &prm_snippet_name),
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

OP_ERROR SOP_julia::cookMySop(OP_Context &context){
    OP_AutoLockInputs inputlock(this);
    if(inputlock.lock(context) >= UT_ERROR_ABORT)
        return error();

    duplicateSource(0, context);
    UT_String code;
    evalString(code, "snippet", 0, context.getTime());

    GA_RWHandleV3 pos_handle(gdp, GA_ATTRIB_POINT, "P");
    GA_RWHandleV3 cd_handle(gdp, GA_ATTRIB_POINT, "Cd");
    if(cachedBuffers.find("P")==cachedBuffers.end())
        cachedBuffers["P"] = std::vector<double>();
    if(cachedBuffers.find("Cd")==cachedBuffers.end())
        cachedBuffers["Cd"] = std::vector<double>();
    /*
    std::vector<std::vector<double>*> used_buffers;
    used_buffers.resize(2);
    used_buffers[0]=&cachedBuffers["P"];
    used_buffers[1]=&cachedBuffers["Cd"];
    used_buffers[0]->resize(gdp->getNumPoints()*3);
    used_buffers[1]->resize(gdp->getNumPoints()*3);  //hm.....
    */

    // store attrib data in vectors
    GA_Offset block_start, block_end;
    size_t linid = 0;
    for(GA_Iterator it(gdp->getPointRange());it.blockAdvance(block_start, block_end);){
        for(GA_Offset off=block_start; off<block_end; ++off){
            UT_Vector3F vpos = pos_handle.get(off);
            UT_Vector3F vcd = cd_handle.get(off);
            for(int i=0;i<3;++i){
                cachedBuffers["P"][3*linid+i] = vpos[i];
                cachedBuffers["Cd"][3*linid+i] = vcd[i];
            }
            ++linid;
        }
    }
    // --

    // init jl variables
    // reference from here: https://discourse.julialang.org/t/api-reference-for-julia-embedding-in-c/3963/3
    jl_value_t *array_type2d = jl_apply_array_type((jl_value_t*)jl_float64_type, 2);
    jl_value_t *t2types[] = {(jl_value_t*)jl_long_type, (jl_value_t*)jl_long_type};
    jl_tupletype_t *t2t = jl_apply_tuple_type_v(t2types, 2);
    jl_value_t* v2size = jl_new_struct_uninit(t2t);
    ((ssize_t*)v2size)[0] = 3;
    ((ssize_t*)v2size)[1] = gdp->getNumPoints();

    //JL_GC_PUSH3(&v2size, &pos_jl, &cd_jl);  // so it's not deleted during next allocations
    jl_array_t *pos_jl = jl_ptr_to_array(array_type2d, cachedBuffers["P"].data(), v2size, 0);
    jl_array_t *cd_jl = jl_ptr_to_array(array_type2d, cachedBuffers["Cd"].data(), v2size, 0);
    //jl_array_t *pos_jl = jl_ptr_to_array_1d(array_type, pos_data.data(), pos_data.size(), 0);
    //jl_array_t *cd_jl = jl_ptr_to_array_1d(array_type, cd_data.data(), cd_data.size(), 0);
    //JL_GC_POP();
    // --

    UT_String nodeFuncName;
    getFullPath(nodeFuncName);
    nodeFuncName.substitute('/', '_');
    if(prevCode!=code || prevFuncName!=nodeFuncName){
        prevCode = code;
        prevFuncName = nodeFuncName;
        prevCode.hardenIfNeeded();
        prevFuncName.hardenIfNeeded();

        code.prepend("(P, Cd)\n");
        code.prepend(nodeFuncName);
        code.prepend("function ");
        code.append("\nend");
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
    jl_call2(jfunc, (jl_value_t*)pos_jl, (jl_value_t*)cd_jl);
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
    linid = 0;
    for(GA_Iterator it(gdp->getPointRange());it.blockAdvance(block_start, block_end);){
        for(GA_Offset off=block_start; off<block_end; ++off){
            UT_Vector3F vpos;
            UT_Vector3F vcd;
            for(int i=0;i<3;++i){
                vpos[i] = cachedBuffers["P"][3*linid+i];
                vcd[i] = cachedBuffers["Cd"][3*linid+i];
            }
            ++linid;
            pos_handle.set(off, vpos);
            cd_handle.set(off, vcd);
        }
    }
    // --

    return error();
}
