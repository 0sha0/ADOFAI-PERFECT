#include "MonoApi.h"
#include "Log.h"
#include <windows.h>

namespace MonoApi
{
    // ---- 函数指针定义 ----
    MonoDomain*   (*mono_get_root_domain)() = nullptr;
    MonoDomain*   (*mono_domain_get)() = nullptr;
    MonoThread*   (*mono_thread_attach)(MonoDomain*) = nullptr;
    MonoThread*   (*mono_thread_detach)(MonoThread*) = nullptr;
    MonoImage*    (*mono_image_loaded)(const char*) = nullptr;
    MonoAssembly* (*mono_assembly_get_image)(MonoAssembly*) = nullptr;
    const char*   (*mono_image_get_name)(MonoImage*) = nullptr;
    MonoClass*    (*mono_class_from_name)(MonoImage*, const char*, const char*) = nullptr;
    const char*   (*mono_class_get_name)(MonoClass*) = nullptr;
    MonoClassField* (*mono_class_get_field_from_name)(MonoClass*, const char*) = nullptr;
    void*         (*mono_class_get_fields)(MonoClass*, void**) = nullptr;
    MonoMethod*   (*mono_class_get_method_from_name)(MonoClass*, const char*, int) = nullptr;
    MonoVTable*   (*mono_class_vtable)(MonoDomain*, MonoClass*) = nullptr;
    int           (*mono_class_init)(MonoClass*) = nullptr;
    void          (*mono_field_static_get_value)(MonoVTable*, MonoClassField*, void*) = nullptr;
    void          (*mono_field_static_set_value)(MonoVTable*, MonoClassField*, void*) = nullptr;
    void          (*mono_field_get_value)(MonoObject*, MonoClassField*, void*) = nullptr;
    void          (*mono_field_set_value)(MonoObject*, MonoClassField*, void*) = nullptr;
    uint32_t      (*mono_field_get_offset)(MonoClassField*) = nullptr;
    const char*   (*mono_field_get_name)(MonoClassField*) = nullptr;
    void*         (*mono_compile_method)(MonoMethod*) = nullptr;
    MonoObject*   (*mono_runtime_invoke)(MonoMethod*, void*, void**, MonoObject**) = nullptr;
    MonoString*   (*mono_string_new)(MonoDomain*, const char*) = nullptr;
    char*         (*mono_string_to_utf8)(MonoString*) = nullptr;
    void          (*mono_free)(void*) = nullptr;
    MonoClass*    (*mono_object_get_class)(MonoObject*) = nullptr;
    void*         (*mono_array_addr_with_size)(MonoObject*, int, int) = nullptr;
    gunichar2*    (*mono_string_chars)(MonoString*) = nullptr;
    int           (*mono_string_length)(MonoString*) = nullptr;
    void*         (*mono_vtable_get_static_field_data)(MonoVTable*) = nullptr;

    static bool g_ready = false;
    bool Ready() { return g_ready; }

    bool Init()
    {
        // 游戏使用 Unity "MonoBleedingEdge"，运行时 dll 名为 mono-2.0-bdwgc.dll
        HMODULE h = GetModuleHandleW(L"mono-2.0-bdwgc.dll");
        if (!h)
        {
            Log::Printf("[MonoApi] mono-2.0-bdwgc.dll not loaded yet");
            return false;
        }

        struct Entry { const char* name; void** pp; };
        static const Entry table[] = {
            { "mono_get_root_domain",            (void**)&mono_get_root_domain },
            { "mono_domain_get",                 (void**)&mono_domain_get },
            { "mono_thread_attach",              (void**)&mono_thread_attach },
            { "mono_thread_detach",              (void**)&mono_thread_detach },
            { "mono_image_loaded",               (void**)&mono_image_loaded },
            { "mono_assembly_get_image",         (void**)&mono_assembly_get_image },
            { "mono_image_get_name",             (void**)&mono_image_get_name },
            { "mono_class_from_name",            (void**)&mono_class_from_name },
            { "mono_class_get_name",             (void**)&mono_class_get_name },
            { "mono_class_get_field_from_name",  (void**)&mono_class_get_field_from_name },
            { "mono_class_get_fields",           (void**)&mono_class_get_fields },
            { "mono_class_get_method_from_name", (void**)&mono_class_get_method_from_name },
            { "mono_class_vtable",               (void**)&mono_class_vtable },
            { "mono_class_init",                 (void**)&mono_class_init },
            { "mono_field_static_get_value",     (void**)&mono_field_static_get_value },
            { "mono_field_static_set_value",     (void**)&mono_field_static_set_value },
            { "mono_field_get_value",            (void**)&mono_field_get_value },
            { "mono_field_set_value",            (void**)&mono_field_set_value },
            { "mono_field_get_offset",           (void**)&mono_field_get_offset },
            { "mono_field_get_name",             (void**)&mono_field_get_name },
            { "mono_compile_method",             (void**)&mono_compile_method },
            { "mono_runtime_invoke",             (void**)&mono_runtime_invoke },
            { "mono_string_new",                 (void**)&mono_string_new },
            { "mono_string_to_utf8",             (void**)&mono_string_to_utf8 },
            { "mono_free",                       (void**)&mono_free },
            { "mono_object_get_class",           (void**)&mono_object_get_class },
            { "mono_array_addr_with_size",       (void**)&mono_array_addr_with_size },
            { "mono_string_chars",               (void**)&mono_string_chars },
            { "mono_string_length",              (void**)&mono_string_length },
            { "mono_vtable_get_static_field_data", (void**)&mono_vtable_get_static_field_data },
        };

        bool allFound = true;
        for (const Entry& e : table)
        {
            void* p = (void*)GetProcAddress(h, e.name);
            *e.pp = p;
            if (!p)
            {
                Log::Printf("[MonoApi] MISSING export: %s", e.name);
                allFound = false;
            }
        }
        if (!allFound)
            return false;

        g_ready = true;
        Log::Printf("[MonoApi] all %d exports resolved (module=%p)", (int)(sizeof(table) / sizeof(table[0])), (void*)h);
        return true;
    }
}
