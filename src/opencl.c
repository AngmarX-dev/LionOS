#include <stdint.h>
#include "opencl.h"

#define OPENCL_MAX_WORK_ITEMS 1048576u
#define OPENCL_LOCAL_MEM_BYTES 16384u
#define OPENCL_TEST_COUNT 32u

static opencl_device_info_t device;
static uint32_t initialized;

static void cpu_dispatch(opencl_kernel_fn kernel, uint32_t global_size, void *args)
{
    for(uint32_t gid=0u; gid<global_size; ++gid)
        kernel(gid, args);
}

int opencl_init(void)
{
    device.available=1u;
    device.backend=OPENCL_BACKEND_CPU;
    device.max_work_items=OPENCL_MAX_WORK_ITEMS;
    device.local_mem_bytes=OPENCL_LOCAL_MEM_BYTES;
    device.platform_name="LionOS";
    device.device_name="LionCL CPU";
    device.backend_name="CPU";
    initialized=1u;
    return 0;
}

int opencl_available(void)
{
    return initialized && device.available ? 1 : 0;
}

int opencl_get_device_info(opencl_device_info_t *out)
{
    if(!out || !opencl_available())
        return -1;
    *out=device;
    return 0;
}

int opencl_enqueue_ndrange_1d(opencl_kernel_fn kernel, uint32_t global_size, void *args)
{
    if(!opencl_available() || !kernel || !global_size ||
       global_size>device.max_work_items)
        return -1;

    if(device.backend==OPENCL_BACKEND_CPU){
        cpu_dispatch(kernel,global_size,args);
        return 0;
    }
    return -1;
}

typedef struct {
    const uint32_t *a;
    const uint32_t *b;
    uint32_t *out;
} vector_add_args_t;

static void vector_add_kernel(uint32_t gid, void *opaque)
{
    vector_add_args_t *args=(vector_add_args_t*)opaque;
    args->out[gid]=args->a[gid]+args->b[gid];
}

int opencl_vector_add_u32(const uint32_t *a,
                          const uint32_t *b,
                          uint32_t *out,
                          uint32_t count)
{
    vector_add_args_t args;
    if(!opencl_available() || !a || !b || !out || !count ||
       count>device.max_work_items)
        return -1;
    args.a=a;
    args.b=b;
    args.out=out;
    return opencl_enqueue_ndrange_1d(vector_add_kernel,count,&args);
}

int opencl_self_test(void)
{
    static const uint32_t a[OPENCL_TEST_COUNT]={
        0u,1u,2u,3u,4u,5u,6u,7u,8u,9u,10u,11u,12u,13u,14u,15u,
        16u,17u,18u,19u,20u,21u,22u,23u,24u,25u,26u,27u,28u,29u,30u,31u
    };
    static const uint32_t b[OPENCL_TEST_COUNT]={
        31u,30u,29u,28u,27u,26u,25u,24u,23u,22u,21u,20u,19u,18u,17u,16u,
        15u,14u,13u,12u,11u,10u,9u,8u,7u,6u,5u,4u,3u,2u,1u,0u
    };
    uint32_t out[OPENCL_TEST_COUNT];

    if(opencl_init()!=0)
        return -1;

    for(uint32_t i=0u;i<OPENCL_TEST_COUNT;++i)
        out[i]=0xFFFFFFFFu;

    if(opencl_vector_add_u32(a,b,out,OPENCL_TEST_COUNT)!=0)
        return -1;

    for(uint32_t i=0u;i<OPENCL_TEST_COUNT;++i)
        if(out[i]!=31u)
            return -1;

    return 0;
}
