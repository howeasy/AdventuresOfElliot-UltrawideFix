// HUDFix - 16:9 HUD constraint for The Adventures of Elliot: The Millennium Tales (UE 5.6.1)
//
// Companion ASI to SUWSF (which makes the 3D world ultrawide). This plugin keeps the HUD/UMG UI
// inside a centred 16:9 band so it doesn't stretch to the ultrawide edges.
//
// How it works: it inline-hooks UGameViewportSubsystem::AddWidget and, on the game thread right
// after each top-level UMG widget is added to the viewport, calls UUserWidget::SetAnchorsInViewport
// with a centred-16:9 FAnchors (UE5 FAnchors/FVector2D are DOUBLE precision). Full-screen overlays
// (fades/movies/loading) are excluded, and the in-world damage vignette is counter-scaled back to
// full screen. Widgets are identified via a from-scratch UE5.6.1 SDK (FName resolved through the
// game's own FName::AppendString, since this build's FNamePool layout differs from the SDK default).
//
// Derived from Lyall's DQ3Fix (MIT). Credits: Lyall, PhantomGamers (SUWSF), Encryqed (Dumper-7).

#include "stdafx.h"
#include "helper.hpp"

#include "SDK/Engine_classes.hpp"
#include "SDK/UMG_classes.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <inipp/inipp.h>
#include <safetyhook.hpp>

#include <unordered_set>
#include <atomic>
#include <mutex>
#include <vector>

#define spdlog_confparse(var) spdlog::info("Config Parse: {}: {}", #var, var)

HMODULE exeModule = GetModuleHandle(NULL);
HMODULE thisModule;

std::string sFixName = "HUDFix";
std::string sFixVersion = "0.32.0-final";
std::filesystem::path sFixPath;

inipp::Ini<char> ini;
std::string sConfigFile = sFixName + ".ini";

std::shared_ptr<spdlog::logger> logger;
std::string sLogFile = sFixName + ".log";
std::filesystem::path sExePath;
std::string sExeName;

std::pair<int, int> DesktopDimensions = { 0,0 };
const float fNativeAspect = 1.777777791f;
float fAspectRatio = 0.0f;
float fHUDWidth = 0.0f, fHUDWidthOffset = 0.0f, fHUDHeight = 0.0f, fHUDHeightOffset = 0.0f;

bool bFixHUD = true;
bool bDiagnostic = true;
int  iSweepIntervalMs = 2000;

// [Debug] isolation toggles (default everything OFF -> minimal probe that only logs + idles)
bool bInstallResHook = false;
bool bEnableSweep = false;
int  iSweepStartDelayMs = 20000;

int iCurrentResX = 0, iCurrentResY = 0;

// ===== Authoritative Elliot UE5.6.1 offsets (captured via patched Dumper-7) =====
void SetElliotOffsets()
{
    SDK::Offsets::GObjects        = 0x9BFB150;
    SDK::Offsets::AppendString    = 0x1379C20;
    SDK::Offsets::GNames          = 0x9ADEAF8;
    SDK::Offsets::ProcessEvent    = 0x15968F0;
    SDK::Offsets::ProcessEventIdx = 0x4C;
}

void Logging()
{
    WCHAR dllPath[_MAX_PATH] = { 0 };
    GetModuleFileNameW(thisModule, dllPath, MAX_PATH);
    sFixPath = dllPath;
    sFixPath = sFixPath.remove_filename();

    WCHAR exePath[_MAX_PATH] = { 0 };
    GetModuleFileNameW(exeModule, exePath, MAX_PATH);
    sExePath = exePath;
    sExeName = sExePath.filename().string();
    sExePath = sExePath.remove_filename();

    try {
        logger = spdlog::basic_logger_st(sFixName.c_str(), sExePath.string() + sLogFile, true);
        spdlog::set_default_logger(logger);
        spdlog::flush_on(spdlog::level::debug);
        spdlog::info("----------");
        spdlog::info("{:s} v{:s} loaded.", sFixName.c_str(), sFixVersion.c_str());
        spdlog::info("Module: {0:s} @ 0x{1:x}", sExeName.c_str(), (uintptr_t)exeModule);
        spdlog::info("----------");
    }
    catch (const spdlog::spdlog_ex& ex) {
        AllocConsole();
        FILE* dummy; freopen_s(&dummy, "CONOUT$", "w", stdout);
        std::cout << "Log init failed: " << ex.what() << std::endl;
        FreeLibraryAndExitThread(thisModule, 1);
    }
}

void Configuration()
{
    std::ifstream iniFile(sFixPath.string() + sConfigFile);
    if (!iniFile) {
        spdlog::warn("Config file not found, using defaults.");
    } else {
        spdlog::info("Config file: {}", sFixPath.string() + sConfigFile);
        ini.parse(iniFile);
        ini.strip_trailing_comments();
        inipp::get_value(ini.sections["Fix HUD"], "Enabled", bFixHUD);
        inipp::get_value(ini.sections["Fix HUD"], "Diagnostic", bDiagnostic);
        inipp::get_value(ini.sections["Fix HUD"], "SweepIntervalMs", iSweepIntervalMs);
        inipp::get_value(ini.sections["Debug"], "InstallResHook", bInstallResHook);
        inipp::get_value(ini.sections["Debug"], "EnableSweep", bEnableSweep);
        inipp::get_value(ini.sections["Debug"], "SweepStartDelayMs", iSweepStartDelayMs);
    }
    spdlog_confparse(bFixHUD);
    spdlog_confparse(bDiagnostic);
    spdlog_confparse(iSweepIntervalMs);
    spdlog_confparse(bInstallResHook);
    spdlog_confparse(bEnableSweep);
    spdlog_confparse(iSweepStartDelayMs);
    spdlog::info("----------");
}

void CalculateAspectRatio(bool bLog)
{
    if (iCurrentResX <= 0 || iCurrentResY <= 0) return;
    fAspectRatio = (float)iCurrentResX / (float)iCurrentResY;
    fHUDWidth = (float)iCurrentResY * fNativeAspect;
    fHUDHeight = (float)iCurrentResY;
    fHUDWidthOffset = (float)(iCurrentResX - fHUDWidth) / 2.00f;
    fHUDHeightOffset = 0.00f;
    if (fAspectRatio < fNativeAspect) {
        fHUDWidth = (float)iCurrentResX;
        fHUDHeight = (float)iCurrentResX / fNativeAspect;
        fHUDWidthOffset = 0.00f;
        fHUDHeightOffset = (float)(iCurrentResY - fHUDHeight) / 2.00f;
    }
    if (bLog) {
        spdlog::info("Resolution: {:d}x{:d} (AR {:.4f}) HUDWidthOffset={:.1f} HUDHeightOffset={:.1f}",
            iCurrentResX, iCurrentResY, fAspectRatio, fHUDWidthOffset, fHUDHeightOffset);
    }
}

void CurrentResolution()
{
    DesktopDimensions = Util::GetPhysicalDesktopDimensions();
    iCurrentResX = DesktopDimensions.first;
    iCurrentResY = DesktopDimensions.second;
    CalculateAspectRatio(true);

    std::uint8_t* CurrentResolutionScanResult = Memory::PatternScan(exeModule, "8D ?? ?? 44 89 ?? ?? ?? ?? ?? 44 89 ?? ?? ?? ?? ?? 44 89 ?? ?? ?? ?? ?? 88 ?? ?? ?? ?? ??");
    if (CurrentResolutionScanResult) {
        spdlog::info("Current Resolution: Address {:s}+{:x}", sExeName.c_str(), CurrentResolutionScanResult - (std::uint8_t*)exeModule);
        static SafetyHookMid CurrentResolutionMidHook{};
        CurrentResolutionMidHook = safetyhook::create_mid(CurrentResolutionScanResult,
            [](SafetyHookContext& ctx) {
                int iResX = (int)ctx.r13;
                int iResY = (int)ctx.r12;
                if (iResX > 0 && iResY > 0 && (iResX != iCurrentResX || iResY != iCurrentResY)) {
                    iCurrentResX = iResX;
                    iCurrentResY = iResY;
                    CalculateAspectRatio(true);
                }
            });
    } else {
        spdlog::error("Current Resolution: Pattern scan failed (using desktop resolution).");
    }
}

// ===================== Diagnostic widget-tree dump (memory-only, worker thread) =====================

static std::unordered_set<std::string> gLoggedNames;

static constexpr uint32_t RF_ClassDefaultObject = 0x00000010;
static constexpr uint32_t RF_ArchetypeObject    = 0x00000020;
static constexpr uint32_t RF_BeginDestroyed     = 0x00000800;
static constexpr uint32_t RF_FinishDestroyed    = 0x00001000;

static SDK::UClass* gUserWidgetClass = nullptr;
static SDK::UClass* gCanvasClass = nullptr;

// Elliot UE5.6.1 UMG offsets (verified from dump; template SDK offsets have drifted).
static constexpr int OFF_UserWidget_WidgetTree   = 0x2D8;  // UObject* WidgetTree
static constexpr int OFF_WidgetTree_RootWidget   = 0x30;   // UWidget* RootWidget
static constexpr int OFF_Widget_Slot             = 0x30;   // UPanelSlot* Slot
static constexpr int OFF_PanelWidget_Slots       = 0x168;  // TArray<UPanelSlot*> Slots
static constexpr int OFF_CanvasSlot_LayoutData   = 0x38;   // FAnchorData LayoutData
static constexpr int OFF_Anchors_in_LayoutData   = 0x10;   // FAnchors within FAnchorData (Min@+0, Max@+8)

template<typename T> static inline T RdMem(void* base, int off) { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + off); }

static inline bool PtrLooksValid(const void* p)
{
    auto v = reinterpret_cast<uintptr_t>(p);
    return v > 0x10000 && v < 0x7FFFFFFFFFFF && (v & 0x3) == 0;
}
static inline uint32_t ObjFlags(SDK::UObject* o)
{
    return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(o) + 0x08);
}
static inline int PanelChildCount(SDK::UWidget* panel)
{
    return RdMem<int32_t>(panel, OFF_PanelWidget_Slots + 0x08); // TArray::Num
}

// Class pointer of a UObject (obj+0x10).
static inline SDK::UClass* ObjClass(SDK::UObject* o)
{
    return *reinterpret_cast<SDK::UClass**>(reinterpret_cast<uint8_t*>(o) + 0x10);
}
// UStruct::SuperStruct at 0x40 (Elliot dump).
static inline SDK::UClass* SuperClass(SDK::UClass* c)
{
    return *reinterpret_cast<SDK::UClass**>(reinterpret_cast<uint8_t*>(c) + 0x40);
}

// Robust IsA: walk the object's class super-chain comparing names (StaticClass/IsA index path
// is unreliable on this build; name resolution via AppendString works).
static bool ObjectIsA(SDK::UObject* obj, const char* targetClassName)
{
    SDK::UClass* cls = ObjClass(obj);
    for (int guard = 0; PtrLooksValid(cls) && guard < 64; ++guard) {
        if (reinterpret_cast<SDK::UObject*>(cls)->GetName() == targetClassName) return true;
        cls = SuperClass(cls);
    }
    return false;
}
static bool WidgetRootIsA(SDK::UWidget* w, const char* targetClassName)
{
    return PtrLooksValid(w) && ObjectIsA(reinterpret_cast<SDK::UObject*>(w), targetClassName);
}

static int gcUserWidgets = 0;   // live UUserWidget instances (non-CDO)
static std::unordered_set<void*> gAppliedSlots;
static std::atomic<int> gcQueued{ 0 };
static std::atomic<int> gcAppliedGT{ 0 };

// UFunctions (found once); work queue handed to game thread.
static SDK::UObject* gSetRenderScaleFn = nullptr;
static SDK::UObject* gIsInViewportFn = nullptr;
struct AnchorTarget { void* widget; float a[4]; };  // a[0..1] = FVector2D render scale
static std::unordered_set<void*> gConstrained;   // widgets already constrained (PE-hook-written)
static std::mutex gQueueMutex;
static std::vector<AnchorTarget> gPending;

// UStruct/UField offsets (Elliot dump): UStruct::Children=0x48, UField::Next=0x28.
static SDK::UObject* FindUFunction(SDK::UClass* cls, const char* fname)
{
    for (SDK::UClass* c = cls; PtrLooksValid(c); c = SuperClass(c)) {
        SDK::UObject* child = RdMem<SDK::UObject*>(c, 0x48);
        for (int g = 0; PtrLooksValid(child) && g < 4000; ++g) {
            if (child->GetName() == fname) return child;
            child = RdMem<SDK::UObject*>(child, 0x28);
        }
    }
    return nullptr;
}

// Widgets that should NOT be constrained (must stay full-screen).
static bool NameExcluded(const std::string& n)
{
    static const char* ex[] = { "Movie", "Loading", "Fade", "FullScreen", "Letterbox", "Pillarbox", "OpenLevel" };
    for (auto e : ex) if (n.find(e) != std::string::npos) return true;
    return false;
}

// Pillarbox: apply a render scale to the whole top-level HUD widget so it occupies a centred 16:9
// band (scales around the widget's centre pivot, independent of how it is anchored). Runs on the
// GAME THREAD (ProcessEvent hook). Only widgets that IsInViewport() are touched. One-shot per widget.
static void ApplyPillarbox(SDK::UObject* uw)
{
    if (iCurrentResX <= 0 || iCurrentResY <= 0) return;

    if (!gSetRenderScaleFn || !gIsInViewportFn) {
        SDK::UClass* c = ObjClass(uw);
        if (PtrLooksValid(c)) {
            if (!gSetRenderScaleFn) gSetRenderScaleFn = FindUFunction(c, "SetRenderScale");
            if (!gIsInViewportFn) gIsInViewportFn = FindUFunction(c, "IsInViewport");
        }
        if (!gSetRenderScaleFn || !gIsInViewportFn) return;
    }

    { std::lock_guard<std::mutex> lk(gQueueMutex); if (gConstrained.count(uw)) return; }

    const bool ultrawide = (fAspectRatio > fNativeAspect);
    const float scaleX = ultrawide ? (fHUDWidth / (float)iCurrentResX) : 1.0f;   // 3840/5120 = 0.75
    const float scaleY = ultrawide ? 1.0f : (fHUDHeight / (float)iCurrentResY);

    AnchorTarget t{ uw, { scaleX, scaleY, 0.0f, 0.0f } };
    { std::lock_guard<std::mutex> lk(gQueueMutex); gPending.push_back(t); }
    gcQueued.fetch_add(1, std::memory_order_relaxed);
}

// Process a single object: proper UUserWidget reflection. Logs live widget tree structure.
static void TryLogWidget(SDK::UObject* obj)
{
    if (!PtrLooksValid(obj)) return;
    uint32_t flags = ObjFlags(obj);
    if (flags & (RF_ClassDefaultObject | RF_ArchetypeObject | RF_BeginDestroyed | RF_FinishDestroyed)) return;
    if (!ObjectIsA(obj, "UserWidget")) return;
    ++gcUserWidgets;

    std::string name = obj->GetName();
    if (gLoggedNames.count(name)) return;

    SDK::UObject* tree = RdMem<SDK::UObject*>(obj, OFF_UserWidget_WidgetTree);
    SDK::UWidget* root = PtrLooksValid(tree) ? RdMem<SDK::UWidget*>(tree, OFF_WidgetTree_RootWidget) : nullptr;
    if (!PtrLooksValid(root)) return;        // only widgets with a real tree (instantiated)
    gLoggedNames.insert(name);

    bool rootIsCanvas = WidgetRootIsA(root, "CanvasPanel");
    int childCount = rootIsCanvas ? PanelChildCount(root) : -1;
    SDK::UClass* cls = ObjClass(obj);
    bool excluded = NameExcluded(name);
    spdlog::info("  UW '{}' class='{}' root='{}' ({}) children={}{}",
        name, PtrLooksValid(cls) ? cls->GetName() : "?", root->GetName(),
        rootIsCanvas ? "Canvas" : "non-canvas", childCount, excluded ? " [excluded]" : "");

    if (bFixHUD && !excluded)
        ApplyPillarbox(obj);
}

// Dump the raw FUObjectArray fields to determine the true layout (uses C++ objects -> no __try here).
static void RawDumpGObjects()
{
    uint8_t* ga = (uint8_t*)exeModule + SDK::Offsets::GObjects;
    int i0 = *(int*)(ga + 0x00), i4 = *(int*)(ga + 0x04), i8 = *(int*)(ga + 0x08), iC = *(int*)(ga + 0x0C);
    int i10 = *(int*)(ga + 0x10), i14 = *(int*)(ga + 0x14), i18 = *(int*)(ga + 0x18), i1C = *(int*)(ga + 0x1C);
    uintptr_t p0 = *(uintptr_t*)(ga + 0x00);
    uintptr_t p8 = *(uintptr_t*)(ga + 0x08);
    spdlog::info("GObjects raw @ +{:x}  ptr@0={:x} ptr@8={:x}", (uintptr_t)SDK::Offsets::GObjects, p0, p8);
    spdlog::info("  int32s: +0={} +4={} +8={} +C={} +10={} +14={} +18={} +1C={}", i0, i4, i8, iC, i10, i14, i18, i1C);
    // If chunked: ptr@0 -> array of chunk pointers; first chunk holds first object pointer.
    if (p0 > 0x10000 && p0 < 0x7FFFFFFFFFFF) {
        uintptr_t firstChunk = *(uintptr_t*)p0;
        spdlog::info("  *ptr0 (firstChunk)={:x}", firstChunk);
        if (firstChunk > 0x10000 && firstChunk < 0x7FFFFFFFFFFF) {
            uintptr_t firstObj = *(uintptr_t*)firstChunk;
            spdlog::info("  firstChunk[0].Object={:x}", firstObj);
        }
    }
}

// Dump the FNamePool + manually decode object[0]'s name to validate name resolution.
static void RawDumpGNames()
{
    uint8_t* pool = (uint8_t*)exeModule + SDK::Offsets::GNames;
    spdlog::info("GNames @ +{:x}  probing layout:", (uintptr_t)SDK::Offsets::GNames);
    // dwords (looking for CurrentBlock ~small, ByteCursor ~large)
    for (int off = 0; off <= 0x20; off += 4)
        spdlog::info("  dword +{:x} = {}", off, *(uint32_t*)(pool + off));
    // qwords (looking for the Blocks[] array of valid pointers)
    for (int off = 0x10; off <= 0x48; off += 8) {
        uintptr_t q = *(uintptr_t*)(pool + off);
        bool looksPtr = (q > 0x10000 && q < 0x7FFFFFFFFFFF);
        spdlog::info("  qword +{:x} = {:x} {}", off, q, looksPtr ? "<-ptr" : "");
    }

    SDK::UObject* o0 = SDK::UObject::GObjects->GetByIndex(0);
    if (!PtrLooksValid(o0)) { spdlog::info("  obj0 invalid"); return; }
    int32_t compIdx = *(int32_t*)((uint8_t*)o0 + 0x18);
    int32_t number  = *(int32_t*)((uint8_t*)o0 + 0x1C);
    spdlog::info("  obj0={:x} FName.CompIdx={} Number={}", (uintptr_t)o0, compIdx, number);

    // Manual FNamePool decode (stride 2, blockbits 16)
    int chunkIdx = compIdx >> 16;
    int inChunk = compIdx & 0xFFFF;
    uint8_t* blk = *(uint8_t**)(pool + 0x10 + (size_t)chunkIdx * 8);
    if (!PtrLooksValid(blk)) { spdlog::info("  block[{}] invalid={:x}", chunkIdx, (uintptr_t)blk); return; }
    uint8_t* entry = blk + (size_t)inChunk * 2;
    uint16_t header = *(uint16_t*)entry;
    int len = header >> 6;
    char buf[128] = { 0 };
    int n = len < 127 ? len : 127;
    for (int i = 0; i < n; ++i) buf[i] = *(char*)(entry + 2 + i);
    spdlog::info("  decoded entry: header={:x} len={} ansi='{}'", header, len, buf);
}

// Sample one object's name+class (uses C++ objects -> no __try here).
static void SampleOneObject(int k)
{
    SDK::UObject* o = SDK::UObject::GObjects->GetByIndex(k);
    if (!PtrLooksValid(o)) return;
    std::string n = o->GetName();
    SDK::UClass* c = ObjClass(o);
    std::string cn = PtrLooksValid(c) ? c->GetName() : "?";
    spdlog::info("  [{}] name='{}' class='{}'", k, n, cn);
}

// Iterate GObjects with per-object SEH isolation. No C++ locals across __try.
static void DiagnosticSweep()
{
    static bool loggedNum = false;
    if (!SDK::UObject::GObjects) { spdlog::warn("DiagnosticSweep: GObjects null"); return; }
    if (!gUserWidgetClass) gUserWidgetClass = SDK::UUserWidget::StaticClass();
    if (!gCanvasClass) gCanvasClass = SDK::UCanvasPanel::StaticClass();

    int num = 0;
    __try { num = SDK::UObject::GObjects->Num(); } __except (EXCEPTION_EXECUTE_HANDLER) { num = -1; }
    if (num <= 0 || num > 5000000) { spdlog::warn("DiagnosticSweep: bad Num()={}", num); return; }

    if (!loggedNum) {
        loggedNum = true;
        spdlog::info("DiagnosticSweep: GObjects->Num()={}. Sampling object names + classes:", num);
        __try { RawDumpGObjects(); } __except (EXCEPTION_EXECUTE_HANDLER) { spdlog::warn("RawDumpGObjects fault"); }
        __try { RawDumpGNames(); } __except (EXCEPTION_EXECUTE_HANDLER) { spdlog::warn("RawDumpGNames fault"); }
        int idxs[] = { 0, 1, 2, 3, 4, 5, 100, 1000, 5000, 20000, 50000, 80000 };
        for (int k : idxs) {
            if (k >= num) continue;
            __try { SampleOneObject(k); }
            __except (EXCEPTION_EXECUTE_HANDLER) { spdlog::warn("  [{}] fault", k); }
        }
    }

    gcUserWidgets = 0;
    for (int i = 0; i < num; ++i) {
        SDK::UObject* obj = nullptr;
        __try { obj = SDK::UObject::GObjects->GetByIndex(i); } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        __try {
            TryLogWidget(obj);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // skip faulting object
        }
    }
    spdlog::info("sweep: objects={} liveUserWidgets={} unique={} queued={} appliedGT={} setAnchorsFn={}",
        num, gcUserWidgets, gLoggedNames.size(), gcQueued.load(), gcAppliedGT.load(), (void*)gSetRenderScaleFn);
}

// ===================== Game-thread apply (ProcessEvent hook) =====================
SafetyHookInline ProcessEvent_sh{};
static thread_local bool tlInApply = false;

// SEH-guarded calls (POD only -> __try allowed).
static bool IsInViewportSEH(void* w)
{
    uint8_t ret = 0;
    __try { ProcessEvent_sh.thiscall<void>(reinterpret_cast<SDK::UObject*>(w), gIsInViewportFn, &ret); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return ret != 0;
}
static void SetRenderScaleSEH(void* w, float* scaleXY)  // FVector2D Scale
{
    __try {
        ProcessEvent_sh.thiscall<void>(reinterpret_cast<SDK::UObject*>(w), gSetRenderScaleFn, scaleXY);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void CounterScaleVignettes(void* hud);   // forward decl (defined in the AddToScreen section)

// Constrained widgets to keep re-applying the dim/border expansion to (beats menu open animations).
struct ConstrainedW { void* w; uint64_t t; };
static std::vector<ConstrainedW> gConstrainedList;
static uint64_t gLastReapplyMs = 0;

static void ReapplyOneSEH(void* w)   // POD wrapper so PE hook can __try around C++ work
{
    __try { CounterScaleVignettes(w); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// We only need the trampoline (ProcessEvent_sh) so the AddWidget hook can call game UFunctions on
// the game thread; the hook body itself just forwards to the original.
void __fastcall ProcessEvent_hk(SDK::UObject* obj, SDK::UObject* fn, void* params)
{
    ProcessEvent_sh.thiscall<void>(obj, fn, params);
}

void HookProcessEvent()
{
    std::uint8_t* pe = (std::uint8_t*)exeModule + SDK::Offsets::ProcessEvent;
    ProcessEvent_sh = safetyhook::create_inline(reinterpret_cast<void*>(pe), reinterpret_cast<void*>(ProcessEvent_hk));
    spdlog::info("ProcessEvent hook ({:s}+{:x}): {}", sExeName.c_str(), (uintptr_t)SDK::Offsets::ProcessEvent,
        (bool)ProcessEvent_sh ? "ok" : "FAILED");
}

// ===================== AddToScreen RE diagnostic =====================
// UUserWidget::AddToScreen(rcx=widget, rdx, r8, r9=slot-geometry template). The template looks like
// FAnchorData {FMargin Offsets @0x00; FAnchors Anchors @0x10; FVector2D Alignment @0x20}. Log it to
// confirm the layout before rewriting anchors to a centred 16:9 box.
static constexpr uint32_t OFF_AddToScreen = 0x35FDBD0;
SafetyHookInline AddToScreen_sh{};
static std::unordered_set<void*> gSeenAdd;

static void LogAddToScreen(void* widget, void* tmpl)   // uses std::string -> no __try here
{
    if (!PtrLooksValid(widget)) return;
    if (gSeenAdd.count(widget)) return;
    gSeenAdd.insert(widget);
    std::string name = reinterpret_cast<SDK::UObject*>(widget)->GetName();
    if (!PtrLooksValid(tmpl)) { spdlog::info("AddWidget '{}' tmpl=invalid({})", name, tmpl); return; }
    float* f = reinterpret_cast<float*>(tmpl);
    // dump first 0x40 bytes as floats so we can see the real FGameViewportWidgetSlot/FAnchorData layout
    spdlog::info("AddWidget '{}' tmpl={} | [0x00]={:.3f},{:.3f},{:.3f},{:.3f} [0x10]={:.3f},{:.3f},{:.3f},{:.3f} [0x20]={:.3f},{:.3f},{:.3f},{:.3f} [0x30]={:.3f},{:.3f},{:.3f},{:.3f}",
        name, tmpl, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
}

// Constrain a just-added viewport widget to a centred 16:9 band via SetAnchorsInViewport.
// Targeted by name (only the gameplay HUD root by default). Runs on the game thread, right after
// the slot is created (offsets default to 0) so a clean anchor change fills a 16:9 box.
static SDK::UObject* gSetAnchorsFn = nullptr;
static std::unordered_set<void*> gDoneConstrain;

// Constrain every top-level UI widget EXCEPT full-screen overlays (fades/movies/loading), which
// must keep covering the whole screen.
static bool ShouldConstrain(const std::string& n)
{
    return !NameExcluded(n);
}

// One-time per widget class: dump direct child widget names so we can find full-screen dim /
// vignette children (which must NOT be pillarboxed) in each menu/HUD widget.
static std::unordered_set<std::string> gDumpedClasses;
static std::string StripInstanceSuffix(const std::string& n)
{
    size_t p = n.find_last_not_of("0123456789");
    if (p != std::string::npos && p + 1 < n.size() && n[p] == '_') return n.substr(0, p);
    return n;
}
static void DumpChildren(void* widget, const std::string& widgetName)
{
    std::string cls = StripInstanceSuffix(widgetName);
    if (gDumpedClasses.count(cls)) return;
    gDumpedClasses.insert(cls);
    SDK::UObject* tree = RdMem<SDK::UObject*>(widget, OFF_UserWidget_WidgetTree);
    if (!PtrLooksValid(tree)) return;
    SDK::UWidget* root = RdMem<SDK::UWidget*>(tree, OFF_WidgetTree_RootWidget);
    if (!PtrLooksValid(root)) return;
    void* slotsData = RdMem<void*>(root, OFF_PanelWidget_Slots);
    int num = RdMem<int32_t>(root, OFF_PanelWidget_Slots + 0x08);
    spdlog::info("[children] {} root='{}' n={}", cls, reinterpret_cast<SDK::UObject*>(root)->GetName(), num);
    if (!PtrLooksValid(slotsData) || num <= 0 || num > 256) return;
    for (int i = 0; i < num; ++i) {
        SDK::UObject* slot = reinterpret_cast<SDK::UObject**>(slotsData)[i];
        if (!PtrLooksValid(slot)) continue;
        SDK::UObject* child = RdMem<SDK::UObject*>(slot, 0x30);   // UPanelSlot::Content
        if (PtrLooksValid(child)) spdlog::info("   {} child[{}] '{}'", cls, i, child->GetName());
    }
}

// Full-screen child overlays that must NOT be pillarboxed (damage/screen vignettes). When the HUD
// root is constrained to 16:9 they get squeezed too, so we counter-scale them back to full width.
static SDK::UObject* gSetRenderScaleFn2 = nullptr;
static bool IsVignetteName(const std::string& n)
{
    // Full-screen overlay widgets inside the non-clipping gameplay HUD that must cover the whole
    // screen (e.g. the damage vignette). Menu BackgroundBlur dims sample their layout geometry,
    // which the 16:9 viewport slot bounds, so they can't be extended this way and are left as-is.
    return n.find("dying") != std::string::npos || n.find("Dying") != std::string::npos ||
           n.find("Vignette") != std::string::npos || n.find("vignette") != std::string::npos ||
           n.find("Damage") != std::string::npos || n.find("ScreenEffect") != std::string::npos;
}

// Disable clip-to-bounds on a widget so overflowing full-screen children can render past 16:9.
static SDK::UObject* gSetClippingFn = nullptr;
static constexpr int OFF_Widget_Clipping = 0xDB;   // EWidgetClipping (uint8); Inherit=0
static void DisableClip(SDK::UObject* w)
{
    if (!PtrLooksValid(w)) return;
    uint8_t cur = RdMem<uint8_t>(w, OFF_Widget_Clipping);
    if (cur == 0) return;   // already Inherit (no clip)
    if (!gSetClippingFn) {
        SDK::UClass* c = ObjClass(w);
        if (PtrLooksValid(c)) gSetClippingFn = FindUFunction(c, "SetClipping");
        if (!gSetClippingFn) return;
    }
    uint8_t inherit[4] = { 0, 0, 0, 0 };   // EWidgetClipping::Inherit
    ProcessEvent_sh.thiscall<void>(w, gSetClippingFn, inherit);
    spdlog::info("  disabled clip (was {}) on '{}'", (int)cur, w->GetName());
}

static SDK::UObject* gSlotSetAnchorsFn = nullptr;
static void CounterScaleVignettes(void* hud)
{
    if (iCurrentResX <= 0 || fHUDWidth <= 0.0f) return;
    SDK::UObject* tree = RdMem<SDK::UObject*>(hud, OFF_UserWidget_WidgetTree);
    if (!PtrLooksValid(tree)) return;
    SDK::UWidget* root = RdMem<SDK::UWidget*>(tree, OFF_WidgetTree_RootWidget);
    if (!PtrLooksValid(root)) return;
    void* slotsData = RdMem<void*>(root, OFF_PanelWidget_Slots);
    int num = RdMem<int32_t>(root, OFF_PanelWidget_Slots + 0x08);
    if (!PtrLooksValid(slotsData) || num <= 0 || num > 256) return;

    const bool ultrawide = (fAspectRatio > fNativeAspect);
    const double span = ultrawide ? ((double)iCurrentResX / (double)fHUDWidth)
                                   : ((double)iCurrentResY / (double)fHUDHeight);   // 1.333
    const double off = (span - 1.0) / 2.0;                                          // 0.1667
    double scale[2] = { ultrawide ? span : 1.0, ultrawide ? 1.0 : span };
    // Overflow anchors expand the child past the constrained 16:9 parent back to full screen.
    double ovr[4] = { 0.0, 0.0, 1.0, 1.0 };
    if (ultrawide) { ovr[0] = -off; ovr[2] = 1.0 + off; }
    else           { ovr[1] = -off; ovr[3] = 1.0 + off; }

    for (int i = 0; i < num; ++i) {
        SDK::UObject* slot0 = reinterpret_cast<SDK::UObject**>(slotsData)[i];
        if (!PtrLooksValid(slot0)) continue;
        SDK::UObject* child = RdMem<SDK::UObject*>(slot0, 0x30);   // UPanelSlot::Content
        if (!PtrLooksValid(child)) continue;
        std::string cn = child->GetName();
        if (!IsVignetteName(cn)) continue;

        // Preferred: overflow the child's own canvas slot anchors (layout, survives animations).
        SDK::UObject* childSlot = RdMem<SDK::UObject*>(child, OFF_Widget_Slot);
        bool didAnchor = false;
        if (PtrLooksValid(childSlot) && ObjectIsA(childSlot, "CanvasPanelSlot")) {
            if (!gSlotSetAnchorsFn) {
                SDK::UClass* sc = ObjClass(childSlot);
                if (PtrLooksValid(sc)) gSlotSetAnchorsFn = FindUFunction(sc, "SetAnchors");
            }
            if (gSlotSetAnchorsFn) {
                ProcessEvent_sh.thiscall<void>(childSlot, gSlotSetAnchorsFn, ovr);
                didAnchor = true;
                spdlog::info("Vignette '{}' anchors->overflow ({:.3f}..{:.3f})", cn, ovr[0], ovr[2]);
            }
        }
        // Fallback (overlay children with no canvas slot): render scale.
        if (!didAnchor) {
            if (!gSetRenderScaleFn2) {
                SDK::UClass* c = ObjClass(child);
                if (PtrLooksValid(c)) gSetRenderScaleFn2 = FindUFunction(c, "SetRenderScale");
            }
            if (gSetRenderScaleFn2) {
                ProcessEvent_sh.thiscall<void>(child, gSetRenderScaleFn2, scale);
                spdlog::info("Vignette '{}' render-scale ({:.3f},{:.3f})", cn, scale[0], scale[1]);
            }
        }
    }
}

static void ConstrainIfHud(void* widget)   // std::string -> no __try here
{
    if (!PtrLooksValid(widget) || iCurrentResX <= 0) return;
    if (gDoneConstrain.count(widget)) return;
    std::string name = reinterpret_cast<SDK::UObject*>(widget)->GetName();
    if (!ShouldConstrain(name)) return;
    DumpChildren(widget, name);

    if (!gSetAnchorsFn) {
        SDK::UClass* c = ObjClass(reinterpret_cast<SDK::UObject*>(widget));
        if (PtrLooksValid(c)) gSetAnchorsFn = FindUFunction(c, "SetAnchorsInViewport");
        if (!gSetAnchorsFn) { spdlog::warn("SetAnchorsInViewport fn not found"); return; }
    }

    const bool ultrawide = (fAspectRatio > fNativeAspect);
    const double offX = (double)fHUDWidthOffset / (double)iCurrentResX;
    const double offY = (double)fHUDHeightOffset / (double)iCurrentResY;
    // FAnchors = { FVector2D Minimum; FVector2D Maximum } and UE5 FVector2D is DOUBLE precision,
    // so this is 4 doubles: {Min.X, Min.Y, Max.X, Max.Y}.
    double anc[4] = { 0.0, 0.0, 1.0, 1.0 };
    if (ultrawide) { anc[0] = offX; anc[2] = 1.0 - offX; }
    else           { anc[1] = offY; anc[3] = 1.0 - offY; }

    ProcessEvent_sh.thiscall<void>(reinterpret_cast<SDK::UObject*>(widget), gSetAnchorsFn, anc);
    gDoneConstrain.insert(widget);
    spdlog::info("Constrained '{}' -> anchors ({:.3f},{:.3f})-({:.3f},{:.3f}) [doubles]", name, anc[0], anc[1], anc[2], anc[3]);

    CounterScaleVignettes(widget);   // restore full-screen overlays (e.g. the HUD damage vignette)
}

// UGameViewportSubsystem::AddWidget(rcx=subsystem, rdx=widget, r8=?, r9=slot template)
void __fastcall AddToScreen_hk(void* subsystem, void* widget, void* r8, void* tmpl)
{
    __try { LogAddToScreen(widget, tmpl); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    AddToScreen_sh.thiscall<void>(subsystem, widget, r8, tmpl);
    if (bFixHUD) { __try { ConstrainIfHud(widget); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
}

void HookAddToScreen()
{
    std::uint8_t* fn = (std::uint8_t*)exeModule + OFF_AddToScreen;
    AddToScreen_sh = safetyhook::create_inline(reinterpret_cast<void*>(fn), reinterpret_cast<void*>(AddToScreen_hk));
    spdlog::info("AddToScreen hook ({:s}+{:x}): {}", sExeName.c_str(), (uintptr_t)OFF_AddToScreen,
        (bool)AddToScreen_sh ? "ok" : "FAILED");
}

DWORD __stdcall Main(void*)
{
    Logging();
    Configuration();
    SetElliotOffsets();

    if (bInstallResHook) {
        CurrentResolution();
    } else {
        DesktopDimensions = Util::GetPhysicalDesktopDimensions();
        iCurrentResX = DesktopDimensions.first;
        iCurrentResY = DesktopDimensions.second;
        CalculateAspectRatio(true);
        spdlog::info("Resolution hook disabled (probe). Using desktop resolution.");
    }

    if (bFixHUD) { HookProcessEvent(); HookAddToScreen(); }

    spdlog::info("HUDFix initialised. ResHook={} Sweep={}", bInstallResHook, bEnableSweep);

    if (!bEnableSweep) {
        spdlog::info("AddToScreen RE diagnostic active; sweep disabled. Idling.");
        return true;
    }

    Sleep(iSweepStartDelayMs > 0 ? iSweepStartDelayMs : 20000);

    for (int pass = 0; pass < 120; ++pass) {  // ~4 min of periodic sweeps
        size_t before = gLoggedNames.size();
        DiagnosticSweep();
        size_t after = gLoggedNames.size();
        if (after != before)
            spdlog::info("--- sweep {}: {} new widget(s), {} total ---", pass, after - before, after);
        Sleep(iSweepIntervalMs > 0 ? iSweepIntervalMs : 2000);
    }
    spdlog::info("Diagnostic sweeps finished.");
    return true;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH: {
        thisModule = hModule;
        HANDLE mainHandle = CreateThread(NULL, 0, Main, 0, NULL, 0);
        if (mainHandle) CloseHandle(mainHandle);
        break;
    }
    }
    return TRUE;
}
