#pragma once
// Minimal Unreal Engine 5.5 reflection: object array, names, properties, functions.
// Everything is located at runtime with heuristics (no hard-coded addresses),
// so a game patch usually does not break it.
#include "common.h"
#include <vector>

namespace ue {

struct Offsets {
    // UObject (stable since UE4)
    int ObjFlags = 0x08, ObjIndex = 0x0C, ObjClass = 0x10, ObjName = 0x18, ObjOuter = 0x20;
    int FieldNext = 0x28;  // UField::Next
    // Found at runtime
    int SuperStruct = -1, Children = -1, ChildProperties = -1, PropertiesSize = -1;
    int FFieldClass = 0x08, FFieldNext = -1, FFieldName = -1;
    int PropOffset = -1;  // FProperty::Offset_Internal (ArrayDim at -0x14, ElementSize at -0x10)
    int FunctionFlags = -1;
    int ProcessEventIndex = -1;
    int ItemStride = 0x18;  // FUObjectItem size
};
extern Offsets off;

constexpr uint32_t RF_ClassDefaultObject = 0x10;
constexpr uint32_t FUNC_Native = 0x400;

struct TArrayRaw {
    void* data;
    int32_t num;
    int32_t max;
};

struct FStringView {  // layout of FString, pointing at caller-owned memory
    const wchar_t* data;
    int32_t num;  // including terminating zero
    int32_t max;
};

struct PropInfo {
    void* field = nullptr;  // FProperty*
    int offset = -1;
    int elementSize = 0;
    std::string type;  // "FloatProperty", "StructProperty", ...
    explicit operator bool() const { return field != nullptr; }
};

bool Init();
bool Ready();

// Objects
int32_t NumObjects();
void* ObjectAt(int32_t index);
bool IsLive(void* obj, int32_t index);  // obj still sits at index (not destroyed/reused)
inline void* ClassOf(void* o) { return *reinterpret_cast<void**>((uint8_t*)o + off.ObjClass); }
inline void* OuterOf(void* o) { return *reinterpret_cast<void**>((uint8_t*)o + off.ObjOuter); }
inline uint32_t FlagsOf(void* o) { return *reinterpret_cast<uint32_t*>((uint8_t*)o + off.ObjFlags); }
inline int32_t IndexOf(void* o) { return *reinterpret_cast<int32_t*>((uint8_t*)o + off.ObjIndex); }
// Class default objects and archetypes (default subobjects/templates) are not live game objects.
inline bool IsCDO(void* o) { return (FlagsOf(o) & (RF_ClassDefaultObject | 0x20 /*RF_ArchetypeObject*/)) != 0; }
inline void* SuperOf(void* s) { return *reinterpret_cast<void**>((uint8_t*)s + off.SuperStruct); }
inline int32_t StructSize(void* s) { return *reinterpret_cast<int32_t*>((uint8_t*)s + off.PropertiesSize); }

std::string NameOf(void* obj);         // short name
std::string PathOf(void* obj);         // Outer.Outer.Name
std::string NameFromFName(const void* fnameAddr);
bool IsA(void* obj, void* cls);
bool IsChildOf(void* cls, void* base);

// Lookups. FindClass caches by short name (e.g. "Actor", "HnSUberAttributeSet").
void* FindClass(const char* name);
void* FindObject(const char* name, void* ofClass = nullptr);  // first object with that name
void* FindFirstInstance(void* cls);                            // first non-CDO instance

PropInfo FindProperty(void* structOrClass, const char* name);
void* FindFunction(void* cls, const char* name);
bool CallFunction(void* obj, void* func, void* params);

// Iterate the properties declared directly on one struct (not supers).
void ForEachProperty(void* strct, void (*cb)(void* field, void* user), void* user);
PropInfo DescribeProperty(void* field);
void* PropertyStruct(void* field);  // UScriptStruct of a StructProperty (nullptr otherwise)
std::string FieldName(void* field);
uint64_t PropertyFlags(void* field);  // EPropertyFlags
int FPropertySize();                  // sizeof(FProperty); subclass data (inner prop, struct, bool mask) follows it
void* PropertySlot(void* field, int index = 0);  // pointer stored at FPropertySize() + 8*index
// Inner property of an ArrayProperty. UE 5.1+ stores EArrayPropertyFlags before Inner.
inline void* ArrayInner(void* field) {
    void* p = PropertySlot(field, 0);
    return p ? p : PropertySlot(field, 1);
}
bool FNameFromString(const std::string& s, uint32_t& comparisonIndex);  // case-insensitive pool search
std::vector<std::string> FindNames(const std::string& substring, int max);  // names containing substring
std::string EnumValueName(void* uenum, int64_t value);  // "" if unknown

constexpr uint64_t CPF_Parm = 0x80, CPF_OutParm = 0x100, CPF_ReturnParm = 0x400;

// Iterates the allocated elements of a TSet/TMap (its TSparseArray). `stride` is the size
// of one TSetElement (value + 8 bytes of hash links, aligned). Returns the element count.
int ForEachSparseElement(const void* setAddr, int stride, void (*cb)(uint8_t* elem, void* user), void* user);
// TSetElement stride for an element of the given size.
inline int SetElementStride(int elemSize) {
    int align = elemSize >= 8 ? 8 : 4;
    return (elemSize + 8 + align - 1) / align * align;
}
// Calls a UFunction by name with parameters filled by the caller; params buffer is sized for you.
bool CallByName(void* obj, const char* fnName, const std::vector<std::pair<const char*, const void*>>& args,
                std::vector<uint8_t>* outParams = nullptr);

template <class T>
inline T& At(void* obj, int offset) { return *reinterpret_cast<T*>((uint8_t*)obj + offset); }

// Writes a text dump of all classes of a script package (e.g. "/Script/HnS").
void DumpPackage(const char* packagePrefix, const std::wstring& path);

}  // namespace ue
