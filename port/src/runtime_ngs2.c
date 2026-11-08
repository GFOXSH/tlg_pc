#define _GNU_SOURCE
#include "runtime.h"
#include "ngs2.h"
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#define NGS2_GUEST_PAGE_SIZE 0x4000u

static Ngs2Module ngs2_module;
static int prepare_attempted;
static int prepare_result = NGS2_STATIC_E_STATE;
static int start_attempted;
static int start_result;

static void *ngs2_allocate(size_t size, size_t alignment, void *userdata) {
    (void)userdata;
    if (!size || !alignment || (alignment & (alignment - 1)) || alignment > NGS2_GUEST_PAGE_SIZE)
        return NULL;
    void *memory = runtime_low_map(size, PROT_READ | PROT_WRITE);
    if (memory && ((uintptr_t)memory & (alignment - 1))) {
        if (runtime_low_unmap(memory, size))
            fputs("NGS2: could not release misaligned guest allocation\n", stderr);
        return NULL;
    }
    return memory;
}

static int ngs2_protect(void *address, size_t size, uint32_t flags, void *userdata) {
    (void)userdata;
    if (!address || !size || ((uintptr_t)address & (NGS2_GUEST_PAGE_SIZE - 1)) ||
        (size & (NGS2_GUEST_PAGE_SIZE - 1)) ||
        (flags & ~(NGS2_STATIC_PROT_READ | NGS2_STATIC_PROT_WRITE | NGS2_STATIC_PROT_EXEC)) ||
        ((flags & NGS2_STATIC_PROT_WRITE) && (flags & NGS2_STATIC_PROT_EXEC)))
        return -1;
#ifdef _WIN32
    DWORD protection = PAGE_NOACCESS;
    if (flags & NGS2_STATIC_PROT_EXEC)
        protection = (flags & NGS2_STATIC_PROT_READ) ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    else if (flags & NGS2_STATIC_PROT_WRITE)
        protection = PAGE_READWRITE;
    else if (flags & NGS2_STATIC_PROT_READ)
        protection = PAGE_READONLY;
    DWORD previous;
    return VirtualProtect(address, size, protection, &previous) ? 0 : -1;
#else
    int protection = 0;
    if (flags & NGS2_STATIC_PROT_READ) protection |= PROT_READ;
    if (flags & NGS2_STATIC_PROT_WRITE) protection |= PROT_WRITE;
    if (flags & NGS2_STATIC_PROT_EXEC) protection |= PROT_EXEC;
    return mprotect(address, size, protection);
#endif
}

static void ngs2_deallocate(void *address, size_t size, void *userdata) {
    (void)userdata;
    if (address && runtime_low_unmap(address, size))
        fputs("NGS2: could not release guest image mapping\n", stderr);
}

static uintptr_t ngs2_resolve_import(const char *nid_library, int is_data, void *userdata) {
    (void)userdata;
    return nid_library ? runtime_resolve(nid_library, is_data != 0) : 0;
}

static const Ngs2HostApi ngs2_host = {
    .size = sizeof(Ngs2HostApi),
    .version = NGS2_STATIC_API_VERSION,
    .allocate = ngs2_allocate,
    .protect = ngs2_protect,
    .deallocate = ngs2_deallocate,
    .resolve = ngs2_resolve_import,
};

static int ngs2_prepare(void) {
    if (prepare_attempted)
        return prepare_result;
    prepare_attempted = 1;
    prepare_result = ngs2_static_prepare(&ngs2_module, &ngs2_host);
    if (prepare_result != NGS2_STATIC_OK) {
        fprintf(stderr, "NGS2: bundled V5 prepare failed (%d, %u unresolved imports)\n",
                prepare_result, ngs2_module.unresolved_imports);
        return prepare_result;
    }
    printf("NGS2: prepared bundled %s (%zu exports)\n",
           ngs2_static_build_id(), ngs2_static_export_count());
    return NGS2_STATIC_OK;
}

uintptr_t runtime_ngs2_resolve(const char *name) {
    if (!name || ngs2_prepare() != NGS2_STATIC_OK)
        return 0;
    return ngs2_static_resolve(&ngs2_module, name);
}

int runtime_ngs2_start(void) {
    if (!prepare_attempted)
        return NGS2_STATIC_OK;
    if (prepare_result != NGS2_STATIC_OK)
        return prepare_result;
    if (start_attempted)
        return start_result;
    start_attempted = 1;
    start_result = ngs2_static_start(&ngs2_module);
    if (start_result != NGS2_STATIC_OK) {
        fprintf(stderr, "NGS2: bundled V5 initialization failed (%d)\n", start_result);
        ngs2_static_unload(&ngs2_module);
        return start_result;
    }
    puts("NGS2: bundled V5 initialized");
    return NGS2_STATIC_OK;
}
