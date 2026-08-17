#include <array>
#include <fstream>
#include <sstream>
#include <iostream>
#include <regex>
#include <utility>

#include "config.h"

#include "../log/log.h"

#include "../util_env.h"

#include "../sha1/sha1_util.h"

namespace dxvk {

  using ProfileList = std::vector<std::pair<const char*, Config>>;


  const static ProfileList g_profiles = {

    { R"(\\Diablo IV\.exe$)", {{
      { "dxgi.hideNvidiaGpu",              "False" },
      { "dxvk.hideIntegratedGraphics",      "True" },
    }} },
    { R"(\\RiftApart\.exe$)", {{
      { "dxgi.hideNvidiaGpu",              "False" },
    }} },
    { R"(\\P3R\.exe$)", {{
      { "dxgi.syncInterval",                   "1" },
    }} },
    { R"(\\Wow(Classic)?\.exe$)", {{
      { "dxvk.hideIntegratedGraphics",      "True" },
    }} },
    { R"(\\BrightMemory_EP1-Win64-Shipping\.exe$)", {{
      { "dxvk.hideIntegratedGraphics",      "True" },
    }} },
    { R"(\\ACShadows\.exe$)", {{
      { "dxgi.enableDummyCompositionSwapchain", "True" }
    }} },
    { R"(\\Anno117\.exe$)", {{
      { "dxgi.enableDummyCompositionSwapchain", "True" }
    }} },


    { R"(\\BatmanAK\.exe$)", {{
      { "dxgi.hideIntelGpu",                "True" },
    }} },
    { R"(\\ACS\.exe$)", {{
      { "dxgi.customVendorId",              "10de" },
    }} },
    { R"(\\dffnt\.exe$)", {{
      { "dxgi.deferSurfaceCreation",        "True" },
    }} },
    { R"(\\EliteDangerous64\.exe$)", {{
      { "dxgi.customVendorId",              "10de" },
    }} },
    { R"(\\evelauncher\.exe$)", {{
      { "d3d11.maxFeatureLevel",            "12_1" },
    }} },
    { R"(\\EvilWithin(Demo)?\.exe$)", {{
      { "d3d11.cachedDynamicResources",       "vi" },
    }} },
    { R"(\\(farcry3|fc3_blooddragon)_d3d11\.exe$)", {{
      { "dxgi.hideNvidiaGpu",              "False" },
      { "dxgi.hideIntelGpu",                "True" },
    }} },
    { R"(\\(FarCry4|FCPrimal)\.exe$)", {{
      { "dxgi.hideNvidiaGpu",              "False" },
      { "dxgi.hideIntelGpu",                "True" },
    }} },
    { R"(\\FarCry(5|NewDawn)\.exe$)", {{
      { "d3d11.forceComputeLdsBarriers",    "True" },
    }} },
    { R"(\\Frostpunk\.exe$)", {{
      { "dxgi.deferSurfaceCreation",        "True" },
      { "d3d11.cachedDynamicResources",        "c" },
    }} },
    { R"(\\nioh\.exe$)", {{
      { "d3d9.deferSurfaceCreation",        "True" },
    }} },
    { R"(\\Anno1800\.exe$)", {{
      { "d3d11.cachedDynamicResources",        "c" },
    }} },
    { R"(\\FIFA(19|20|21|22)(_demo)?\.exe$)", {{
      { "dxvk.useRawSsbo",                  "True" },
    }} },
    { R"(\\re(2|3|3demo)\.exe$)", {{
      { "d3d11.relaxedBarriers",            "True" },
    }} },
    { R"(\\DevilMayCry5\.exe$)", {{
      { "d3d11.relaxedBarriers",            "True" },
    }} },
    { R"(\\s2_sp64_ship\.exe$)", {{
      { "dxgi.hideNvidiaGpu",              "False" },
    }} },
    { R"(\\NFS16\.exe$)", {{
      { "dxgi.hideNvidiaGpu",              "False" },
    }} },
    { R"(\\MassEffectAndromeda\.exe$)", {{
      { "dxgi.hideNvidiaGpu",              "False" },
    }} },
    { R"(\\MirrorsEdgeCatalyst(Trial)?\.exe$)", {{
      { "dxgi.customVendorId",              "10de" },
    }} },
    { R"(\\starwarsbattlefront(trial)?\.exe$)", {{
      { "dxgi.customVendorId",              "10de" },
    }} },
    { R"(\\NieR Replicant ver\.1\.22474487139\.exe)", {{
      { "d3d11.cachedDynamicResources",       "vi" },
    }} },
    { R"(\\HITMAN2\.exe$)", {{
      { "dxgi.customVendorId",              "10de" },
      { "d3d11.cachedDynamicResources",        "c" },
    }} },
    { R"(\\h1(_[ms]p64_ship|-mod)\.exe$)", {{
      { "dxgi.customVendorId",              "10de" },
    }} },
    { R"(\\h2m-mod\.exe$)", {{
      { "dxgi.customVendorId",              "10de" },
    }} },
    { R"(\\hmw-mod\.exe$)", {{
      { "dxgi.customVendorId",              "10de" },
    }} },
    { R"(\\iw7(_ship|-mod)\.exe$)", {{
      { "dxgi.customVendorId",              "10de" },
    }} },
    { R"(\\MW2CR\.exe$)", {{
      { "dxgi.customVendorId",              "10de" },
    }} },
    { R"(\\Crysis3\.exe$)", {{
      { "dxgi.hideNvidiaGpu",              "False" },
      { "d3d11.cachedDynamicResources",        "a" },
    }} },
    { R"(\\Crysis3Remastered\.exe$)", {{
      { "d3d11.cachedDynamicResources",        "a" },
    }} },
    { R"(\\Atelier_(Ayesha|Escha_and_Logy|Shallie)(_EN)?\.exe$)", {{
      { "d3d9.deferSurfaceCreation",        "True" },
    }} },
    { R"(\\A18\.exe$)", {{
      { "d3d9.deferSurfaceCreation",        "True" },
    }} },
    { R"(\\A(11R|12V|13V)_x64_Release(_en)?\.exe$)", {{
      { "d3d9.deferSurfaceCreation",        "True" },
    }} },
    { R"(\\Atelier_(Lulua|Lydie_and_Suelle|Ryza(_2|_3)?|Sophie_2)\.exe$)", {{
      { "d3d9.deferSurfaceCreation",        "True" },
    }} },
    { R"(\\Atelier_(Lydie_and_Suelle|Firis|Sophie)_DX\.exe$)", {{
      { "d3d9.deferSurfaceCreation",        "True" },
    }} },
    { R"(\\FAIRY_TAIL\.exe$)", {{
      { "d3d9.deferSurfaceCreation",        "True" },
    }} },
    { R"(\\CNN\.exe$)", {{
      { "d3d9.deferSurfaceCreation",        "True" },
    }} },
    { R"(\\starwarsbattlefrontii\.exe$)", {{
      { "dxgi.customVendorId",              "10de" },
    }} },
    { R"(\\F1_20(1[89]|[2-9][0-9])\.exe$)", {{
      { "d3d11.forceComputeLdsBarriers",    "True" },
    }} },
    { R"(\\darksiders1\.exe$)", {{
      { "d3d11.cachedDynamicResources",        "a" },
    }} },
    { R"(\\MonsterHunterWorld\.exe$)", {{
      { "d3d11.cachedDynamicResources",        "a" },
    }} },
    { R"(\\KingdomCome\.exe$)", {{
      { "d3d11.cachedDynamicResources",        "a" },
    }} },
    { R"(\\Homefront2_Release\.exe$)", {{
      { "d3d11.cachedDynamicResources",        "a" },
    }} },
    { R"(\\SGWContracts\.exe$)", {{
      { "d3d11.cachedDynamicResources",        "a" },
    }} },
    { R"(\\armoredwarfare\.exe$)", {{
      { "d3d11.cachedDynamicResources",        "c" },
    }} },
    { R"(\\nioh2\.exe$)", {{
      { "dxgi.deferSurfaceCreation",        "True" },
      { "dxvk.zeroMappedMemory",            "True" },
    }} },
    { R"(\\cm3\.exe$)", {{
      { "dxgi.customDeviceDesc",     "DXVK Device" },
    }} },
    { R"(\\WOFF\.exe$)", {{
      { "d3d11.disableMsaa",                "True" },
      { "dxgi.customDeviceDesc",     "DXVK Device" },
    }} },
    { R"(\\MarySkelter2\.exe$)", {{
      { "d3d11.disableMsaa",                "True" },
    }} },
    { R"(\\ffxiv_dx11\.exe$)", {{
      { "d3d11.cachedDynamicResources",       "vi" },
    }} },
    { R"(\\ffxv_s\.exe$)", {{
      { "d3d11.relaxedGraphicsBarriers",    "True" },
    }} },
    { R"(\\GoW\.exe$)", {{
      { "d3d11.relaxedBarriers",            "True" },
      { "dxgi.hideNvidiaGpu",              "False" },
      { "dxgi.maxFrameLatency",                "1" },
    }} },
    { R"(\\AoE2DE_s\.exe$)", {{
      { "d3d11.cachedDynamicResources",        "a" },
    }} },
    { R"(\\ac(3|4bf)[sm]p\.exe$)", {{
      { "d3d11.cachedDynamicResources",        "a" },
    }} },
    { R"(\\SOPFFO\.exe$)", {{
      { "d3d9.deferSurfaceCreation",        "True" },
    }} },
    { R"(\\SRBT\.exe$)", {{
      { "d3d9.deferSurfaceCreation",        "True" },
    }} },
    { R"(\\AWayOut(_friend)?\.exe$)", {{
      { "dxgi.maxFrameLatency",                "1" },
    }} },
    { R"(\\GW2\.Main_Win64_Retail\.exe$)", {{
      { "dxgi.customVendorId",              "10de" },
    }} },
    { R"(\\DayZ_x64\.exe$)", {{
      { "d3d11.cachedDynamicResources",       "cr" },
    }} },
    { R"(\\Stray-Win64-Shipping\.exe$)", {{
      { "d3d11.relaxedGraphicsBarriers",    "True" },
    }} },
    { R"(\\MgsGroundZeroes\.exe$)", {{
      { "dxgi.maxDeviceMemory",             "4095" },
    }} },
    { R"(\\ShantaeCurse\.exe$)", {{
      { "dxgi.maxFrameRate",                 "-60" },
    }} },
    { R"(\\MSFC\.exe$)", {{
      { "dxgi.maxFrameRate",                 "-60" },
    }} },
    { R"(\\TEW2\.exe$)", {{
      { "dxgi.maxFrameRate",                "-120" },
    }} },
    { R"(\\SonicFrontiers\.exe$)", {{
      { "dxgi.maxFrameLatency",                "1" },
    }} },
    { R"(\\SF3ClientFinal\.exe$)", {{
      { "d3d11.cachedDynamicResources",        "v" },
    }} },
    { R"(\\GRB\.exe$)", {{
      { "dxgi.hideNvidiaGpu",              "False" },
    }} },
    { R"(\\GTA5\.exe$)", {{
      { "d3d11.cachedDynamicResources",       "vi" },
    }} },
    { R"(\\CrashBandicootNSaneTrilogy\.exe$)", {{
      { "dxgi.syncInterval",                   "1" },
    }} },
    { R"(\\Fallout76\.exe$)", {{
      { "dxgi.syncInterval",                   "1" },
    }} },
    { R"(\\BLADESTORM Nightmare\\Launch_(EA|JP)\.exe$)", {{
      { "dxgi.maxFrameRate",                 "-60" },
    }} },
    { R"(\\Vindictus(_x64)?\.exe$)", {{
      { "d3d11.cachedDynamicResources",       "cr" },
      { "dxvk.zeroMappedMemory",            "True" },
    }} },
    { R"(\\RidersRepublic(_BE)?\.exe$)", {{
      { "dxgi.hideAmdGpu",                  "True" },
    }} },
    { R"(\\kenshi_x64\.exe$)", {{
      { "d3d11.cachedDynamicResources",        "v" },
    }} },
    { R"(\\granblue_fantasy_relink\.exe$)", {{
      { "d3d11.relaxedGraphicsBarriers",    "True" },
      { "d3d11.exposeDriverCommandLists",  "False" },
      { "dxgi.hideNvidiaGpu",              "False" },
    }} },
    { R"(\\Crysis(64)?\.exe$)", {{
      { "d3d9.maxFrameRate",                  "-1" },
      { "dxgi.maxFrameRate",                  "-1" },
    }} },
    { R"(\\EDF6\.exe$)", {{
      { "d3d11.enableContextLock",          "True" },
    }} },
    { R"(\\Kena-Win64-Shipping\.exe$)", {{
      { "dxgi.hideIntelGpu",                "True" },
    }} },
    { R"(\\(LibertyCity|ViceCity|SanAndreas)\.exe$)", {{
      { "dxgi.enableUe4Workarounds",        "True" },
    }} },
    { R"(\\x86_64\\Warcraft III\.exe$)", {{
      { "dxvk.hideIntegratedGraphics",      "True" },
    }} },
    { R"(\\EDF5\.exe$)", {{
      { "dxgi.tearFree",                   "False" },
      { "dxgi.syncInterval",                   "1" },
    }} },
    { R"(\\Varstray_steam(_demo)?\.exe$)", {{
      { "dxgi.maxFrameRate",                 "-60" },
    }} },
    { R"(\\WatchDogs2\.exe$)", {{
      { "d3d11.forceComputeUavBarriers",    "True" },
    }} },
    { R"(\\Rocketbirds 2\\Game\.exe$)", {{
      { "d3d11.disableDirectImageMapping",  "True" },
    }} },
    { R"(\\LEGOLCUR_DX11\.exe$)", {{
      { "d3d11.disableDirectImageMapping",  "True" },
    }} },
    { R"(\\Wargame European Escalation\\WarGame\.exe$)", {{
      { "dxgi.hideNvidiaGpu",               "True" },
    }} },
    { R"(\\GuiltyGear\.exe$)", {{
      { "dxgi.maxFrameRate",                 "-60" },
    }} },
    { R"(\\Rapture_Release\.exe$)", {{
      { "d3d11.cachedDynamicResources",        "a" },
    }} },
    { R"(\\Pharaoh\.exe$)", {{
      { "d3d11.disableDirectImageMapping",  "True" },
      { "dxvk.enableImplicitResolves",     "False" },
    }} },


    { R"(\\HatinTimeGame\.exe$)", {{
      { "d3d9.lenientClear",                "True" },
    }} },
    { R"(\\anarchyonline\.exe$)", {{
      { "d3d9.memoryTrackTest",             "True" },
    }} },
    { R"(\\Borderlands\.exe$)", {{
      { "d3d9.lenientClear",                "True" },
    }} },
    { R"(\\Borderlands2\.exe$)", {{
      { "d3d9.lenientClear",                "True" },
      { "d3d9.supportDFFormats",           "False" },
      { "d3d9.floatEmulation",            "Strict" },
    }} },
    { R"(\\BorderlandsPreSequel\.exe$)", {{
      { "d3d9.lenientClear",                "True" },
      { "d3d9.supportDFFormats",           "False" },
    }} },
    { R"(\\Gothic(3|3Final| III Forsaken Gods)\.exe$)", {{
      { "d3d9.supportDFFormats",           "False" },
    }} },
    { R"(\\Sonic Adventure 2\\(launcher|sonic2app)\.exe$)", {{
      { "d3d9.floatEmulation",            "Strict" },
      { "d3d9.maxFrameRate",                 "-60" },
    }} },
    { R"(\\(Sims2.*|TS2BodyShop|SimsLS|SimsPS|SimsCS)"
      R"(|The Sims 2 Content Manager|TS2HomeCrafterPlus)\.exe$)", {{
      { "d3d9.customVendorId",              "10de" },
      { "d3d9.customDeviceId",              "0091" },
      { "d3d9.customDeviceDesc", "GeForce 7800 GTX" },
      { "d3d9.disableA8RT",                 "True" },
      { "d3d9.supportX4R4G4B4",            "False" },
      { "d3d9.maxAvailableMemory",          "2048" },
      { "d3d9.memoryTrackTest",             "True" },
      { "d3d9.cachedWriteOnlyBuffers",      "True" },
    }} },
    { R"(\\Dead Space\.exe$)", {{
      { "d3d9.supportDFFormats",           "False" },
      { "d3d9.maxFrameRate",                 "-60" },
      { "d3d9.presentInterval",                "1" },
    }} },
    { R"(\\deadspace2\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
      { "d3d9.presentInterval",                "1" },
    }} },
    { R"(\\halo(ce)?\.exe$)", {{
      { "d3d9.customVendorId",              "1002" },
      { "d3d9.customDeviceId",              "4172" },
      { "d3d9.forceSamplerTypeSpecConstants", "True" },
    }} },
    { R"(\\csgo\.exe$)", {{
      { "d3d9.hideNvidiaGpu",               "True" },
      { "d3d9.forceDrawTimeBufferUpload",   "True" },
    }} },
    { R"(\\vampire\.exe$)", {{
      { "d3d9.deferSurfaceCreation",        "True" },
      { "d3d9.memoryTrackTest",             "True" },
      { "d3d9.maxAvailableMemory",          "1024" },
    }} },
    { R"(\\SKShinoviVersus\.exe$)", {{
      { "d3d9.forceAspectRatio",            "16:9" },
    }} },
    { R"(\\TESV\.exe$)", {{
      { "d3d9.hideNvidiaGpu",               "True" },
    }} },
    { R"(\\Neptunia\.exe$)", {{
      { "d3d9.forceAspectRatio",            "16:9" },
    }} },
    { R"(\\(GTAIV|EFLC)\.exe$)", {{
      { "d3d9.hideNvidiaGpu",               "True" },
      { "dxgi.maxDeviceMemory",              "128" },
      { "d3d9.supportDFFormats",           "False" },
      { "d3d9.deviceLossOnFocusLoss",       "True" },
    }} },
    { R"(\\(BF2|BF2142|PRBF2)\.exe$)", {{
      { "d3d9.deviceLossOnFocusLoss",       "True" },
      { "d3d9.countLosableResources",      "False" },
    }} },
    { R"(\\SpellForce2.*\.exe$)", {{
      { "d3d9.forceSamplerTypeSpecConstants", "True" },
    }} },
    { R"(\\(trl|tra|tru)\.exe$)", {{
      { "d3d9.cachedWriteOnlyBuffers",      "True" },
      { "d3d9.maxFrameRate",                 "-60" },
    }} },
    { R"(\\eqgame\.exe$)", {{
      { "d3d9.cachedWriteOnlyBuffers",      "True" },
    }} },
    { R"(\\mm\.exe$)", {{
      { "d3d9.deferSurfaceCreation",        "True" },
      { "d3d9.memoryTrackTest",             "True" },
    }} },
    { R"(\\mafia2\.exe$)", {{
      { "d3d9.customVendorId",              "10de" },
      { "d3d9.customDeviceId",              "0402" },
    }} },
    { R"(\\(WAR(-64)?|WARTEST(-64)?)\.exe$)", {{
      { "d3d9.hideNvidiaGpu",               "True" },
    }} },
    { R"(\\DragonNest_x64\.exe$)", {{
      { "d3d9.memoryTrackTest",             "True" },
    }} },
    { R"(\\DST\.exe$)", {{
      { "d3d9.deferSurfaceCreation",        "True" },
    }} },
    { R"(\\k2\.exe$)", {{
      { "d3d9.memoryTrackTest",             "True" },
    }} },
    { R"(\\tlpsl\.exe$)", {{
      { "d3d9.deferSurfaceCreation",        "True" },
    }} },
    { R"(\\NINJA GAIDEN SIGMA(2)?\.exe$)", {{
      { "d3d9.deferSurfaceCreation",        "True" },
    }} },
    { R"(\\Demonstone\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
    }} },
    { R"(\\FarCry\.exe$)", {{
      { "d3d9.hideAmdGpu",                  "True" },
    }} },
    { R"(\\SineMoraEX\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
    }} },
    { R"(\\ROGame\.exe$)", {{
      { "d3d9.floatEmulation",            "Strict" },
    }} },
    { R"(\\DarkSoulsII\.exe$)", {{
      { "d3d9.floatEmulation",            "Strict" },
    }} },
    { R"(\\Dogfight1942\.exe$)", {{
      { "d3d9.floatEmulation",            "Strict" },
    }} },
    { R"(\\Bayonetta\.exe$)", {{
      { "d3d9.floatEmulation",            "Strict" },
    }} },
    { R"(\\Rayman Origins\.exe$)", {{
      { "d3d9.floatEmulation",            "Strict" },
    }} },
    { R"(\\GuiltyGearXrd\.exe$)", {{
      { "d3d9.floatEmulation",            "Strict" },
    }} },
    { R"(\\RichardBurnsRally_SSE\.exe$)", {{
      { "d3d9.floatEmulation",            "Strict" },
    }} },
    { R"(\\BBCF\.exe$)", {{
      { "d3d9.floatEmulation",            "Strict" },
    }} },
    { R"(\\limbo\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
    }} },
    { R"(\\BsgLauncher\.exe$)", {{
      { "d3d9.shaderModel",                    "1" },
    }} },
    { R"(\\SWTFU2\.exe$)", {{
      { "d3d9.forceSamplerTypeSpecConstants", "True" },
    }} },
    { R"(\\eoa\.exe$)", {{
      { "d3d9.cachedWriteOnlyBuffers",      "True" },
      { "d3d9.countLosableResources",      "False" },
    }} },
    { R"(\\(SupremeCommander|ForgedAlliance)\.exe$)", {{
      { "d3d9.floatEmulation",            "Strict" },
    }} },
    { R"(\\bionic_commando\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
    }} },
    { R"(\\BGE\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
    }} },
    { R"(\\kof(xiii|13_win32_Release)\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
    }} },
    { R"(\\yso_win\.exe$)", {{
      { "d3d9.maxFrameLatency",                "1" },
    }} },
    { R"(\\SR2_pc\.exe$)", {{
      { "d3d9.textureMemory",                  "0" },
    }} },
    { R"(\\witcher\.exe$)", {{
      { "d3d9.maxFrameRate",                "-300" },
    }} },
    { R"(\\(GHWT|GHWT_Definitive)\.exe$)", {{
      { "d3d9.textureMemory",                 "16" },
    }} },
    { R"(\\ship\.exe$)", {{
      { "d3d9.memoryTrackTest",             "True" },
    }} },
    { R"(\\SinEpisodes\.exe$)", {{
      { "d3d9.memoryTrackTest",             "True" },
    }} },
    { R"(\\(hammer(plusplus)?|mallet|wc)\.exe$)", {{
      { "d3d9.cachedWriteOnlyBuffers",      "True" },
    }} },
    { R"(\\ASN_App_PcDx9_Final\.exe$)", {{
      { "d3d9.cachedWriteOnlyBuffers",      "True" },
    }} },
    { R"(\\ffxiv\.exe$)", {{
      { "d3d9.textureMemory",                  "0" },
    }} },
    { R"(\\(ShippingPC-AFEARGame|ARageMP)\.exe$)", {{
      { "d3d9.customVendorId",              "10de" },
      { "d3d9.customDeviceId",              "05E0" },
      { "dxgi.hideNvidiaGpu",              "False" },
    }} },
    { R"(\\bf10\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
    }} },
    { R"(\\(PANZERS|PANZERS_Phase_2)\.exe$)", {{
      { "d3d9.deferSurfaceCreation",        "True" },
      { "d3d9.cachedWriteOnlyBuffers",      "True" },
    }} },
    { R"(\\DCGAME\.EXE$)", {{
      { "d3d9.deviceLossOnFocusLoss",       "True" },
    }} },
    { R"(\\eldorado\.exe$)", {{
      { "d3d9.floatEmulation",            "Strict" },
      { "d3d9.allowDirectBufferMapping",   "False" },
    }} },
    { R"(\\injustice\.exe$)", {{
      { "d3d9.allowDirectBufferMapping",   "False" },
    }} },
    { R"(\\SG_ELITE\\Game\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
    }} },
    { R"(\\IncPC\.exe$)", {{
      { "d3d9.maxFrameRate",                  "59" },
    }} },
    { R"(\\Vietnam\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
    }} },
    { R"(\\Snowblind\.(SP|MP|exe)$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
    }} },
    { R"(\\Aviary Attorney\\nw\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
    }} },
    { R"(\\drakensang\.exe$)", {{
      { "d3d9.deferSurfaceCreation",        "True" },
    }} },
    { R"(\\AoK HD\.exe$)", {{
      { "d3d9.maxFrameLatency",                "1" },
    }} },
    { R"(\\Battlestationsmidway\.exe$)", {{
      { "d3d9.cachedWriteOnlyBuffers",      "True" },
    }} },
    { R"(\\AssassinsCreedIIGame\.exe$)" , {{
      { "d3d9.deviceLossOnFocusLoss",       "True" },
    }} },
    { R"(\\soniccd\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
    }} },
    { R"(\\UK Truck Simulator\\bin\\win_x86\\game\.exe$)", {{
      { "d3d9.floatEmulation",            "Strict" },
    }} },
    { R"(\\SupremeRuler(Ultimate|GreatWar|1936|CW)\.exe$)", {{
      { "d3d9.countLosableResources",      "False" },
    }} },
    { R"(\\RedRiver\.exe$)", {{
      { "d3d9.floatEmulation",            "Strict" },
    }} },
    { R"(\\ShippingPC-SkyGame\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
    }} },
    { R"(\\ninthdawnii\.exe$)", {{
      { "d3d9.deferSurfaceCreation",        "True" },
    }} },
    { R"(\\(DFX|dfx2)\.exe$)", {{
      { "d3d9.cachedWriteOnlyBuffers",      "True" },
    }} },
    { R"(\\prototypef\.exe$)", {{
      { "d3d9.hideAmdGpu",                  "True" },
      { "dxgi.maxDeviceMemory",             "2047" },
    }} },
    { R"(\\SWTFU\.exe$)", {{
      { "d3d9.deviceLossOnFocusLoss",       "True" },
    }} },
    { R"(\\Fallout( -)? New Vegas.*\\(FalloutNV)?(Launcher)?\.exe$)", {{
      { "d3d9.floatEmulation",            "Strict" },
      { "d3d9.hideNvidiaGpu",               "True" },
    }} },
    { R"(\\Dragonshard\.exe$)", {{
      { "d3d9.cachedWriteOnlyBuffers",      "True" },
    }} },
    { R"(\\(The Battle for Middle-earth( \(tm\))? II( Demo)?)"
      R"(|The Lord of the Rings, The Rise of the Witch-king)\\game\.dat$)", {{
      { "d3d9.cachedWriteOnlyBuffers",      "True" },
    }} },
    { R"(\\WRC4\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
    }} },
    { R"(\\conviction_game\.exe$)", {{
      { "dxgi.customVendorId",              "10de" },
      { "dxgi.customDeviceId",              "05e0" },
      { "dxgi.customDeviceDesc", "GeForce GTX 295" },
    }} },
    { R"(\\RaccoonCity\.exe$)", {{
      { "d3d9.textureMemory",                  "0" },
    }} },
    { R"(\\APB\.exe$)", {{
      { "d3d9.cachedWriteOnlyBuffers",      "True" },
    }} },
    { R"(\\Battle Mages\\mages\.exe$)", {{
      { "d3d9.cachedWriteOnlyBuffers",      "True" },
    }} },
    { R"(\\Prince( of Persia|OfPersia_Launcher)\.exe$)", {{
      { "d3d9.maxFrameRate",                 "240" },
    }} },
    { R"(\\FEAR(MP|XP|XP2)?\.exe$)", {{
      { "d3d9.maxFrameRate",                "-360" },
    }} },
    { R"(\\SecretWorldLegends\.exe$)", {{
      { "d3d9.memoryTrackTest",             "True" },
    }} },
    { R"(\\(FarCry2|farcry2game)\.exe$)", {{
      { "d3d9.cachedWriteOnlyBuffers",      "True" },
    }} },
    { R"(\\DS\.exe$)", {{
      { "d3d9.textureMemory",                  "0" },
    }} },
    { R"(\\(AH3LM|AALib)\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
    }} },
    { R"(\\MaxPayne3\.exe$)", {{
      { "d3d9.floatEmulation",            "Strict" },
    }} },
    { R"(\\(StarWarsG|sweaw|swfoc)\.exe$)", {{
      { "d3d9.maxAvailableMemory",          "2048" },
      { "d3d9.memoryTrackTest",             "True" },
    }} },
    { R"(\\CivCity Rome\.exe$)", {{
      { "d3d9.customVendorId",              "10de" },
    }} },
    { R"(\\sh2pc\.exe$)", {{
      { "d3d9.extraFrontbuffer",            "True" },
    }} },
    { R"(\\LEGOIndy\.exe$)", {{
      { "d3d9.cachedWriteOnlyBuffers",      "True" },
    }} },
    { R"((\\LEGOBatman|LegoBatman\\Game)\.exe$)", {{
      { "d3d9.cachedWriteOnlyBuffers",      "True" },
    }} },
    { R"(\\THUMPER_dx9\.exe$)", {{
      { "d3d9.floatEmulation",            "Strict" },
    }} },
    { R"(\\RedOrchestra\.exe$)", {{
      { "d3d9.deviceLossOnFocusLoss",       "True" },
      { "d3d9.countLosableResources",      "False" },
    }} },
    { R"(\\PH\.exe$)", {{
      { "d3d9.memoryTrackTest",             "True" },
      { "d3d9.maxAvailableMemory",          "2048" },
    }} },
    { R"(\\BEA\.exe$)", {{
      { "d3d9.customVendorId",              "10de" },
      { "d3d9.customDeviceId",              "0330" },
      { "d3d9.customDeviceDesc", "NVIDIA GeForce FX 5900 Ultra" },
    }} },
    { R"(\\Astebreed\.exe$)", {{
      { "d3d9.countLosableResources",      "False" },
      { "d3d9.maxFrameRate",                 "-60" },
    }} },
    { R"(\\(DLSteamEdition|dlords)\.exe$)", {{
      { "d3d9.textureMemory",                  "0" },
    }} },
    { R"(\\TestDrive2\.exe$)", {{
      { "d3d9.deviceLossOnFocusLoss",       "True" },
    }} },
    { R"(\\Perilous Warp\\system(64)?\\game\.exe$)", {{
      { "d3d9.hideNvidiaGpu",               "True" },
    }} },
    { R"(\\AN2\.dat$)", {{
      { "d3d9.modeCountCompatibility",      "True" },
    }} },
    { R"(\\PsiOps\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
    }} },
    { R"(\\Alone\.exe$)", {{
      { "d3d9.memoryTrackTest",             "True" },
    }} },
    { R"(\\Scarface\.exe$)", {{
      { "d3d9.allowDiscard",               "False" },
    }} },
    { R"(\\Heroes of Annihilated Empires.*\\engine\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
    }} },
    { R"(\\RRRE(64)?\.exe$)", {{
      { "d3d9.hideNvidiaGpu",               "True" },
    }} },
    { R"(\\Blitzkrieg 2( - Fall of the Reich| - Liberation)?\\bin\\game\.exe$)", {{
      { "d3d9.memoryTrackTest",             "True" },
    }} },
    { R"(\\(BioDungeon|BioLab2|MineDungeon2)\.exe$)", {{
      { "d3d9.countLosableResources",      "False" },
    }} },
    { R"(\\RRU(_demo)?\.exe$)", {{
      { "dxvk.zeroMappedMemory",            "True" },
    }} },
    { R"(\\TS3(W)?\.exe$)", {{
      { "d3d9.customVendorId",              "10de" },
      { "d3d9.customDeviceId",              "1080" },
      { "d3d9.customDeviceDesc", "Geforce GTX 580" },
    }} },
    { R"(\\Pirates!\.exe$)", {{
      { "d3d9.countLosableResources",      "False" },
    }} },
    { R"(\\W40k(_gog)?\.exe$)", {{
      { "dxvk.zeroMappedMemory",            "True" },
    }} },
    { R"(\\insurgency\.exe$)", {{
      { "d3d9.forceDrawTimeBufferUpload",   "True" },
    }} },
    { R"(\\SCSDestinations\.exe$)", {{
      { "d3d9.supportCubeDepthFormats",     "True" },
    }} },
    { R"(\\(SplinterCell4|SCDA_online)\.exe$)", {{
      { "d3d9.hideAmdGpu",                  "True" },
    }} },
    { R"(\\splintercell3\.exe$)", {{
      { "d3d9.ignoreDefaultBufferLockRange", "True" },
    }} },


    { R"(\\DukeForever\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
    }} },
    { R"(\\Anito\.exe$)", {{
      { "d3d9.memoryTrackTest",             "True" },
      { "d3d9.maxAvailableMemory",          "1024" },
    }} },
    { R"(\\MCity_d\.exe$)", {{
      { "d3d8.batching",                    "True" },
    }} },
    { R"(\\RT3\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
    }} },
    { R"(\\Pure Pinball 2\.0 REDUX\.exe$)", {{
      { "d3d8.forceVsDecl",  "0:2,4:2,7:4,9:1,8:1" },
    }} },
    { R"(\\nfs3\.exe$)", {{
      { "d3d8.batching",                    "True" },
    }} },
    { R"(\\nfs4\.exe$)", {{
      { "d3d9.memoryTrackTest",             "True" },
      { "d3d9.maxAvailableMemory",          "1024" },
      { "d3d8.batching",                    "True" },
    }} },
    { R"(\\NFSHP2\.exe$)", {{
      { "d3d9.cachedWriteOnlyBuffers",      "True" },
    }} },
    { R"(\\igi2\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
    }} },
    { R"(\\TP_Win32\.exe$)", {{
      { "d3d8.forceVsDecl",      "0:2,3:2,6:4,7:1" },
    }} },
    { R"(\\Scrap\.exe$)", {{
      { "d3d9.deferSurfaceCreation",        "True" },
    }} },
    { R"(\\VRally3(Demo)?\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
    }} },
    { R"(\\Soldiers\.exe$)", {{
      { "d3d9.memoryTrackTest",             "True" },
      { "d3d9.maxAvailableMemory",           "512" },
    }} },
    { R"(\\Cossacks II.*\\engine\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
    }} },
    { R"(\\Alexander\\Data\\engine\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
    }} },
    { R"(\\3DMark2001(SE)?\.exe$)", {{
      { "d3d9.allowDirectBufferMapping",   "False" },
    }} },
    { R"(\\dfbhd\.exe$)", {{
      { "d3d9.cachedWriteOnlyBuffers",      "True" },
    }} },
    { R"(\\X2\.exe$)", {{
      { "d3d9.cachedWriteOnlyBuffers",      "True" },
    }} },
    { R"(\\Fellowship\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
      { "d3d8.placeP8InScratch",            "True" },
    }} },
    { R"(\\Inquisitor\.exe$)", {{
      { "d3d9.countLosableResources",      "False" },
    }} },
    { R"(\\Art of Murder - FBI Confidential\\game\.exe$)", {{
      { "d3d9.cachedWriteOnlyBuffers",      "True" },
    }} },
    { R"(\\MaxPayne\.exe$)", {{
      { "d3d9.allowDirectBufferMapping",   "False" },
    }} },
    { R"(\\fifa2003(demo)?\.exe$)", {{
      { "d3d9.cachedWriteOnlyBuffers",      "True" },
    }} },
    { R"(\\offline\\system\\SplinterCell2\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
      { "d3d8.scaleDref",                     "24" },
    }} },
    { R"(\\Splinter Cell Pandora Tomorrow\\system\\SplinterCell2\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
    }} },
    { R"(\\Chrome(Single|Net)\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
    }} },
    { R"(\\Rayman3\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
      { "d3d8.forceLegacyDiscard",          "True" },
    }} },
    { R"(\\splintercell\.exe$)", {{
      { "d3d9.hideAmdGpu",                  "True" },
      { "d3d9.maxFrameRate",                 "-60" },
      { "d3d9.deviceLossOnFocusLoss",       "True" },
      { "d3d8.scaleDref",                     "24" },
      { "d3d8.shadowPerspectiveDivide",     "True" },
    }} },
    { R"(\\bin\\trainz\.exe$)", {{
      { "d3d9.deviceLossOnFocusLoss",       "True" },
    }} },
    { R"(\\AIM\.exe$)", {{
      { "d3d9.deviceLossOnFocusLoss",       "True" },
    }} },
    { R"(\\SFC3\.exe$)", {{
      { "d3d9.countLosableResources",      "False" },
    }} },
    { R"(\\GTR (- FIA GT Rac(e)?ing Game|Demo)\\(GTR(Demo)?|(3D)?Config)\.exe$)", {{
      { "d3d9.maxAvailableMemory",          "1024" },
      { "d3d9.memoryTrackTest",             "True" },
      { "d3d9.cachedWriteOnlyBuffers",      "True" },
    }} },
     { R"(\\c4(lan)?\.exe$)", {{
      { "d3d9.customVendorId",              "10de" },
      { "d3d9.customDeviceId",              "0250" },
      { "d3d9.customDeviceDesc", "NVIDIA GeForce4 Ti 4600" },
    }} },
    { R"(\\Aqua\.exe$)", {{
      { "d3d9.modeCountCompatibility",      "True" },
    }} },
    { R"(\\TopSpin\.exe$)", {{
      { "d3d8.forceLegacyDiscard",          "True" },
    }} },
    { R"(\\LEGO Racers 2\.exe$)", {{
      { "d3d9.allowDirectBufferMapping",   "False" },
    }} },
    { R"(\\Smash up Derby\\cars\.exe$)", {{
      { "d3d9.allowDirectBufferMapping",   "False" },
    }} },
    { R"(\\(Age of Pirates|Sea Dogs).*Caribbean Tales\\ENGINE\.exe$)", {{
      { "d3d8.textureUAFGuard",             "True" },
    }} },
    { R"(\\(Age of Pirates|Sea Dogs).*City of Abandoned Ships\\START\.exe$)", {{
      { "d3d8.textureUAFGuard",             "True" },
    }} },
    { R"(\\Mafia\\Game\.exe$)", {{
      { "d3d9.samplerAnisotropy",             "16" },
    }} },
    { R"(\\manhunt\.exe$)", {{
      { "d3d9.maxFrameRate",                 "-60" },
    }} },

  };


  const static ProfileList g_deckProfiles = {
    { R"(\\Fallout4\.exe$)", {{
      { "dxgi.syncInterval",                   "1" },
      { "dxgi.maxFrameRate",                 "-60" },
    }} },
  };


  const static ProfileList g_hashedProfiles = {
  };


  const Config* findProfile(const ProfileList& profiles, const std::string& appName) {
    auto appConfig = std::find_if(profiles.begin(), profiles.end(),
      [&appName] (const std::pair<const char*, Config>& pair) {
        try {
          std::regex expr(pair.first, std::regex::extended | std::regex::icase);
          return std::regex_search(appName, expr);
        } catch (const std::bad_alloc& e) {
          Logger::err(str::format("Failed to parse regular expression: ", pair.first));
          return false;
        }
      });

    return appConfig != profiles.end()
      ? &appConfig->second
      : nullptr;
  }


  const Config* findHashedProfile(const ProfileList& profiles, const std::string& appName) {
    if (profiles.empty())
      return nullptr;

    auto n = appName.find_last_of('\\') + 1u;

    if (n >= appName.size())
      return nullptr;

    auto hash = Sha1Hash::compute(&appName[n], appName.size() - n).toString();

    auto appConfig = std::find_if(profiles.begin(), profiles.end(),
      [&hash] (const std::pair<const char*, Config>& pair) {
        return hash == pair.first;
      });

    return appConfig != profiles.end()
      ? &appConfig->second
      : nullptr;
  }


  static bool isWhitespace(char ch) {
    return ch == ' ' || ch == '\x9' || ch == '\r';
  }


  static bool isValidKeyChar(char ch) {
    return (ch >= '0' && ch <= '9')
        || (ch >= 'A' && ch <= 'Z')
        || (ch >= 'a' && ch <= 'z')
        || (ch == '.' || ch == '_');
  }


  static size_t skipWhitespace(const std::string& line, size_t n) {
    while (n < line.size() && isWhitespace(line[n]))
      n += 1;
    return n;
  }


  struct ConfigContext {
    bool active;
  };


  static void parseUserConfigLine(Config& config, ConfigContext& ctx, const std::string& line) {
    std::stringstream key;
    std::stringstream value;

    size_t n = skipWhitespace(line, 0);

    if (n < line.size() && line[n] == '[') {
      n += 1;

      size_t e = line.size() - 1;
      while (e > n && line[e] != ']')
        e -= 1;

      while (n < e)
        key << line[n++];

      ctx.active = key.str() == env::getExeName();
    } else {
      while (n < line.size() && isValidKeyChar(line[n]))
        key << line[n++];

      n = skipWhitespace(line, n);
      if (n >= line.size() || line[n] != '=')
        return;

      bool insideString = false;
      n = skipWhitespace(line, n + 1);

      while (n < line.size()) {
        if (!insideString && isWhitespace(line[n]))
          break;

        if (line[n] == '"') {
          insideString = !insideString;
          n++;
        } else
          value << line[n++];
      }

      if (ctx.active)
        config.setOption(key.str(), value.str());
    }
  }


  Config::Config() { }
  Config::~Config() { }


  Config::Config(OptionMap&& options)
  : m_options(std::move(options)) { }


  void Config::merge(const Config& other) {
    for (auto& pair : other.m_options)
      m_options.insert(pair);
  }


  void Config::setOption(const std::string& key, const std::string& value) {
    m_options.insert_or_assign(key, value);
  }


  std::string Config::getOptionValue(const char* option) const {
    auto iter = m_options.find(option);

    return iter != m_options.end()
      ? iter->second : std::string();
  }


  bool Config::parseOptionValue(
    const std::string&  value,
          std::string&  result) {
    result = value;
    return true;
  }


  bool Config::parseOptionValue(
    const std::string&  value,
          bool&         result) {
    static const std::array<std::pair<const char*, bool>, 2> s_lookup = {{
      { "true",  true  },
      { "false", false },
    }};

    return parseStringOption(value,
      s_lookup.begin(), s_lookup.end(), result);
  }


  bool Config::parseOptionValue(
    const std::string&  value,
          int32_t&      result) {
    if (value.size() == 0)
      return false;

    int32_t sign = 1;
    size_t start = 0;

    if (value[0] == '-') {
      sign = -1;
      start = 1;
    }

    int32_t intval = 0;

    for (size_t i = start; i < value.size(); i++) {
      if (value[i] < '0' || value[i] > '9')
        return false;

      intval *= 10;
      intval += value[i] - '0';
    }

    result = sign * intval;
    return true;
  }


  bool Config::parseOptionValue(
    const std::string&  value,
          float&        result) {
    if (value.size() == 0)
      return false;

    size_t pos = 0;
    bool negate = false;

    if (value[0] == '-') {
      negate = true;

      if (++pos == value.size())
        return false;
    }

    uint64_t intPart = 0;

    if (value[pos] == '.')
      return false;

    while (pos < value.size()) {
      if (value[pos] == '.') {
        if (++pos == value.size())
          return false;
        break;
      }

      if (value[pos] < '0' || value[pos] > '9')
        return false;

      intPart *= 10;
      intPart += value[pos] - '0';
      pos += 1;
    }

    uint64_t fractPart = 0;
    uint64_t fractDivisor = 1;

    while (pos < value.size()) {
      if (value[pos] < '0' || value[pos] > '9')
        return false;

      fractDivisor *= 10;
      fractPart *= 10;
      fractPart += value[pos] - '0';
      pos += 1;
    }

    result = float((double(fractPart) / double(fractDivisor)) + double(intPart));

    if (negate)
      result = -result;

    return std::isfinite(result);
  }


  bool Config::parseOptionValue(
    const std::string&  value,
          Tristate&     result) {
    static const std::array<std::pair<const char*, Tristate>, 3> s_lookup = {{
      { "true",  Tristate::True  },
      { "false", Tristate::False },
      { "auto",  Tristate::Auto  },
    }};

    return parseStringOption(value,
      s_lookup.begin(), s_lookup.end(), result);
  }


  template<typename I, typename V>
  bool Config::parseStringOption(
          std::string   str,
          I             begin,
          I             end,
          V&            value) {
    str = Config::toLower(str);

    for (auto i = begin; i != end; i++) {
      if (str == i->first) {
        value = i->second;
        return true;
      }
    }

    return false;
  }


  Config Config::getAppConfig(const std::string& appName) {
    const Config* config = nullptr;

    if (env::getEnvVar("SteamDeck") == "1")
      config = findProfile(g_deckProfiles, appName);

    if (!config)
      config = findProfile(g_profiles, appName);

    if (!config)
      config = findHashedProfile(g_hashedProfiles, appName);

    if (config) {
      Logger::info(str::format("Found built-in config:"));

      for (auto& pair : config->m_options)
        Logger::info(str::format("  ", pair.first, " = ", pair.second));

      return *config;
    }

    return Config();
  }


  Config Config::getUserConfig() {
    Config config;

    std::string filePath = env::getEnvVar("DXVK_CONFIG_FILE");
    std::string confLine = env::getEnvVar("DXVK_CONFIG");

    if (filePath == "")
      filePath = "dxvk.conf";

    std::ifstream stream(str::topath(filePath.c_str()).c_str());

    if (!stream && confLine.empty())
      return config;

    ConfigContext ctx;
    ctx.active = true;

    if (stream) {
      Logger::info(str::format("Found config file: ", filePath));

      std::string line;

      while (std::getline(stream, line))
        parseUserConfigLine(config, ctx, line);
    }

    if (!confLine.empty()) {
      ctx.active = true;

      Logger::info(str::format("Found config env: ", confLine));

      for(auto l : str::split(confLine, ";"))
        parseUserConfigLine(config, ctx, std::string(l.data(), l.size()));
    }

    return config;
  }


  void Config::logOptions() const {
    if (!m_options.empty()) {
      Logger::info("Effective configuration:");

      for (auto& pair : m_options)
        Logger::info(str::format("  ", pair.first, " = ", pair.second));
    }
  }

  std::string Config::toLower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(),
      [] (unsigned char c) { return (c >= 'A' && c <= 'Z') ? (c + 'a' - 'A') : c; });
    return str;
  }

}
