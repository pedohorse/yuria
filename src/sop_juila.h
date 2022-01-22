#ifndef __sop_julia_h__
#define __sop_julia_h__

#include <SOP/SOP_Node.h>

class SOP_julia: public SOP_Node{
public:
    SOP_julia(OP_Network *net, const char *name, OP_Operator *op);
    virtual ~SOP_julia();

    static PRM_Template parmtemplates[];
    static OP_Node *make_me(OP_Network*, const char*, OP_Operator*);

protected:
    OP_ERROR cookMySop(OP_Context &context) override;
private:
    UT_String prevCode, prevFuncName, prevAttrs;
    std::map<UT_String,std::vector<double>> cachedBuffers;

    static size_t instance_count;
    static bool jl_initialized;
    
    static void atExit(void*);
};

#endif