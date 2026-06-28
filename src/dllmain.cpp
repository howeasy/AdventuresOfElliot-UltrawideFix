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
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <vector>
#include <clocale>
#include <chrono>

#define spdlog_confparse(var) spdlog::info("Config Parse: {}: {}", #var, var)

HMODULE exeModule = GetModuleHandle(NULL);
HMODULE thisModule;

std::string sFixName = "HUDFix";
std::string sFixVersion = "1.2.0";
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
int  iMarkerShiftPx = -1;   // interact-prompt anchor shift in px; -1 = auto (= HUD pillarbox offset)

// ===================== Dynamic rule engine (data-driven HUD element fixes) =====================
// Every per-element adjustment is a Rule parsed from a [Rule.*] section of HUDFix.ini, matched
// against widget names as the HUD tree is walked. One op per rule:
//   move       - render-scale once (preserves the widget's own animation) + reposition by Offset.
//                Canvas-slot widgets move via SetPosition (stable layout channel, re-applied so a
//                game re-layout can't knock them); overlay widgets move via a one-shot translation.
//   hide       - SetRenderOpacity(0), re-applied so it stays hidden.
//   fullscreen - expand a child back past the 16:9 band to full screen (damage vignette, frames).
//   marker     - world-tracked prompt (interact icon, magic-unavailable, ...): translate its inner
//                content root by the pillarbox shift, re-applied each frame.
//   nocrop     - exclude this top-level widget from the 16:9 constraint entirely (keep it full-screen,
//                e.g. world-tracked speech/emoticon bubbles that must track NPCs across the full width).
enum { OP_MOVE = 0, OP_HIDE = 1, OP_FULLSCREEN = 2, OP_MARKER = 3, OP_NOCROP = 4 };
struct Rule {
    std::vector<std::string> match;     // ANY of these matches the widget name (substring or exact)
    bool   exact = false;
    int    op = OP_MOVE;
    double scale = 1.0, pivotX = 0.5, pivotY = 0.5, offX = 0.0, offY = 0.0;
    std::string label;
};
static std::vector<Rule> gRules;

static std::vector<std::string> SplitCsv(std::string s)
{
    std::vector<std::string> out; size_t p;
    while ((p = s.find(',')) != std::string::npos) { out.push_back(s.substr(0, p)); s.erase(0, p + 1); }
    out.push_back(s);
    std::vector<std::string> r;
    for (auto& t : out) {
        size_t a = t.find_first_not_of(" \t"); if (a == std::string::npos) continue;
        size_t b = t.find_last_not_of(" \t"); r.push_back(t.substr(a, b - a + 1));
    }
    return r;
}
static void ParseRules()   // called from Configuration() after the ini is parsed
{
    std::setlocale(LC_NUMERIC, "C");   // std::stod below must use '.' decimals regardless of game locale
    gRules.clear();
    for (auto& sec : ini.sections) {
        if (sec.first.rfind("Rule.", 0) != 0) continue;   // sections named [Rule.<anything>]
        auto& kv = sec.second;
        auto S = [&](const char* k) -> std::string { auto it = kv.find(k); return it != kv.end() ? it->second : std::string(); };
        Rule r; r.label = sec.first;
        r.match = SplitCsv(S("Match"));
        if (r.match.empty()) continue;
        std::string ex = S("Exact"); for (auto& c : ex) if (c >= 'A' && c <= 'Z') c += 32;
        r.exact = (ex == "true" || ex == "1");
        std::string op = S("Op"); for (auto& c : op) if (c >= 'A' && c <= 'Z') c += 32;
        r.op = (op == "hide") ? OP_HIDE : (op == "fullscreen") ? OP_FULLSCREEN : (op == "marker") ? OP_MARKER
             : (op == "nocrop") ? OP_NOCROP : OP_MOVE;
        auto D = [&](const char* k, double& out) { auto it = kv.find(k); if (it != kv.end()) { try { out = std::stod(it->second); } catch (...) {} } };
        D("Scale", r.scale); D("PivotX", r.pivotX); D("PivotY", r.pivotY); D("OffsetX", r.offX); D("OffsetY", r.offY);
        gRules.push_back(r);
    }
}

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
        // Flush only on warnings/errors + every 2s (NOT per info line) — a synchronous per-line flush
        // during a HUD reload (fast travel) blocks the game thread into a lockup.
        spdlog::flush_on(spdlog::level::warn);
        spdlog::flush_every(std::chrono::seconds(2));
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
        inipp::get_value(ini.sections["Fix HUD"], "MarkerShiftPx", iMarkerShiftPx);
        inipp::get_value(ini.sections["Debug"], "InstallResHook", bInstallResHook);
        inipp::get_value(ini.sections["Debug"], "EnableSweep", bEnableSweep);
        inipp::get_value(ini.sections["Debug"], "SweepStartDelayMs", iSweepStartDelayMs);
        ParseRules();
    }
    spdlog_confparse(bFixHUD);
    spdlog_confparse(bDiagnostic);
    spdlog_confparse(iSweepIntervalMs);
    spdlog_confparse(iMarkerShiftPx);
    spdlog::info("Parsed {} rule(s):", gRules.size());
    for (auto& r : gRules) {
        std::string m; for (auto& x : r.match) m += (m.empty() ? "" : ",") + x;
        spdlog::info("  [{}] op={} match=[{}] exact={} scale={:.3f} pivot=({:.2f},{:.2f}) off=({:.0f},{:.0f})",
            r.label, r.op, m, r.exact, r.scale, r.pivotX, r.pivotY, r.offX, r.offY);
    }
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


// ---- Diagnostic: watch how the game positions widgets each frame (find the world-tracked Ⓧ) ----
// Resolved (canonical UWidget/UUserWidget UFunctions) in ConstrainIfHud once.
static SDK::UObject* gFnSetRenderTransform = nullptr;
static SDK::UObject* gFnSetRenderTranslation = nullptr;
static SDK::UObject* gFnSetPositionInViewport = nullptr;
static void* gHudWidget = nullptr;   // cached WB_HUDLayout for periodic tree dumps

// Recursive widget-tree snapshot: name, slot kind, live canvas position/anchors, render translation.
// Run periodically while the Ⓧ is on screen so the widget sitting at its (wrong) location stands out.
static void DumpTree(SDK::UObject* w, int depth, int& budget)
{
    if (!PtrLooksValid(w) || budget <= 0 || depth > 9) return;
    --budget;
    std::string nm = w->GetName();
    SDK::UObject* slot = RdMem<SDK::UObject*>(w, OFF_Widget_Slot);   // UPanelSlot* @0x30
    float rx = RdMem<float>(w, 0x90), ry = RdMem<float>(w, 0x94);    // RenderTransform.Translation
    if (PtrLooksValid(slot) && ObjectIsA(slot, "CanvasPanelSlot")) {
        uint8_t* ld = reinterpret_cast<uint8_t*>(slot) + OFF_CanvasSlot_LayoutData;
        float*  o = reinterpret_cast<float*>(ld);                              // L,T,R,B (pos/size)
        double* a = reinterpret_cast<double*>(ld + OFF_Anchors_in_LayoutData); // anchors
        spdlog::info("{:>{}}{} [canvas pos=({:.0f},{:.0f}) sz=({:.0f},{:.0f}) anc=({:.2f},{:.2f})-({:.2f},{:.2f})] rt=({:.0f},{:.0f})",
            "", depth * 2, nm, o[0], o[1], o[2], o[3], a[0], a[1], a[2], a[3], rx, ry);
    } else {
        std::string st = PtrLooksValid(slot) ? reinterpret_cast<SDK::UObject*>(slot)->GetName() : "none";
        spdlog::info("{:>{}}{} [{}] rt=({:.0f},{:.0f})", "", depth * 2, nm, st, rx, ry);
    }
    if (ObjectIsA(w, "UserWidget")) {
        SDK::UObject* tree = RdMem<SDK::UObject*>(w, OFF_UserWidget_WidgetTree);
        SDK::UWidget* root = PtrLooksValid(tree) ? RdMem<SDK::UWidget*>(tree, OFF_WidgetTree_RootWidget) : nullptr;
        if (PtrLooksValid(root)) DumpTree(reinterpret_cast<SDK::UObject*>(root), depth + 1, budget);
    }
    if (ObjectIsA(w, "PanelWidget")) {
        void* slotsData = RdMem<void*>(w, OFF_PanelWidget_Slots);
        int num = RdMem<int32_t>(w, OFF_PanelWidget_Slots + 0x08);
        if (PtrLooksValid(slotsData) && num > 0 && num <= 256) {
            for (int i = 0; i < num && budget > 0; ++i) {
                SDK::UObject* s = reinterpret_cast<SDK::UObject**>(slotsData)[i];
                if (!PtrLooksValid(s)) continue;
                SDK::UObject* ch = RdMem<SDK::UObject*>(s, 0x30);   // UPanelSlot::Content
                if (PtrLooksValid(ch)) DumpTree(ch, depth + 1, budget);
            }
        }
    }
}
static void DumpTreeTick()
{
  try {
    if (!PtrLooksValid(gHudWidget)) return;
    static uint64_t lastDump = 0; static int dumps = 0;
    uint64_t now = GetTickCount64();
    if (dumps >= 30 || now - lastDump < 2000) return;
    lastDump = now; ++dumps;
    spdlog::info("=== TREE dump #{} (HUD descendants) ===", dumps);
    int budget = 700;
    DumpTree(reinterpret_cast<SDK::UObject*>(gHudWidget), 0, budget);
  } catch (...) {}
}

// ---- rule engine state + per-frame re-assert (the apply/match functions live further down) ----
struct RuleTarget { void* w; int rule; bool captured; bool overlay; float natL, natT; std::string name; };
static std::vector<RuleTarget> gRuleTargets;                              // re-apply cache (HUD-root only)
static std::unordered_map<void*, std::pair<float, float>> gRuleNatural;   // widget -> true natural slot pos
static double gMarkTrX = 0.0, gMarkTrY = 0.0;                             // marker shift (set in ApplyRules)

// Shared, lazily-resolved UFunctions (render-level on UWidget; slot-level on UCanvasPanelSlot).
static SDK::UObject* gFnRenderScale = nullptr, * gFnRenderPivot = nullptr, * gFnRenderTrans = nullptr,
                   * gFnRenderOpacity = nullptr, * gFnSlotSetPos = nullptr, * gFnSlotSetAnchors = nullptr;
static SDK::UObject* ResolveFn(SDK::UObject*& cache, SDK::UObject* fromObj, const char* name)
{
    if (!cache && PtrLooksValid(fromObj)) { SDK::UClass* c = ObjClass(fromObj); if (PtrLooksValid(c)) cache = FindUFunction(c, name); }
    return cache;
}
static SDK::UObject* InnerRoot(SDK::UObject* w)   // a UserWidget's content root (else the widget itself)
{
    SDK::UObject* tree = RdMem<SDK::UObject*>(w, OFF_UserWidget_WidgetTree);
    SDK::UWidget* root = PtrLooksValid(tree) ? RdMem<SDK::UWidget*>(tree, OFF_WidgetTree_RootWidget) : nullptr;
    return PtrLooksValid(root) ? reinterpret_cast<SDK::UObject*>(root) : w;
}

// Re-assert one cached target each tick. Keeps moves/markers/hides locked against game-driven
// re-layout WITHOUT re-touching render scale (that would freeze the widget's own idle animation).
static void ReassertTarget(RuleTarget& t)
{
    if (!PtrLooksValid(t.w) || t.rule < 0 || t.rule >= (int)gRules.size()) return;
    SDK::UObject* w = reinterpret_cast<SDK::UObject*>(t.w);
    // Identity check: on fast travel the old HUD is freed for the seconds the new level loads, leaving
    // this cached pointer dangling. Only touch it if it is still the SAME live widget (name matches) —
    // a freed/reused address won't match, so we never call a UFunction on garbage (which can hang the
    // game's layout). A faulting GetName on an unmapped address is caught by RulesTick's SEH.
    if (w->GetName() != t.name) {
        if (bDiagnostic) { static std::unordered_set<std::string> warned;
            if (warned.insert(t.name).second) spdlog::info("REASSERT skip(name) cached='{}' live='{}'", t.name, w->GetName()); }
        return;
    }
    const Rule& r = gRules[t.rule];
    if (bDiagnostic) { static std::unordered_set<std::string> okd;
        if (okd.insert(t.name).second) spdlog::info("REASSERT ok '{}' op={} captured={}", t.name, r.op, (int)t.captured); }
    if (r.op == OP_MARKER) {
        SDK::UObject* inner = InnerRoot(w);
        if (gFnRenderTrans) { double tr[2] = { (double)t.natL, (double)t.natT }; ProcessEvent_sh.thiscall<void>(inner, gFnRenderTrans, tr); }
    } else if (r.op == OP_HIDE) {
        if (gFnRenderOpacity) { float op = 0.0f; ProcessEvent_sh.thiscall<void>(w, gFnRenderOpacity, &op); }
    } else if (r.op == OP_MOVE && t.captured) {   // canvas-slot move: re-pin position via SetPosition
        SDK::UObject* slot = RdMem<SDK::UObject*>(w, OFF_Widget_Slot);
        if (PtrLooksValid(slot) && gFnSlotSetPos && ObjectIsA(slot, "CanvasPanelSlot")) {
            double pos[2] = { (double)t.natL + r.offX, (double)t.natT + r.offY };
            ProcessEvent_sh.thiscall<void>(slot, gFnSlotSetPos, pos);
        }
    } else if (r.op == OP_MOVE && t.overlay) {    // overlay move: re-pin via inner-root render-translation
        SDK::UObject* inner = InnerRoot(w);        // natL/natT hold the offset for overlay targets
        if (gFnRenderTrans) { double tr[2] = { (double)t.natL, (double)t.natT }; ProcessEvent_sh.thiscall<void>(inner, gFnRenderTrans, tr); }
    }
}
// Per-frame re-assert. Cached widget pointers can go stale between an old HUD being freed and the new
// one being constrained, so each re-assert is wrapped in SEH (C++ catch(...) does NOT catch access
// violations under /EHsc). Index loop (not range-for) keeps this function SEH-compatible (no unwinding
// locals). Re-entrancy is prevented by tlInApply in the hooks, so gRuleTargets is never mutated here.
static void RulesTick()
{
    if (gRules.empty() || gRuleTargets.empty()) return;
    static uint64_t last = 0; uint64_t now = GetTickCount64();
    if (now - last < 150) return; last = now;
    size_t n = gRuleTargets.size();
    for (size_t i = 0; i < n; ++i) {
        __try { ReassertTarget(gRuleTargets[i]); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}
static std::unordered_map<std::string, int> gPosLogCount;   // throttle per (object,fn)
static const char* PosFnTag(SDK::UObject* fn)
{
    if (fn == gFnSetRenderTransform)    return "SetRenderTransform";
    if (fn == gFnSetRenderTranslation)  return "SetRenderTranslation";
    if (fn == gFnSetPositionInViewport) return "SetPositionInViewport";
    return "?";
}
static void PosLog(SDK::UObject* obj, SDK::UObject* fn, void* params)
{
  try {
    if (!PtrLooksValid(obj) || !PtrLooksValid(params)) return;
    std::string n = obj->GetName();
    const char* tag = PosFnTag(fn);
    int& c = gPosLogCount[n + "|" + tag];
    if (c >= 8) return;                 // a few samples so we can see coords change with the world
    ++c;
    float*  f = reinterpret_cast<float*>(params);
    double* d = reinterpret_cast<double*>(params);
    spdlog::info("POS {} '{}' f=[{:.1f},{:.1f},{:.1f},{:.1f}] d=[{:.1f},{:.1f}]",
        tag, n, f[0], f[1], f[2], f[3], d[0], d[1]);
  } catch (...) {}
}

// We need the trampoline (ProcessEvent_sh) so the AddWidget hook can call game UFunctions on the
// game thread; with Diagnostic on we also sample the game's own positioning calls to find markers.
void __fastcall ProcessEvent_hk(SDK::UObject* obj, SDK::UObject* fn, void* params)
{
    // tlInApply guards against re-entrancy: our UFunction calls (trampoline) can make the game dispatch
    // nested ProcessEvents through this hook. Skip all engine work on re-entry so gRuleTargets is never
    // mutated mid-iteration and we don't recurse RulesTick/ApplyRules.
    if (!tlInApply) {
        tlInApply = true;
        __try {
            if (bDiagnostic && params &&
                (fn == gFnSetRenderTransform || fn == gFnSetRenderTranslation || fn == gFnSetPositionInViewport))
                PosLog(obj, fn, params);
            RulesTick();         // functional: re-asserts all dynamic rule targets (markers/moves/hides)
            if (bDiagnostic) DumpTreeTick();   // diagnostic: capture HUD tree to identify new widgets
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        tlInApply = false;   // always reset, even if a diagnostic call SEH-faulted
    }
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
  try {
    if (!PtrLooksValid(widget)) return;
    if (gSeenAdd.count(widget)) return;
    gSeenAdd.insert(widget);
    std::string name = reinterpret_cast<SDK::UObject*>(widget)->GetName();
    if (!PtrLooksValid(tmpl)) { spdlog::info("AddWidget '{}' tmpl=invalid({})", name, tmpl); return; }
    float* f = reinterpret_cast<float*>(tmpl);
    // dump first 0x40 bytes as floats so we can see the real FGameViewportWidgetSlot/FAnchorData layout
    spdlog::info("AddWidget '{}' tmpl={} | [0x00]={:.3f},{:.3f},{:.3f},{:.3f} [0x10]={:.3f},{:.3f},{:.3f},{:.3f} [0x20]={:.3f},{:.3f},{:.3f},{:.3f} [0x30]={:.3f},{:.3f},{:.3f},{:.3f}",
        name, tmpl, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
  } catch (...) {}
}

// Constrain a just-added viewport widget to a centred 16:9 band via SetAnchorsInViewport.
// Targeted by name (only the gameplay HUD root by default). Runs on the game thread, right after
// the slot is created (offsets default to 0) so a clean anchor change fills a 16:9 box.
static SDK::UObject* gSetAnchorsFn = nullptr;
static std::unordered_set<void*> gDoneConstrain;

// Constrain every top-level UI widget EXCEPT full-screen overlays (fades/movies/loading), which
// must keep covering the whole screen.
// A 'nocrop' rule excludes a top-level widget from the 16:9 constraint (keeps it full-screen).
static bool MatchesNoCrop(const std::string& n)
{
    for (auto& r : gRules)
        if (r.op == OP_NOCROP)
            for (auto& m : r.match)
                if (r.exact ? (n == m) : (n.find(m) != std::string::npos)) return true;
    return false;
}
static bool ShouldConstrain(const std::string& n)
{
    return !NameExcluded(n) && !MatchesNoCrop(n);
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
  try {
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
        if (!PtrLooksValid(child)) continue;
        // For CanvasPanelSlot children, also log anchors + offsets so we can tell which children are
        // corner-anchored (decorations/borders) or world-tracked (markers) vs stretched HUD layers.
        // LayoutData @0x38: FMargin Offsets(4 float @0x00), FAnchors(4 double @0x10), FVector2D Align(@0x30).
        if (ObjectIsA(slot, "CanvasPanelSlot")) {
            uint8_t* ld = reinterpret_cast<uint8_t*>(slot) + OFF_CanvasSlot_LayoutData;
            float*  offs = reinterpret_cast<float*>(ld);                                   // L,T,R,B
            double* anc  = reinterpret_cast<double*>(ld + OFF_Anchors_in_LayoutData);       // MinX,MinY,MaxX,MaxY
            spdlog::info("   {} child[{}] '{}' anchors=({:.3f},{:.3f})-({:.3f},{:.3f}) offs=({:.1f},{:.1f},{:.1f},{:.1f})",
                cls, i, child->GetName(), anc[0], anc[1], anc[2], anc[3], offs[0], offs[1], offs[2], offs[3]);
        } else {
            spdlog::info("   {} child[{}] '{}' slot='{}'", cls, i, child->GetName(),
                reinterpret_cast<SDK::UObject*>(slot)->GetName());
        }
    }
  } catch (...) {}
}

// ---- rule application (initial, walked during constrain) ----

// fullscreen op: expand a child back past the constrained 16:9 band to full screen. Canvas-slot
// children overflow their anchors (layout, survives animations); overlay children render-scale.
static void ApplyFullscreen(SDK::UObject* child)
{
    const bool uw = (fAspectRatio > fNativeAspect);
    const double span = uw ? ((double)iCurrentResX / (double)fHUDWidth) : ((double)iCurrentResY / (double)fHUDHeight);
    const double off = (span - 1.0) / 2.0;
    SDK::UObject* slot = RdMem<SDK::UObject*>(child, OFF_Widget_Slot);
    if (PtrLooksValid(slot) && ObjectIsA(slot, "CanvasPanelSlot")) {
        double ovr[4] = { 0.0, 0.0, 1.0, 1.0 };
        if (uw) { ovr[0] = -off; ovr[2] = 1.0 + off; } else { ovr[1] = -off; ovr[3] = 1.0 + off; }
        if (ResolveFn(gFnSlotSetAnchors, slot, "SetAnchors")) ProcessEvent_sh.thiscall<void>(slot, gFnSlotSetAnchors, ovr);
    } else {
        double sc[2] = { uw ? span : 1.0, uw ? 1.0 : span };
        if (ResolveFn(gFnRenderScale, child, "SetRenderScale")) ProcessEvent_sh.thiscall<void>(child, gFnRenderScale, sc);
    }
}

// Match a widget name to a rule. Exact matches always win over substring matches (independent of
// section/file order, since inipp iterates sections alphabetically); first substring otherwise.
static int MatchRuleIdx(const std::string& name)
{
    int sub = -1;
    for (size_t i = 0; i < gRules.size(); ++i)
        for (auto& m : gRules[i].match) {
            if (gRules[i].exact) { if (name == m) return (int)i; }
            else if (sub < 0 && name.find(m) != std::string::npos) sub = (int)i;
        }
    return sub;
}

// Apply a matched rule's op to a widget. When collect=true (HUD root), also cache it for per-frame
// re-assert. Scale is applied ONCE (re-applying it would freeze the widget's idle animation); the
// position/marker/hide parts are what get re-asserted in ReassertTarget.
static void ApplyRuleToWidget(SDK::UObject* w, int ruleIdx, bool collect)
{
    const Rule& r = gRules[ruleIdx];
    if (r.op == OP_NOCROP) return;   // handled at constraint time (ShouldConstrain); nothing to apply here
    if (bDiagnostic) {               // log each unique rule->widget once (never per-constrain) to avoid a storm
        static std::unordered_set<std::string> logged;
        std::string k = r.label + ">" + w->GetName();
        if (logged.insert(k).second) spdlog::info("Rule [{}] -> '{}' op={}", r.label, w->GetName(), r.op);
    }
    RuleTarget t{ w, ruleIdx, false, false, 0.0f, 0.0f, w->GetName() };   // name = identity check for re-assert
    if (r.op == OP_HIDE) {
        if (ResolveFn(gFnRenderOpacity, w, "SetRenderOpacity")) { float op = 0.0f; ProcessEvent_sh.thiscall<void>(w, gFnRenderOpacity, &op); }
    } else if (r.op == OP_FULLSCREEN) {
        ApplyFullscreen(w);
    } else if (r.op == OP_MARKER) {
        // Per-axis shift override: a non-zero rule Offset on an axis replaces the global MarkerShiftPx
        // on that axis (so you can nudge Y alone). Stored in t for the per-frame re-assert.
        double mx = (r.offX != 0.0) ? r.offX : gMarkTrX;
        double my = (r.offY != 0.0) ? r.offY : gMarkTrY;
        t.natL = (float)mx; t.natT = (float)my;
        SDK::UObject* inner = InnerRoot(w);
        if (ResolveFn(gFnRenderTrans, inner, "SetRenderTranslation")) { double tr[2] = { mx, my }; ProcessEvent_sh.thiscall<void>(inner, gFnRenderTrans, tr); }
    } else { // OP_MOVE: render-scale once + reposition (applied for EVERY match, not just the HUD root)
        if (r.scale != 1.0) {
            if (ResolveFn(gFnRenderPivot, w, "SetRenderTransformPivot")) { double pv[2] = { r.pivotX, r.pivotY }; ProcessEvent_sh.thiscall<void>(w, gFnRenderPivot, pv); }
            if (ResolveFn(gFnRenderScale, w, "SetRenderScale")) { double sc[2] = { r.scale, r.scale }; ProcessEvent_sh.thiscall<void>(w, gFnRenderScale, sc); }
        }
        if (r.offX != 0.0 || r.offY != 0.0) {
            SDK::UObject* slot = RdMem<SDK::UObject*>(w, OFF_Widget_Slot);
            if (PtrLooksValid(slot) && ObjectIsA(slot, "CanvasPanelSlot")) {
                // Canvas slot: move via SetPosition (stable, invalidates layout). Capture the TRUE natural
                // position once per widget (persist across re-constrains within a HUD gen) so it can't compound.
                float* off = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(slot) + OFF_CanvasSlot_LayoutData);
                auto it = gRuleNatural.find(w);
                if (it == gRuleNatural.end()) { gRuleNatural[w] = { off[0], off[1] }; it = gRuleNatural.find(w); }
                t.natL = it->second.first; t.natT = it->second.second; t.captured = true;
                bool posOk = ResolveFn(gFnSlotSetPos, slot, "SetPosition") != nullptr;
                if (posOk) {
                    double pos[2] = { (double)t.natL + r.offX, (double)t.natT + r.offY };
                    ProcessEvent_sh.thiscall<void>(slot, gFnSlotSetPos, pos);
                }
                if (bDiagnostic) { static std::unordered_set<std::string> mlog;
                    if (mlog.insert(w->GetName()).second) spdlog::info("MOVE '{}' canvas nat=({:.0f},{:.0f}) off=({:.0f},{:.0f}) -> ({:.0f},{:.0f}) scaleFn={} posFn={}",
                        w->GetName(), t.natL, t.natT, r.offX, r.offY, t.natL + r.offX, t.natT + r.offY, (void*)gFnRenderScale, (void*)gFnSlotSetPos); }
            } else {
                // Overlay slot has no movable layout position. Translate the widget's INNER content root and
                // re-assert it each frame (same channel as the interact marker): an overlay panel is often
                // shown LATER than the HUD-load walk and its OUTER transform is reset on show, so a one-shot
                // outer translate is lost. The inner WBP content is left alone by the game, so it sticks.
                SDK::UObject* inner = InnerRoot(w);
                t.overlay = true; t.natL = (float)r.offX; t.natT = (float)r.offY;
                if (ResolveFn(gFnRenderTrans, inner, "SetRenderTranslation")) { double tr[2] = { r.offX, r.offY }; ProcessEvent_sh.thiscall<void>(inner, gFnRenderTrans, tr); }
                if (bDiagnostic) { static std::unordered_set<std::string> olog;
                    if (olog.insert(w->GetName()).second) spdlog::info("MOVE '{}' OVERLAY inner-root translate off=({:.0f},{:.0f}) re-asserted", w->GetName(), r.offX, r.offY); }
            }
        }
    }
    // Cache for per-frame re-assert (HUD root only). Fullscreen persists one-shot so it needn't re-assert.
    if (collect && r.op != OP_FULLSCREEN) {
        for (auto& e : gRuleTargets) if (e.w == w) return;   // dedupe
        gRuleTargets.push_back(t);
    }
}

// Walk a widget subtree applying rules. budget caps the TOTAL nodes visited: during a level transition
// (fast travel) the widget tree is mid-construction, so RdMem can read garbage that looks like a panel
// with hundreds of children pointing to more garbage panels — without a total-node cap the recursion
// explodes (256^depth) and the game freezes. The real HUD is well under the cap.
static void ApplyRulesRec(SDK::UObject* w, int depth, bool collect, int& budget, bool insideMoved)
{
    if (!PtrLooksValid(w) || depth > 24 || budget <= 0) return;
    --budget;
    int idx = MatchRuleIdx(w->GetName());
    if (idx >= 0) {
        const int op = gRules[idx].op;
        if (op != OP_MOVE) {
            // hide/fullscreen/marker/nocrop claim their whole subtree.
            ApplyRuleToWidget(w, idx, collect);
            return;
        }
        // OP_MOVE: apply ONLY the outermost move, then keep descending. A descendant that also matches a
        // move rule must NOT be transformed again — it already inherits this widget's render scale, so a
        // second scale/reposition double-shrinks and shifts it (the fairy "not moved properly" regression).
        // We still descend so deeper NON-move rules (e.g. hiding an internal Decoration_L) keep working.
        if (!insideMoved) { ApplyRuleToWidget(w, idx, collect); insideMoved = true; }
    }
    if (ObjectIsA(w, "UserWidget")) {
        SDK::UObject* root = InnerRoot(w);
        if (root != w) ApplyRulesRec(root, depth + 1, collect, budget, insideMoved);
    }
    if (ObjectIsA(w, "PanelWidget")) {
        void* sd = RdMem<void*>(w, OFF_PanelWidget_Slots);
        int num = RdMem<int32_t>(w, OFF_PanelWidget_Slots + 0x08);
        if (PtrLooksValid(sd) && num > 0 && num <= 256)
            for (int i = 0; i < num && budget > 0; ++i) {
                SDK::UObject* s = reinterpret_cast<SDK::UObject**>(sd)[i];
                if (!PtrLooksValid(s)) continue;
                SDK::UObject* ch = RdMem<SDK::UObject*>(s, 0x30);   // UPanelSlot::Content
                if (PtrLooksValid(ch)) ApplyRulesRec(ch, depth + 1, collect, budget, insideMoved);
            }
    }
}

// Entry point, called from ConstrainIfHud for every constrained top-level widget. The re-apply cache
// is refreshed only from the gameplay HUD root (which owns the markers/moves); other widgets' ops
// (menu vignettes etc.) are applied one-shot and persist on their own.
static void ApplyRules(void* hud, bool isHudRoot)
{
  try {
    if (gRules.empty() || iCurrentResX <= 0 || fHUDWidth <= 0.0f) return;
    const bool uw = (fAspectRatio > fNativeAspect);
    double shiftPx = (iMarkerShiftPx >= 0) ? (double)iMarkerShiftPx : (uw ? (double)fHUDWidthOffset : (double)fHUDHeightOffset);
    gMarkTrX = uw ? -shiftPx : 0.0;
    gMarkTrY = uw ? 0.0 : -shiftPx;
    if (isHudRoot) gRuleTargets.clear();   // refresh re-apply cache from the live HUD tree
    // Total-node cap. The real HUD is a few thousand nodes; a mid-construction garbage tree explodes to
    // 256^depth instantly, so any cap well above the real size stops the freeze without truncating the
    // legit walk. 8000 was too tight once 'move' ops started descending their full subtrees.
    int budget = 200000;
    int start = budget;
    ApplyRulesRec(reinterpret_cast<SDK::UObject*>(hud), 0, isHudRoot, budget, false);
    if (bDiagnostic) spdlog::info("ApplyRules('{}', hudRoot={}) walked {} nodes (budget left={})",
        reinterpret_cast<SDK::UObject*>(hud)->GetName(), isHudRoot, start - budget, budget);
  } catch (...) {}
}

static void ConstrainIfHud(void* widget)   // std::string -> no __try here
{
  try {
    if (!PtrLooksValid(widget) || iCurrentResX <= 0) return;
    // Only UUserWidgets have SetAnchorsInViewport; AddWidget can be handed other widget types.
    if (!ObjectIsA(reinterpret_cast<SDK::UObject*>(widget), "UserWidget")) return;
    std::string name = reinterpret_cast<SDK::UObject*>(widget)->GetName();
    if (!ShouldConstrain(name)) return;
    bool isHudRoot = (name.find("WB_HUDLayout") != std::string::npos);
    if (isHudRoot && widget != gHudWidget) {
        // New HUD generation: the previous HUD's widgets are freed. Flush every cache that keys on raw
        // widget pointers so a reused address can't (a) poison natural-position lookups, (b) falsely mark
        // a new widget already-constrained, or (c) be re-asserted as a stale target.
        gHudWidget = widget;
        gDoneConstrain.clear();
        gSeenAdd.clear();
        gRuleNatural.clear();
        gRuleTargets.clear();
    }
    if (gDoneConstrain.count(widget)) return;   // checked AFTER the gen flush so reuse can't suppress it
    DumpChildren(widget, name);

    if (!gSetAnchorsFn) {
        SDK::UClass* c = ObjClass(reinterpret_cast<SDK::UObject*>(widget));
        if (PtrLooksValid(c)) gSetAnchorsFn = FindUFunction(c, "SetAnchorsInViewport");
        if (!gSetAnchorsFn) { spdlog::warn("SetAnchorsInViewport fn not found"); return; }
    }

    if (bDiagnostic && !gFnSetRenderTransform) {
        SDK::UClass* wc = ObjClass(reinterpret_cast<SDK::UObject*>(widget));
        if (PtrLooksValid(wc)) {
            gFnSetRenderTransform    = FindUFunction(wc, "SetRenderTransform");
            gFnSetRenderTranslation  = FindUFunction(wc, "SetRenderTranslation");
            gFnSetPositionInViewport = FindUFunction(wc, "SetPositionInViewport");
            spdlog::info("PosLog fns: RT={} RTr={} PIV={}", (void*)gFnSetRenderTransform,
                (void*)gFnSetRenderTranslation, (void*)gFnSetPositionInViewport);
        }
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

    // Apply all INI rules (markers/moves/hides/fullscreen) to this widget's subtree. The re-apply
    // cache is refreshed only from the gameplay HUD root (it owns the world-tracked + moved widgets).
    ApplyRules(widget, isHudRoot);
  } catch (...) {}
}

// UGameViewportSubsystem::AddWidget(rcx=subsystem, rdx=widget, r8=?, r9=slot template)
void __fastcall AddToScreen_hk(void* subsystem, void* widget, void* r8, void* tmpl)
{
    __try { LogAddToScreen(widget, tmpl); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    AddToScreen_sh.thiscall<void>(subsystem, widget, r8, tmpl);
    // Same re-entrancy guard as ProcessEvent_hk: never mutate gRuleTargets while RulesTick iterates it.
    if (bFixHUD && !tlInApply) {
        tlInApply = true;
        __try { ConstrainIfHud(widget); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        tlInApply = false;
    }
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
