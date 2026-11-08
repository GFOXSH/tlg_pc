#ifndef LIBSCE_NGS2_V5_STATIC_H
#define LIBSCE_NGS2_V5_STATIC_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NGS2_STATIC_API_VERSION 1u
#define NGS2_STATIC_PROT_READ 1u
#define NGS2_STATIC_PROT_WRITE 2u
#define NGS2_STATIC_PROT_EXEC 4u
#define NGS2_STATIC_OK 0
#define NGS2_STATIC_E_ARGUMENT -1
#define NGS2_STATIC_E_API -2
#define NGS2_STATIC_E_ALLOC -3
#define NGS2_STATIC_E_ALIGNMENT -4
#define NGS2_STATIC_E_IMPORT -5
#define NGS2_STATIC_E_PROTECT -6
#define NGS2_STATIC_E_STATE -7
typedef struct Ngs2HostApi {
    uint32_t size; uint32_t version; void *userdata;
    void *(*allocate)(size_t size,size_t alignment,void *userdata);
    int (*protect)(void *address,size_t size,uint32_t flags,void *userdata);
    void (*deallocate)(void *address,size_t size,void *userdata);
    uintptr_t (*resolve)(const char *nid_library,int is_data,void *userdata);
} Ngs2HostApi;
typedef struct Ngs2Module { unsigned char *image; size_t allocation_size; const Ngs2HostApi *host; uint32_t state; uint32_t unresolved_imports; } Ngs2Module;
typedef struct Ngs2ExportInfo { const char *nid; uint64_t rva; uint64_t size; } Ngs2ExportInfo;
/* Zero-initialize Ngs2Module. prepare() binds imports and seals RX/RO pages; it does NOT call DT_INIT. */
int ngs2_static_prepare(Ngs2Module *module,const Ngs2HostApi *host);
/* Call only after the TLG runtime, TLS and procparam context are initialized. */
int ngs2_static_start(Ngs2Module *module);
int ngs2_static_fini(Ngs2Module *module);
void ngs2_static_unload(Ngs2Module *module);
/* Resolve any of the ten Sony exports by 11-character base64 NID, optionally followed by #library. */
uintptr_t ngs2_static_resolve(const Ngs2Module *module,const char *nid);
const Ngs2ExportInfo *ngs2_static_export_at(size_t index);
size_t ngs2_static_export_count(void);
size_t ngs2_static_image_size(void);
size_t ngs2_static_function_import_count(void);
size_t ngs2_static_data_import_count(void);
size_t ngs2_static_relative_relocation_count(void);
size_t ngs2_static_function_relocation_count(void);
const char *ngs2_static_build_id(void);
#ifdef __cplusplus
}
#endif
#endif
