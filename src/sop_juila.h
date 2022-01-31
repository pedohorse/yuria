#ifndef __sop_julia_h__
#define __sop_julia_h__

#include <SOP/SOP_Node.h>
#include <mutex>
#include <condition_variable>

typedef struct julia_thread_input_data_t julia_thread_input_data_t;  // fwd

class SOP_julia: public SOP_Node{
public:
    SOP_julia(OP_Network *net, const char *name, OP_Operator *op);
    virtual ~SOP_julia();

    static PRM_Template parmtemplates[];
    static OP_Node *make_me(OP_Network*, const char*, OP_Operator*);

protected:
    OP_ERROR cookMySop(OP_Context &context) override;
private:
    UT_String prevCode, prevInitCode, prevFuncName;
    std::string prevAttrs;
    std::map<UT_String,std::vector<double>> cachedBuffersf64;
    std::map<UT_String,std::vector<int64>> cachedBuffersi64;

    static size_t instance_count;
    static bool jl_initialized;
    
    static void atExit(void*);
    static julia_thread_input_data_t *julia_thread_input_data;
    static void julia_dedicated_thread_func();
    static int julia_inner_function();
    static std::mutex jt_input_ready_mutex;
    static std::condition_variable jt_input_ready;
    static bool time_to_stop_julia_thread;
    static std::unique_ptr<std::thread> julia_thread;

    static std::mutex julia_access_mutex;
};

#endif