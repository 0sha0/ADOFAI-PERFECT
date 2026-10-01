#pragma once
#include <cstdint>
// ============================================================
// MonoApi.h — 动态解析游戏自带 Mono 运行时 (mono-2.0-bdwgc.dll) 的导出函数。
// 全部为运行时 GetProcAddress，不做任何链接期依赖。
// ============================================================

// 前置声明（与 mono 头文件中的不透明类型一致）
typedef struct MonoDomain     MonoDomain;
typedef struct MonoImage      MonoImage;
typedef struct MonoAssembly   MonoAssembly;
typedef struct MonoClass      MonoClass;
typedef struct MonoClassField MonoClassField;
typedef struct MonoMethod     MonoMethod;
typedef struct MonoObject     MonoObject;
typedef struct MonoVTable     MonoVTable;
typedef struct MonoString     MonoString;
typedef struct MonoThread     MonoThread;
typedef unsigned short        gunichar2;

namespace MonoApi
{
    bool Init();   // 解析全部导出，缺关键导出返回 false
    bool Ready();

    // ---- 导出函数指针 ----
    extern MonoDomain*   (*mono_get_root_domain)();
    extern MonoDomain*   (*mono_domain_get)();
    extern MonoThread*   (*mono_thread_attach)(MonoDomain* domain);
    extern MonoThread*   (*mono_thread_detach)(MonoThread* thread);
    extern MonoImage*    (*mono_image_loaded)(const char* name);
    extern MonoAssembly* (*mono_assembly_get_image)(MonoAssembly* assembly);
    extern const char*   (*mono_image_get_name)(MonoImage* image);
    extern MonoClass*    (*mono_class_from_name)(MonoImage* image, const char* name_space, const char* name);
    extern const char*   (*mono_class_get_name)(MonoClass* klass);
    extern MonoClassField* (*mono_class_get_field_from_name)(MonoClass* klass, const char* name);
    extern void*         (*mono_class_get_fields)(MonoClass* klass, void** iter);
    extern MonoMethod*   (*mono_class_get_method_from_name)(MonoClass* klass, const char* name, int param_count);
    extern MonoVTable*   (*mono_class_vtable)(MonoDomain* domain, MonoClass* klass);
    extern int           (*mono_class_init)(MonoClass* klass);
    extern void          (*mono_field_static_get_value)(MonoVTable* vt, MonoClassField* field, void* value);
    extern void          (*mono_field_static_set_value)(MonoVTable* vt, MonoClassField* field, void* value);
    extern void          (*mono_field_get_value)(MonoObject* obj, MonoClassField* field, void* value);
    extern void          (*mono_field_set_value)(MonoObject* obj, MonoClassField* field, void* value);
    extern uint32_t      (*mono_field_get_offset)(MonoClassField* field);
    extern const char*   (*mono_field_get_name)(MonoClassField* field);
    extern void*         (*mono_compile_method)(MonoMethod* method);
    extern MonoObject*   (*mono_runtime_invoke)(MonoMethod* method, void* obj, void** params, MonoObject** exc);
    extern MonoString*   (*mono_string_new)(MonoDomain* domain, const char* text);
    extern char*         (*mono_string_to_utf8)(MonoString* string_obj);
    extern void          (*mono_free)(void* ptr);
    extern MonoClass*    (*mono_object_get_class)(MonoObject* obj);
    extern void*         (*mono_array_addr_with_size)(MonoObject* array, int size, int index);
    extern gunichar2*    (*mono_string_chars)(MonoString* string_obj);
    extern int           (*mono_string_length)(MonoString* string_obj);
    extern void*         (*mono_vtable_get_static_field_data)(MonoVTable* vtable);
}
