// main.cpp
// EKey-Jerboa V2.0.0
// Author: egorrushka
// Based on VanitySearch by Jean Luc PONS (GPLv3)
// License: GPLv3 - https://www.gnu.org/licenses/
//
// Usage:
//   EKey-Jerboa.exe -a <address> -s 0xSTART -e 0xEND -T <seconds> [-W <0-10>] [-G <gpuId>] [-b]

#include <sstream>
#include "Timer.h"
#include "Vanity.h"
#include "Jerboa.h"
#include "SECP256k1.h"
#include <string>
#include <string.h>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <vector>
#include <csignal>
#include <cuda_runtime.h>

#if defined(_WIN32)||defined(_WIN64)
#include <io.h>
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#endif

std::atomic<bool> Pause(false);
std::atomic<bool> Paused(false);
std::atomic<bool> stopMonitorKey(false);
int    idxcount  = 0;
double t_Paused  = 0.0;
bool   backupMode= false;
using namespace std;

VanitySearch* g_vs = nullptr;
std::atomic<bool> g_shutdown(false);

void signalHandler(int sig) {
    if (!backupMode) { printf("\n"); fflush(stdout); exit(sig); }
    if (g_shutdown.exchange(true)) exit(sig);
    printf("\n[!] Ctrl+C - shutting down..."); fflush(stdout);
    if (g_vs) g_vs->endOfSearch = true;
}

#if defined(_WIN32)||defined(_WIN64)
void monitorKeypress() {
    while (!stopMonitorKey) {
        Timer::SleepMillis(1);
        if (_kbhit()) {
            char ch = _getch();
            if (ch=='p'||ch=='P') { Pause = !Pause.load(); }
        }
    }
}
#else
// Linux keyboard monitor (simplified)
void monitorKeypress() {
    while (!stopMonitorKey) { Timer::SleepMillis(50); }
}
#endif

// ?????????????????????????????????????????????????????????????????????????????
// EKey-Jerboa -- FAQ / Manual
// ?????????????????????????????????????????????????????????????????????????????
static void printFaq() {
    printf("\n");
    printf("=======================================================================\n");
    printf("  EKey-Jerboa V2.0.0 -- FULL FAQ & MANUAL\n");
    printf("=======================================================================\n\n");

    /* 1. WHAT IS IT */
    printf("1. WHAT IS IT?\n");
    printf("   GPU-accelerated Bitcoin private key search (puzzle hunting).\n");
    printf("   Searches a hex range for a target P2PKH address or compressed\n");
    printf("   public key. Uses NVIDIA CUDA. Optimised for Turing/Ampere/Ada.\n\n");

    /* 2. BASIC USAGE */
    printf("2. BASIC USAGE\n");
    printf("   EKey-Jerboa.exe -a <addr> -s 0xSTART -e 0xEND [options]\n\n");
    printf("   Required:\n");
    printf("     -a <addr>    Target P2PKH Bitcoin address\n");
    printf("     -p <pubkey>  Target compressed public key (alt. to -a)\n");
    printf("     -s 0x<hex>   Chunk start  e.g. 0x40000000000000000\n");
    printf("     -e 0x<hex>   Chunk end    e.g. 0x41FFFFFFFFFFFFFF\n");
    printf("     -r <bits>    Bit-range shorthand: -r 71 (puzzle 71)\n\n");
    printf("   Options:\n");
    printf("     -T <sec>     Jump interval (default 30). 999999999=linear.\n");
    printf("     -G <id>      GPU device ID (default 0)\n");
    printf("     -W <0-7>     Grid: 0=auto 5=6144x256 7=12288x256\n");
    printf("     -R           Random slot order (LCG permutation)\n");
    printf("     -b           Resume from saved progress\n");
    printf("     -S<N>        Depth sections: 100/500/1000/5000/10000\n");
    printf("     -faq / -inf  This manual / credits\n\n");

    /* 3. FUNCTIONS & ALGORITHMS IMPLEMENTED */
    printf("3. FUNCTIONS AND ALGORITHMS IMPLEMENTED\n");
    printf("   a) secp256k1 elliptic-curve engine (Int/IntMod/Point/SECP256K1)\n");
    printf("      256-bit modular arithmetic, point add/double, batch modular\n");
    printf("      inversion (Montgomery trick) to amortise the costly inverse.\n");
    printf("   b) Hash160 pipeline: SHA-256 then RIPEMD-160 over the compressed\n");
    printf("      public key, on both CPU (verify) and GPU (search).\n");
    printf("   c) Address codecs: Base58Check (P2PKH) and Bech32 (segwit) for\n");
    printf("      parsing the target and printing results.\n");
    printf("   d) Jerboa interleave engine (Jerboa.cpp): splits the chunk into\n");
    printf("      slots and visits them in sequential or random (LCG) order.\n");
    printf("   e) Two-level golden-ratio scatter: chooses WHERE inside a slot to\n");
    printf("      search so passes spread evenly with no early repetition.\n");
    printf("   f) Progress persistence: per-puzzle/chunk/mode/section JSON files\n");
    printf("      plus a .launcher satellite holding the GUI state.\n");
    printf("   g) Search match path: SINGLE_TARGET_MODE (one address, constant\n");
    printf("      memory) or multi-target table with warp voting.\n\n");

    /* 4. HOW EACH PART WORKS */
    printf("4. HOW EACH PART WORKS\n");
    printf("   CHUNK -> SLOTS: the hex range is split by the hex-digit structure\n");
    printf("   of its bounds (e.g. 0x40000000-0x41FFFFFF -> 32 slots). Each slot\n");
    printf("   is a contiguous sub-range; the engine stays -T seconds per slot\n");
    printf("   then jumps to the next.\n\n");
    printf("   GPU KERNEL: each thread holds a starting point; the kernel walks a\n");
    printf("   group of consecutive keys using point addition with one shared\n");
    printf("   batch inverse, computes Hash160 for each, and compares.\n\n");
    printf("   SCATTER (two-level golden ratio), per slot of S sections:\n");
    printf("     visitCount = times this slot was visited\n");
    printf("     cycle      = visitCount / S      pos = visitCount %% S\n");
    printf("     slot_phase = ((slotIdx+1) * 6181) %% S      [per-slot offset]\n");
    printf("     section    = (pos * 6181 + slot_phase) %% S     [MACRO: where]\n");
    printf("     micro      = (cycle * 6181) %% S                [MICRO: depth]\n");
    printf("   The absolute batch is scaled to the real slot size (V2.0.0):\n");
    printf("     sectionStart = section * totalBatches / S\n");
    printf("     sectionSpan  = totalBatches / S\n");
    printf("     batchCount   = sectionStart + micro * sectionSpan / S\n");
    printf("   gcd(6181,S)=1 so each level is a bijection: S*S unique positions\n");
    printf("   per slot before any repeat (e.g. 100,000,000 at -S10000).\n");
    printf("   On small ranges (totalBatches < S) it degrades gracefully instead\n");
    printf("   of collapsing to one point (fixed in V2.0.0).\n\n");
    printf("   STATUS LINE:\n");
    printf("     [Depth:NN%%]    batchCount / totalBatches * 100\n");
    printf("     [Scatter P:0x] absolute key address of the scatter start\n");
    printf("     [Sec X/S Cyc N +B] macro section, cycle number, batches in visit\n\n");
    printf("   PROGRESS: saved every ~30 s and on Ctrl+C into\n");
    printf("     p{puz}_0x{lo8}-0x{hi8}_{mode}_S{N}/jerboa_..._gpu{G}_{mode}.json\n");
    printf("   Each puzzle+chunk+mode+section combo gets its own folder; resume\n");
    printf("   with -b restores visitCount and continues deterministically.\n\n");

    /* 5. OPTIMIZATIONS FOR MAX SPEED */
    printf("5. OPTIMIZATIONS USED FOR MAXIMUM PERFORMANCE\n");
    printf("   A) Batch modular inversion: one inverse shared across a whole key\n");
    printf("      group instead of one per key (Montgomery trick).\n");
    printf("   B) SHA256Transform / RIPEMD160Transform are __forceinline__ to\n");
    printf("      remove call overhead on every one of ~1.5M keys/batch.\n");
    printf("   C) SHA256Transform_inplace writes the digest back into w[0..7],\n");
    printf("      dropping a separate buffer (~96 MB less local memory pressure).\n");
    printf("   D) _GetHash160Comp uses a single reused w[16] array for both hash\n");
    printf("      passes -> lower register pressure, higher occupancy.\n");
    printf("   E) SINGLE_TARGET_MODE: the target hash160 sits in GPU constant\n");
    printf("      memory; CheckPoint_Opt does a 5-word cascade compare entirely\n");
    printf("      in registers -> ZERO global reads for ~99.9999999%% of keys.\n");
    printf("   F) Warp voting (__any_sync) for the multi-target path: a whole\n");
    printf("      warp skips the lookup unless at least one lane has a candidate.\n");
    printf("   G) Multi-arch fat binary: sm_75 + sm_86 + sm_89 cubins plus PTX,\n");
    printf("      so the driver JITs for newer GPUs automatically.\n");
    printf("   H) V2.0.0: thread-safe per-thread RNG (no shared rand() race) and\n");
    printf("      full-range scatter scaling that never degenerates.\n");
    printf("   Result: 1.44 Gk/s -> 1.47 Gk/s on RTX A4000 (sm_86).\n\n");

    /* 6. BUILDING */
    printf("6. HOW TO BUILD\n\n");
    printf("   WINDOWS (ready-made script in the repo):\n");
    printf("     1. Install CUDA Toolkit + Visual Studio Build Tools (MSVC).\n");
    printf("     2. Open the \"x64 Native Tools Command Prompt for VS\".\n");
    printf("     3. Run:  compile_modern.bat\n");
    printf("        It calls nvcc with -gencode for sm_75/sm_86/sm_89 + PTX and\n");
    printf("        produces EKey-Jerboa.exe. Edit the 'cd /d ...' path and the\n");
    printf("        -arch list at the top if your setup differs.\n\n");
    printf("   LINUX (Makefile provided in the repo):\n");
    printf("     1. Install CUDA Toolkit (nvcc) and a C++17 compiler (g++).\n");
    printf("     2. Build:        make            (auto multi-arch)\n");
    printf("        Single arch:   make CUDA_ARCH=sm_86\n");
    printf("        Clean:         make clean\n");
    printf("     3. Run:          ./EKey-Jerboa -a <addr> -s 0x.. -e 0x..\n");
    printf("     The Makefile compiles the same sources (hash/ and GPU/ subdirs)\n");
    printf("     with -O3 -std=c++17 -Xcompiler -fopenmp and links the GPU code.\n\n");

    /* 7. GPU ARCHITECTURE SUPPORT */
    printf("7. GPU ARCHITECTURE SUPPORT\n");
    printf("   The sources are adapted to current NVIDIA architectures:\n");
    printf("     sm_75  -- Turing  (RTX 2060-2080 Ti)\n");
    printf("     sm_86  -- Ampere  (RTX 3070-3090 / A4000)\n");
    printf("     sm_89  -- Ada     (RTX 4070-4090)\n");
    printf("     PTX    -- forward compat (RTX 50xx+ via driver JIT)\n");
    printf("   A legacy build (CUDA 12.3) can target sm_61 (GTX 1060+).\n");
    printf("   One fat binary carries all listed cubins; the right one is chosen\n");
    printf("   at runtime, and mixed-GPU systems are supported.\n\n");

    /* 8. LINEAR SCAN */
    printf("8. LINEAR SCAN (no scatter, no jumps)\n");
    printf("   Set -T 999999999. The jump timer never fires; the program scans\n");
    printf("   from chunk start to end sequentially. -b saves/restores as usual.\n\n");

    printf("=======================================================================\n");
    printf("  Contact / feedback: egor.gr1@gmail.com\n");
    printf("=======================================================================\n\n");
    exit(0);
}

static void printInfo() {
    printf("\n");
    printf("=======================================================================\n");
    printf("  EKey-Jerboa V2.0.0 -- Version Info & Credits\n");
    printf("=======================================================================\n\n");

    printf("  WHAT IS THIS PROGRAM FOR\n");
    printf("  ------------------------\n");
    printf("  EKey-Jerboa is a GPU-accelerated search tool for Bitcoin private\n");
    printf("  keys inside a chosen key range (\"puzzle hunting\"). Given a target\n");
    printf("  P2PKH address or a compressed public key, it sweeps a hex range on\n");
    printf("  the secp256k1 curve, deriving each candidate public key and its\n");
    printf("  Hash160, and reports the private key when a match is found.\n");
    printf("  Its distinctive feature is the Jerboa interleave engine: instead of\n");
    printf("  a plain linear scan it \"hops\" across the range using a two-level\n");
    printf("  golden-ratio scatter, spreading coverage evenly with no repeats.\n\n");

    printf("  AUTHORSHIP\n");
    printf("  ----------\n");
    printf("  This program was written by Claude (Anthropic) under the meticulous\n");
    printf("  guidance and direction of egorrushka. Every algorithm, fix and\n");
    printf("  optimization here was implemented by the AI and reviewed, tested\n");
    printf("  and steered step by step by the project author.\n\n");

    printf("  BUILT UPON\n");
    printf("  ----------\n");
    printf("  Based on the respected VanitySearch by Jean Luc PONS\n");
    printf("             https://github.com/JeanLucPons/VanitySearch  (GPLv3)\n");
    printf("  Author fork: egorrushka\n\n");

    printf("  WRITTEN IN UKRAINE\n");
    printf("  ------------------\n");
    printf("  Created in Ukraine under the extreme conditions of war, in the\n");
    printf("  glorious city of Chernihiv. Built with resolve, despite everything.\n\n");

    printf("  CONTACT / FEEDBACK\n");
    printf("  ------------------\n");
    printf("  Questions and feedback: egor.gr1@gmail.com\n\n");

    printf("  -------------------------------------------------------------------\n");
    printf("  Project  : EKey-Jerboa V2.0.0\n");
    printf("  Purpose  : GPU Bitcoin private key range search (puzzle hunting)\n");
    printf("  Author   : egorrushka  (fork)\n");
    printf("  Code by  : Claude (Anthropic)\n");
    printf("  License  : GPLv3 -- https://www.gnu.org/licenses/\n");
    printf("  Based on : VanitySearch V1.19 by Jean Luc PONS (GPLv3)\n");
    printf("  Crypto   : secp256k1 ECDSA, SHA-256, RIPEMD-160\n");
    printf("  GPU      : CUDA C++ (NVIDIA)\n");
    printf("             sm_75 (RTX 2060-2080), sm_86 (RTX 3070-A4000),\n");
    printf("             sm_89 (RTX 4070-4090), PTX for future GPUs.\n");
    printf("             Legacy build (CUDA 12.3): sm_61 (GTX 1060+)\n\n");
    printf("  Added by the fork on top of VanitySearch:\n");
    printf("    + Jerboa multi-slot interleave engine\n");
    printf("    + Two-level golden-ratio depth scatter (Variant A)\n");
    printf("    + Per-puzzle/chunk/mode/sections progress folders\n");
    printf("    + .launcher satellite file for full UI state restore\n");
    printf("    + Configurable scatter depth (-S100 to -S10000)\n");
    printf("    + SINGLE_TARGET_MODE constant-memory fast path\n");
    printf("    + SHA256Transform_inplace (saves ~96 MB register memory)\n");
    printf("    + RIPEMD160 + SHA256 __forceinline__ optimizations\n");
    printf("    + Multi-arch CUDA binary (one .exe for all GPU families)\n");
    printf("    + Thread-safe RNG, robust scatter on small ranges (V2.0.0)\n");
    printf("    + Python launcher with scatter/progress UI\n\n");
    printf("  Tested on : RTX A4000 (sm_86), CUDA 13.1, Windows 11\n");
    printf("              1.47 Gk/s sustained\n\n");
    printf("  GitHub    : https://github.com/egorrushka/EKey-Jerboa\n\n");
    printf("=======================================================================\n\n");
    exit(0);
}

static void printHelp() {
    printf("\nEKey-Jerboa V2.0.0  by egorrushka\n");
    printf("Based on VanitySearch by Jean Luc PONS\n\n");
    printf("Usage: EKey-Jerboa.exe -a <address> -s 0xSTART -e 0xEND [options]\n\n");
    printf("Required:\n");
    printf("  -a <addr>   Target Bitcoin P2PKH address\n");
    printf("  -p <pubkey> Target public key (compressed hex, alternative to -a)\n");
    printf("  -s <hex>    Chunk start (hex, e.g. 0x400000000000000000)\n");
    printf("  -e <hex>    Chunk end   (hex, e.g. 0x7fffffffffffffffff)\n");
    printf("  -r <bits>   Bit range (alternative to -s/-e)\n\n");
    printf("Options:\n");
    printf("  -T <sec>    Jump interval in seconds (default 30)\n");
    printf("              Use 999999999 for linear non-stop scan\n");
    printf("  -G <id>     GPU device ID (default 0)\n");
    printf("  -W <0-7>    Grid profile  0=auto  5=6144x256  7=12288x256\n");
    printf("  -R          Random slot order (LCG permutation)\n");
    printf("  -b          Resume from progress file\n");
    printf("  -S<N>       Depth sections: -S100 -S500 -S1000 -S5000 -S10000 (default)\n");
    printf("  -faq        Full manual (scatter, progress, examples)\n");
    printf("  -inf        Version info and credits\n");
    printf("  -h          This help\n\n");
    printf("Grid profiles (-W):\n");
    printf("  0:auto  1:512  2:1024  3:2048  4:4096  5:6144  6:8192  7:12288\n\n");
    exit(0);
}

static const int GRIDS[] = {-1,512,1024,2048,4096,6144,8192,12288};
static const int NGRID   = 8;

static bool parseHex(const string& raw, Int& out) {
    string s = raw;
    if (s.size()>=2&&s[0]=='0'&&(s[1]=='x'||s[1]=='X')) s=s.substr(2);
    if (s.empty()||s.size()>64) return false;
    for (char c:s) if (!isxdigit((unsigned char)c)) return false;
    while (s.size()<64) s.insert(s.begin(),'0');
    vector<char> b(s.begin(),s.end()); b.push_back('\0');
    out.SetBase16(b.data()); return true;
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signalHandler);
    thread inputThread(monitorKeypress);
    Timer::Init();
    setvbuf(stdout, NULL, _IONBF, 0); // disable CRT buffering: Win32 console API must stay in sync with printf
    Secp256K1* secp = new Secp256K1(); secp->Init();

    if (argc < 2) printHelp();

    string taddr, tpubkey, hexStart, hexEnd;
    int gpuId=0, bits=0, gridProfile=0;
    double jumpSec = 30.0;
    int depthSections = 10000;  // -S100/-S500/-S1000/-S5000/-S10000
    string launcherJson;  // ?????????? launcher JSON (???????? ?? ?????)

    for (int i=1; i<argc; i++) {
        string arg = argv[i];
        if      (arg=="-h"||arg=="--help") printHelp();
        else if (arg=="-faq") printFaq();
        else if (arg=="-inf") printInfo();
        else if (arg=="-b") backupMode=true;
        else if (arg=="-a"&&i+1<argc) taddr    = argv[++i];
        else if (arg=="-p"&&i+1<argc) tpubkey  = argv[++i];
        else if (arg=="-s"&&i+1<argc) hexStart  = argv[++i];
        else if (arg=="-e"&&i+1<argc) hexEnd    = argv[++i];
        else if (arg=="-r"&&i+1<argc) bits = atoi(argv[++i]);
        else if (arg=="-T"&&i+1<argc) jumpSec = atof(argv[++i]);
        else if (arg=="-R") { /* randSlotMode set below */ }
        else if (arg.size()>=3 && arg.substr(0,2)=="-S" && isdigit((unsigned char)arg[2])) {
            int v=atoi(arg.substr(2).c_str());
            if(v==100||v==500||v==1000||v==5000||v==10000) depthSections=v;
            else{fprintf(stderr,"[ERROR] -S: must be 100/500/1000/5000/10000\n");exit(-1);}
        }
        else if (arg=="-J"&&i+1<argc) jumpSec = atof(argv[++i])*60.0; // minutes compat
        else if (arg=="-G"&&i+1<argc) gpuId = atoi(argv[++i]);
        else if (arg=="-W"&&i+1<argc) {
            gridProfile = atoi(argv[++i]);
            if (gridProfile<0||gridProfile>=NGRID){fprintf(stderr,"[ERROR] -W: 0-%d\n",NGRID-1);exit(-1);}
        }
        else if (arg=="--launcher-file"&&i+1<argc) {
            // ?????? JSON ?? ?????????? ????? (??????? ???????? ????????????? ? cmd.exe)
            std::string lfpath = argv[++i];
            FILE* lf = fopen(lfpath.c_str(), "r");
            if (lf) {
                fseek(lf, 0, SEEK_END); long sz = ftell(lf); rewind(lf);
                if (sz > 0 && sz < 4096) {
                    launcherJson.resize(sz);
                    fread(&launcherJson[0], 1, sz, lf);
                    // trim trailing whitespace/newlines
                    while (!launcherJson.empty() &&
                           (launcherJson.back()=='\n'||launcherJson.back()=='\r'||launcherJson.back()==' '))
                        launcherJson.pop_back();
                }
                fclose(lf);
            }
        }
        else { fprintf(stderr,"[ERROR] Unknown: %s\n",arg.c_str()); printHelp(); }
    }

    if (taddr.empty()&&tpubkey.empty()){fprintf(stderr,"[ERROR] Need -a or -p\n");printHelp();}
    bool useChunk = (!hexStart.empty()||!hexEnd.empty());
    if (useChunk&&(hexStart.empty()||hexEnd.empty())){fprintf(stderr,"[ERROR] Need both -s and -e\n");exit(-1);}
    if (!useChunk&&bits==0){fprintf(stderr,"[ERROR] Need -s/-e or -r\n");printHelp();}
    if (jumpSec<1.0) jumpSec=1.0;

    // Enable ANSI + set fixed console window size (Windows)
#if defined(_WIN32)||defined(_WIN64)
    {
        HANDLE h=GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD m=0;
        if(GetConsoleMode(h,&m))
            SetConsoleMode(h,m|ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        // Fixed window: 190 cols x 40 rows  (wide enough for 8-sector table)
        COORD buf={190,2000};
        SetConsoleScreenBufferSize(h,buf);
        SMALL_RECT win={0,0,189,39};
        SetConsoleWindowInfo(h,TRUE,&win);
        // Set window title
        SetConsoleTitleA("EKey-Jerboa V2.0.0  by egorrushka");
    }
#endif

    // CUDA check
    int devCount=0;
    cudaGetDeviceCount(&devCount);
    if (devCount==0){fprintf(stderr,"[ERROR] No CUDA GPU found\n");exit(-1);}
    if (gpuId>=devCount||gpuId<0){fprintf(stderr,"[ERROR] Invalid GPU id %d\n",gpuId);exit(-1);}

    // Build params
    BITCRACK_PARAM bc={};
    if (useChunk) {
        if (!parseHex(hexStart,bc.ksStart)||!parseHex(hexEnd,bc.ksFinish)){fprintf(stderr,"[ERROR] Bad hex\n");exit(-1);}
        if (bc.ksFinish.IsLower(&bc.ksStart)){fprintf(stderr,"[ERROR] end < start\n");exit(-1);}
    } else {
        bc.ksStart.SetInt32(1); if(bits>1) bc.ksStart.ShiftL(bits-1);
        bc.ksFinish.SetInt32(1); bc.ksFinish.ShiftL(bits); bc.ksFinish.SubOne();
    }
    bc.ksNext.Set(&bc.ksStart);
    bc.jerboaMode    = true;
    // -R flag: random within slot (LCG full-period permutation)
    bc.randSlotMode  = false;
    for (int _i=1;_i<argc;_i++) if (std::string(argv[_i])=="-R") bc.randSlotMode=true;
    bc.jerboaJumpSec = jumpSec;
    bc.depthSections = depthSections;
    // Store launcher JSON string (from --launcher "...") for satellite file
    memset(bc.launcherJson, 0, sizeof(bc.launcherJson));
    if (!launcherJson.empty())
        strncpy(bc.launcherJson, launcherJson.c_str(), sizeof(bc.launcherJson)-1);

    // Print header
    printf("\n[+] EKey-Jerboa V2.0.0  by egorrushka\n");
    if (!tpubkey.empty())
        printf("[+] Search : %s [Public Key]\n", tpubkey.c_str());
    else
        printf("[+] Search : %s [P2PKH/Compressed]\n", taddr.c_str());
    time_t now=time(NULL);
    char tbuf[64];
#if defined(_WIN32)||defined(_WIN64)
    ctime_s(tbuf,sizeof(tbuf),&now);
#else
    ctime_r(&now,tbuf);
#endif
    printf("[+] Start  : %s", tbuf);
    {
        string cs = bc.ksStart.GetBase16();
        string ce = bc.ksFinish.GetBase16();
        // trim leading zeros
        size_t n=cs.find_first_not_of('0'); if(n!=string::npos)cs=cs.substr(n);else cs="0";
        n=ce.find_first_not_of('0'); if(n!=string::npos)ce=ce.substr(n);else ce="0";
        printf("[+] Chunk  : 0x%s -> 0x%s\n", cs.c_str(), ce.c_str());
    }
    // Jump interval display: show "No Jump" when T=999999999
    if (jumpSec >= 999999998.0)
        printf("[+] Jump   : No Jump\n");
    else {
        printf("[+] Jump   : %.1f sec/position\n", jumpSec);
        // Slot mode only relevant when jumping
        printf("[+] Slot   : %s\n", bc.randSlotMode ? "RANDOM (LCG)" : "Sequential");
    }
    fflush(stdout);

    vector<string> targets;
    if (!tpubkey.empty()) targets.push_back(tpubkey);
    else targets.push_back(taddr);

    uint32_t maxFound = 65536*4;
    VanitySearch* v = new VanitySearch(secp, targets, SEARCH_COMPRESSED, true, "", maxFound, &bc);
    g_vs = v;

    int gx = (gridProfile>0&&gridProfile<NGRID) ? GRIDS[gridProfile] : 6144;
    vector<int> gpuIds={gpuId};
    vector<int> gridSizes={gx,256};
    if (gx>0)
        printf("[+] Grid   : %d x 256 = %d threads\n", gx, gx*256);
    fflush(stdout);

    v->Search(gpuIds, gridSizes);

    stopMonitorKey=true;
    if (inputThread.joinable()) inputThread.join();
    printf("\n");
    delete v; delete secp;
    return 0;
}
