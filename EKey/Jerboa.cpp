// Jerboa.cpp
// EKey-Jerboa V2.0.0  -  Multi-pass slot interleave
// Author fork: egorrushka
// Based on VanitySearch by Jean Luc PONS (GPLv3)

#include "Vanity.h"
#include "Jerboa.h"
#include "Timer.h"
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>     // O_WRONLY, open
#include <unistd.h>    // dup, dup2, close, STDOUT_FILENO
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <climits>
#include <string>
#include <vector>
#include <algorithm>
#include <random>
#include <chrono>
#include <thread>
#include <functional>
using namespace std;

// ── Thread-safe RNG ─────────────────────────────────────────────────────────
// FIX (5.1/5.2): replaces rand()/srand(time()).
//  * rand()/srand() are not thread-safe — with multiple GPU worker threads the
//    shared libc state is a data race.
//  * Repeated srand(time(NULL)) within the same second produced identical
//    shuffles across GPUs (lost decorrelation).
// A per-thread mt19937_64, seeded once from random_device + clock + thread id,
// fixes both. (Seed must NOT reference the object being constructed — MSVC
// rejects &gen inside gen's own initializer, hence the thread-id hash.)
static std::mt19937_64& jerboa_rng() {
    static thread_local std::mt19937_64 gen(
        (((uint64_t)std::random_device{}()) << 32) ^ (uint64_t)std::random_device{}() ^
        (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count() ^
        (uint64_t)std::hash<std::thread::id>{}(std::this_thread::get_id()));
    return gen;
}
// uniform integer in [0, n)  (n must be > 0)
static uint64_t jerboa_rand_below(uint64_t n) {
    if (n <= 1) return 0;
    return std::uniform_int_distribution<uint64_t>(0, n - 1)(jerboa_rng());
}
// full 64-bit random
static uint64_t jerboa_rand64() { return jerboa_rng()(); }

// ── helpers ───────────────────────────────────────────────────────────────────

static string i2h(const Int& v){ return const_cast<Int&>(v).GetBase16(); }
static string trim0(const string& h){
    size_t n=h.find_first_not_of('0');
    return (n!=string::npos)?h.substr(n):"0";
}
static int  hv(char c){
    if(c>='0'&&c<='9')return c-'0';
    if(c>='a'&&c<='f')return c-'a'+10;
    if(c>='A'&&c<='F')return c-'A'+10;
    return 0;
}
static char vc(int v){ return v<10?'0'+v:'A'+v-10; }

static bool h2i(const string& s, Int& out){
    string h=s;
    if(h.size()>=2&&h[0]=='0'&&(h[1]=='x'||h[1]=='X')) h=h.substr(2);
    if(h.empty()) return false;
    while(h.size()<64) h.insert(h.begin(),'0');
    vector<char> b(h.begin(),h.end()); b.push_back('\0');
    out.SetBase16(b.data()); return true;
}

// ── Progress file naming ─────────────────────────────────────────────────────
// Формат: jerboa_{puzzle}_{lo8}_{hi8}_gpu{N}_{mode}.json
// Пример: jerboa_71_40000000_41FFFFFF_gpu0_yesjump.json
//
// Номер пазла вычисляется из hex_lo:
//   "400000000000000000" → 71, "800..." → 72, "8000000000" → 40

static int puzzleFromHex(const string& hex_lo) {
    if(hex_lo.empty()) return 0;
    int bits = (int)(hex_lo.size() - 1) * 4;
    char c = (char)tolower((unsigned char)hex_lo[0]);
    if     (c>='8') bits += 4;
    else if(c>='4') bits += 3;
    else if(c>='2') bits += 2;
    else            bits += 1;
    return bits;
}

// ── mkdir helper (cross-platform) ────────────────────────────────────────────
#ifdef _WIN32
#include <direct.h>
static void mkdirIfNeeded(const char* path){ _mkdir(path); }
#else
#include <sys/stat.h>
static void mkdirIfNeeded(const char* path){ mkdir(path, 0755); }
#endif

// PATCH 2 (extended): Computes saveFileBase AND progressDir from chunk bounds + mode.
// progressDir  = "p{puz}_0x{lo8}-0x{hi8}_{mode}"
// saveFileBase = "{progressDir}/jerboa_{puz}_{lo8}_{hi8}"
// Used by BOTH jerboa_load and jerboa_setup.
static void computeSaveFileBase(JerboaEngine& e, BITCRACK_PARAM* bc) {
    string lo_full = trim0(i2h(bc->ksStart));
    string hi_full = trim0(i2h(bc->ksFinish));
    int puz = puzzleFromHex(lo_full);
    string lo8 = lo_full.size()>8 ? lo_full.substr(0,8) : lo_full;
    string hi8 = hi_full.size()>8 ? hi_full.substr(0,8) : hi_full;

    // Mode suffix
    const char* modeSuffix = bc->randSlotMode ? "random" : "sequential";

    // Folder: p71_0x40000000-0x41FFFFFF_random_S10000
    // _SXXXXX suffix makes each section-count create its own progress folder.
    int ds = (bc->depthSections > 0) ? bc->depthSections : 10000;
    snprintf(e.progressDir, sizeof(e.progressDir),
             "p%d_0x%s-0x%s_%s_S%d",
             puz, lo8.c_str(), hi8.c_str(), modeSuffix, ds);

    // Full file base inside folder
    snprintf(e.saveFileBase, sizeof(e.saveFileBase),
             "%s/jerboa_%d_%s_%s", e.progressDir, puz, lo8.c_str(), hi8.c_str());
}

static void ensureProgressDir(const JerboaEngine& e) {
    mkdirIfNeeded(e.progressDir);
}

static string jsonFile(const JerboaEngine& e, int gpuId) {
    return string(e.saveFileBase) + "_gpu" + to_string(gpuId) + "_yesjump.json";
}
static string jsonFileSingle(const JerboaEngine& e, int gpuId) {
    return string(e.saveFileBase) + "_gpu" + to_string(gpuId) + "_nojump.json";
}
static string launcherFile(const JerboaEngine& e, int gpuId, bool isSingle) {
    string base = string(e.saveFileBase) + "_gpu" + to_string(gpuId);
    return base + (isSingle ? "_nojump.launcher" : "_yesjump.launcher");
}

// ── sector analysis ───────────────────────────────────────────────────────────

static void analyzeSectors(JerboaEngine& e){
    string sH=trim0(i2h(e.chunkStart));
    string eH=trim0(i2h(e.chunkEnd));
    while(sH.size()<eH.size()) sH.insert(sH.begin(),'0');
    while(eH.size()<sH.size()) eH.insert(eH.begin(),'0');
    const int sl=0;
    e.sectorLevel=sl;
    int firstSec=hv(sH[0]), lastSec=hv(eH[0]);
    e.numSectors=lastSec-firstSec+1;
    e.hexFirstSector=firstSec;
    bool trimmedBoundary=false;
    if(e.numSectors>1){
        int lss=(1<(int)eH.size())?hv(eH[1]):0;
        if(lss==0){
            bool exact=true;
            for(int _d=2;_d<(int)eH.size();_d++)
                if(hv(eH[_d])!=0){exact=false;break;}
            if(exact){e.numSectors--;trimmedBoundary=true;}
        }
    }
    for(int z=0;z<e.numSectors;z++){
        int fSlot=0,lSlot=15;
        if(z==0) fSlot=(1<(int)sH.size())?hv(sH[1]):0;
        if(z==e.numSectors-1)
            lSlot=trimmedBoundary?15:((1<(int)eH.size())?hv(eH[1]):15);
        e.sectorFirstSlot[z]=fSlot;
        e.sectorLastSlot[z]=lSlot;
        e.sectorSlotCount[z]=lSlot-fSlot+1;
    }
}

static void computeWindowForPos(JerboaEngine& e, int posIdx, Int& ws, Int& we){
    ws.Set(const_cast<Int*>(&e.posStart[posIdx]));
    we.Set(const_cast<Int*>(&e.posStart[posIdx]));
    we.Add(const_cast<Int*>(&e.posSize[posIdx]));
    we.SubOne();
}

static void _updateWindow(JerboaEngine& e, BITCRACK_PARAM* bc){
    int pos=e.jumpOrder[e.jumpCursor];
    computeWindowForPos(e,pos,e.windowStart,e.windowEnd);
    bc->ksStart.Set(&e.windowStart);
    bc->ksFinish.Set(&e.windowEnd);
}

// ── LCG (slot order only) ─────────────────────────────────────────────────────


// ============================================================================
// Вычисляет hex-адрес ключа scatter позиции для текущего визита к слоту
//
// Правильная формула:
//   scatter_key = posStart[slot] + batchCount × posSize[slot] / totalBatches
//
// Это реальная позиция ключа в слоте (старшие биты меняются!):
//   batchCount=0         → posStart            (0% слота)
//   batchCount=0.467×tb  → posStart + 0.467×slotSize  (46.7% слота)
//   batchCount=0.933×tb  → posStart + 0.933×slotSize  (93.3% слота)
//
// НЕ требует bc или stepSize — всё берём из JerboaEngine
static void updateScatterPosHex(JerboaEngine& e)
{
    if(e.J_sec >= 999999998.0) return;  // NO-JUMP: не нужно
    if(e.totalPositions <= 0) return;

    int idx = e.jumpCursor % e.totalPositions;
    int pos = e.jumpOrder[idx];

    // VARIANT-A: scatter_key = posStart + section × posSize / e.depthSections
    // Это реальная позиция ключа, меняются старшие биты адреса!
    Int offset;
    offset.Set(&e.posSize[pos]);                          // размер слота
    offset.Mult((uint64_t)(uint32_t)e.currentSection);    // × номер секции
    Int secInt;
    secInt.SetInt32(e.depthSections);                  // / 10000
    offset.Div(&secInt);
    offset.Add(&e.posStart[pos]);                         // + начало слота

    string hexStr = offset.GetBase16();
    snprintf(e.scatterPosHex, sizeof(e.scatterPosHex)-1,
             "0x%s", hexStr.c_str());
}

// ============================================================================
// SCATTER — Метод "мельтешения" (YES-JUMP режим)
//
// Распределяет 1.5M потоков по слоту по принципу золотого сечения φ=0.618.
// Вместо последовательного порядка (поток i → позиция i*step),
// каждый поток получает позицию по формуле scatter(i) = (i * GOLDEN64) % N.
//
// Свойства:
//   ✅ Биекция (gcd(GOLDEN64, N) = 1 для стандартных грид-размеров)
//   ✅ Квази-равномерное покрытие с первой секунды на каждом слоте
//   ✅ Resume-совместимо: scatter детерминирован по thread_id
//   ✅ NO-JUMP режим НЕ затрагивается
//
// Применяется ТОЛЬКО в YES-JUMP режиме (J_sec < 999999998.0)
// Вызывается после getGPUStartingKeys, до SetKeys.
// ============================================================================
static const uint64_t SCATTER_GOLDEN64 = 0x9E3779B97F4A7C15ULL;

static void applyScatter(Point* pk, int numThreads) {
    // Выделяем временный буфер для перестановки
    Point* tmp = new Point[numThreads];
    uint64_t N = (uint64_t)numThreads;

    // tmp[j] = pk[scatter(j)]
    // scatter(j) = (j * GOLDEN64) % N
    // Поток 0: позиция 0 (0% слота)
    // Поток 1: позиция ~77% слота
    // Поток 2: позиция ~54% слота
    // Поток 3: позиция ~31% слота ...и т.д. по φ
    for (int j = 0; j < numThreads; j++) {
        int src = (int)(((uint64_t)j * SCATTER_GOLDEN64) % N);
        tmp[j] = pk[src];
    }

    memcpy(pk, tmp, (size_t)numThreads * sizeof(Point));
    delete[] tmp;
}

static void jerboa_init_slot_lcg(JerboaEngine& e){
    if(e.totalBatches<=1){e.slotLCGSeed=0;e.slotLCGStep=1;return;}
    int tb=e.totalBatches;
    e.slotLCGSeed=(int)(jerboa_rand64()%(unsigned)tb);
    int step=(tb/4)+(int)jerboa_rand_below((uint64_t)tb/2+1);step|=1;
    auto my_gcd=[](int a,int b)->int{while(b){int t=b;b=a%b;a=t;}return a;};
    while(my_gcd(step,tb)!=1) step=(step+2)|1;
    e.slotLCGStep=step;
}

static int jerboa_batch_index(const JerboaEngine&, int n){ return n; }

// ── jerboa_setup ──────────────────────────────────────────────────────────────

void jerboa_setup(JerboaEngine& e, int numThreads, int STEP_SIZE, BITCRACK_PARAM* bc, int gpuId)
{
    e.numThreads=numThreads;
    e.STEP_SIZE_val=STEP_SIZE;
    e.J_sec=bc->jerboaJumpSec;
    if(e.J_sec<1.0) e.J_sec=1.0;

    e.chunkStart.Set(&bc->ksStart);
    e.chunkEnd.Set(&bc->ksFinish);

    analyzeSectors(e);

    int pos=0;
    for(int z=0;z<e.numSectors;z++){
        for(int s=e.sectorFirstSlot[z];s<=e.sectorLastSlot[z];s++){
            e.posMap[pos].sectorIdx=z;
            e.posMap[pos].digit2=s;
            e.posFor[z][s]=pos;
            pos++;
        }
    }
    e.totalPositions=pos;

    // Natural hex-aligned stripes
    {
        string sH_=trim0(i2h(e.chunkStart));
        string eH_=trim0(i2h(e.chunkEnd));
        while(sH_.size()<eH_.size()) sH_.insert(sH_.begin(),'0');
        while(eH_.size()<sH_.size()) eH_.insert(eH_.begin(),'0');
        int hexW=(int)eH_.size();
        Int unitWidth; unitWidth.SetInt32(1);
        for(int _k=0;_k<hexW-2;_k++) unitWidth.ShiftL(4);
        for(int _i=0;_i<e.totalPositions;_i++){
            int _z=e.posMap[_i].sectorIdx, _s=e.posMap[_i].digit2;
            int secD=e.hexFirstSector+_z;
            Int natS; natS.Set(&unitWidth);
            Int fac; fac.SetInt32(secD*16+_s); natS.Mult(&fac);
            Int natE; natE.Set(&natS); natE.Add(&unitWidth); natE.SubOne();
            if(natS.IsLower(const_cast<Int*>(&e.chunkStart)))
                e.posStart[_i].Set(const_cast<Int*>(&e.chunkStart));
            else
                e.posStart[_i].Set(&natS);
            Int clampE;
            if(natE.IsGreater(const_cast<Int*>(&e.chunkEnd)))
                clampE.Set(const_cast<Int*>(&e.chunkEnd));
            else
                clampE.Set(&natE);
            e.posSize[_i].Set(&clampE);
            e.posSize[_i].Sub(&e.posStart[_i]);
            e.posSize[_i].AddOne();
        }
        Int chunkSize;
        chunkSize.Set(const_cast<Int*>(&e.chunkEnd));
        chunkSize.Sub(const_cast<Int*>(&e.chunkStart));
        chunkSize.AddOne();
        e.stripeWidth.Set(&chunkSize);
        Int tpInt; tpInt.SetInt32(e.totalPositions);
        e.stripeWidth.Div(&tpInt);
    }

    // totalBatches for first position
    {
        int firstPos=e.jumpOrder[0];
        Int ntInt; ntInt.SetInt32(numThreads);
        Int stride; stride.Set(const_cast<Int*>(&e.posSize[firstPos])); stride.Div(&ntInt);
        Int step((uint64_t)(unsigned)STEP_SIZE);
        Int m1; m1.Set(&step); m1.SubOne();
        Int num; num.Set(&stride); num.Add(&m1); num.Div(&step);
        uint64_t tb=num.bits64[0];
        if(num.bits64[1]!=0||tb>(uint64_t)INT_MAX) tb=INT_MAX;
        e.totalBatches=(int)tb;
        if(e.totalBatches<1) e.totalBatches=1;
    }

    // Set randSlotMode FIRST — before Fisher-Yates shuffle!
    e.randSlotMode=bc->randSlotMode;

    for(int i=0;i<e.totalPositions;i++) e.jumpOrder[i]=i;
    if(e.randSlotMode){
        for(int i=e.totalPositions-1;i>0;i--){
            int j=(int)jerboa_rand_below((uint64_t)i+1);
            swap(e.jumpOrder[i],e.jumpOrder[j]);
        }
    }

    e.jumpCursor=0;
    e.jumpsDone=0;
    e.batchCount=0;
    e.passCount=0;
    // Depth sections from flag (-S100/-S500/-S1000/-S5000/-S10000)
    e.depthSections = (bc->depthSections > 0) ? bc->depthSections : 10000;
    memset(e.visitedPos,0,sizeof(e.visitedPos));
    memset(e.slotBatchPos,0,sizeof(e.slotBatchPos));
    memset(e.scatterPosHex,0,sizeof(e.scatterPosHex));
    strcpy(e.scatterPosHex,"0x0");
    e.currentSection=0;
    e.currentCycle  =0;
    e.currentMicro  =0;

    // ── Формируем имя файла прогресса (PATCH 2) ──────────────────────────
    // Папка: p{puz}_0x{lo8}-0x{hi8}_{mode}
    // Файл:  {папка}/jerboa_{puz}_{lo8}_{hi8}_gpu{N}_{mode}.json
    computeSaveFileBase(e, bc);
    ensureProgressDir(e);
    printf("[Jerboa] ProgDir   : %s\n", e.progressDir);
    printf("[Jerboa] Progress  : %s_gpu{N}_{mode}.json\n", e.saveFileBase);
    printf("[Jerboa] Sections  : %d (flag -S%d)\n", e.depthSections, e.depthSections);
    // FIX (3.3): inform the user when the slot is smaller than the requested
    // section count. Scatter still works (graceful full-range scaling) but the
    // effective granularity is capped at the number of batches in the slot.
    if (e.totalBatches < e.depthSections)
        printf("[Jerboa] Note      : slot has %d batches < %d sections; "
               "scatter granularity capped at slot size.\n",
               e.totalBatches, e.depthSections);
    // Сохраняем сателлит лаунчера при старте (если передан --launcher "...")
    if (bc->launcherJson[0] != '\0')
        jerboa_save_launcher(e, gpuId, bc->launcherJson);

    _updateWindow(e,bc);

    if(e.randSlotMode) jerboa_init_slot_lcg(e);
    else {e.slotLCGSeed=0;e.slotLCGStep=1;}

    e.jumpStartTime=Timer::get_tick();
    e.speedMkey=1000.0;
    e.lastCounterSnap=0;
    e.lastSpeedTime=0.0;
    e.displayShown=false;
    e.inited=true;
}

// ── jerboa_do_jump ────────────────────────────────────────────────────────────

bool jerboa_do_jump(JerboaEngine& e, BITCRACK_PARAM* bc)
{
    // Save current slot info BEFORE moving away
    int curPos=e.jumpOrder[e.jumpCursor];
    if(e.J_sec >= 999999998.0)
        e.slotBatchPos[curPos]=e.batchCount;    // NO-JUMP: сохраняем позицию батча
    else
        e.slotBatchPos[curPos]++;               // YES-JUMP: инкрементируем счётчик визитов

    // Mark visited and advance
    e.visitedPos[curPos]=true;
    e.jumpsDone++;
    e.jumpCursor++;

    if(e.jumpCursor>=e.totalPositions){
        // ── All slots visited this pass → start new pass ──────────────────
        e.passCount++;
        e.jumpCursor=0;
        e.jumpsDone=0;
        memset(e.visitedPos,0,sizeof(e.visitedPos));

        // Re-shuffle slot order for next pass if random mode
        if(e.randSlotMode){
            for(int i=e.totalPositions-1;i>0;i--){
                int j=(int)jerboa_rand_below((uint64_t)i+1);
                swap(e.jumpOrder[i],e.jumpOrder[j]);
            }
        }
        // Sequential mode keeps same order — naturally "deepening" row by row

        // OPT: Остановка после 10000 проходов в YES-JUMP режиме
        if(e.J_sec < 999999998.0 && e.passCount >= 10000) {
            printf("\n[Jerboa] 10000 passes completed. Search finished.\n");
            fflush(stdout);
            return false;  // сигнал остановки
        }
    }

    // Set window for new slot
    _updateWindow(e,bc);

    int newPos=e.jumpOrder[e.jumpCursor];

    // ── PATCH 4: Recompute totalBatches for NEW slot FIRST
    // Must happen BEFORE scatter formula so it uses correct slot size
    {
        Int nt2; nt2.SetInt32(e.numThreads);
        Int stride; stride.Set(const_cast<Int*>(&e.posSize[newPos])); stride.Div(&nt2);
        Int step2((uint64_t)(unsigned)e.STEP_SIZE_val);
        Int m1; m1.Set(&step2); m1.SubOne();
        Int num; num.Set(&stride); num.Add(&m1); num.Div(&step2);
        uint64_t tb_new=num.bits64[0];
        if(num.bits64[1]!=0||tb_new>(uint64_t)INT_MAX) tb_new=INT_MAX;
        e.totalBatches=(int)tb_new;
        if(e.totalBatches<1) e.totalBatches=1;
    }

    // ── Restore batch / compute VARIANT-A scatter depth for new slot
    if(e.J_sec >= 999999998.0){
        // NO-JUMP: продолжаем с линейной позиции (без scatter)
        e.batchCount=e.slotBatchPos[newPos];
        e.currentSection=0;
        e.currentCycle  =0;
        e.currentMicro  =0;
    } else {
        // YES-JUMP ДВУХУРОВНЕВЫЙ SCATTER (Вариант А)
        // ─────────────────────────────────────────────────────────────
        // Уровень 1 (макро): секция [0..9999] — где в слоте искать
        //   macro_sec = (pos_in_cycle * 6181) % 10000
        //   Первые прыжки: 0%, 61.8%, 23.6%, 85.4%, 47.2%...
        //
        // Уровень 2 (микро): глубина внутри секции — цикл за циклом
        //   micro_off = (cycle * 6181) % 10000
        //   Цикл 1: начало каждой секции (depth 0%)
        //   Цикл 2: 61.8% внутри секции (depth 61.8%)
        //   Цикл 3: 23.6% внутри секции (depth 23.6%)
        //   → реальное углубление без повторов!
        //
        // Уникальность: 10000 × 10000 = 100M уникальных точек в слоте
        // gcd(6181, 10000) = 1 → биекция на каждом уровне
        int vc        = e.slotBatchPos[newPos];
        int cycle     = vc / e.depthSections;   // номер полного цикла
        int pos       = vc % e.depthSections;   // позиция внутри цикла
        // ── PATCH 7: per-slot PHASE SHIFT
        // Без этого на первом проходе ВСЕ слоты стартуют с section 0,
        // и Scatter P == Current (visually ничего не происходит).
        // С phase shift каждый слот получает уникальную стартовую секцию
        // даже на первом визите, сохраняя биекцию внутри слота.
        // (newPos + 1) - inache slot 0 daet slot_phase=0
        int slot_phase = (int)(((uint64_t)(newPos + 1) * DEPTH_GOLDEN) % e.depthSections);
        int macro_sec = (int)((((uint64_t)pos   * DEPTH_GOLDEN) + slot_phase) % e.depthSections);
        int micro_off = (int)(((uint64_t)cycle * DEPTH_GOLDEN) % e.depthSections);
        int tb        = e.totalBatches > 1 ? e.totalBatches : 1;
        int DS        = e.depthSections;
        e.currentSection = macro_sec;
        e.currentCycle   = cycle;
        e.currentMicro   = micro_off;
        // FIX (3.3): full-range scaling so the scatter never collapses when the
        // slot has fewer batches than sections (tb < DS). The previous code
        // computed sec_size = tb/DS and clamped it to 1, which made every
        // section map onto the same tiny [0,tb) range with massive collisions
        // (the "100M unique positions" property silently broke).
        //   sectionStart = macro_sec * tb / DS   — section start across whole slot
        //   sectionSpan  = tb / DS                — width of one section (0 if tb<DS)
        //   microDepth   = micro_off * sectionSpan / DS — depth inside the section
        // When tb >= DS this is equivalent to the old per-section walk; when
        // tb < DS it degrades gracefully (golden-ratio spread over tb batches).
        int64_t sectionStart = ((int64_t)macro_sec * (int64_t)tb) / (int64_t)DS;
        int64_t sectionSpan  = (int64_t)tb / (int64_t)DS;
        int64_t microDepth   = (sectionSpan > 0)
                             ? ((int64_t)micro_off * sectionSpan) / (int64_t)DS
                             : 0;
        int64_t bc64 = sectionStart + microDepth;
        if(bc64 >= tb) bc64 = tb - 1;   // Не выходим за пределы слота
        if(bc64 < 0)   bc64 = 0;
        e.batchCount = (int)bc64;
    }
    e.slotKeyCount=0;

    if(e.randSlotMode) jerboa_init_slot_lcg(e);
    else {e.slotLCGSeed=0;e.slotLCGStep=1;}
    e.jumpStartTime=Timer::get_tick();
    return true;  // Always continue — multi-pass never stops by itself
}

// ── save / load ───────────────────────────────────────────────────────────────

void jerboa_save_single(const JerboaEngine& e, int gpuId)
{
    ensureProgressDir(e);
    string path=jsonFileSingle(e, gpuId);
    FILE* f=fopen(path.c_str(),"w");
    if(!f){ printf("[Jerboa] WARNING: cannot open progress file %s\n", path.c_str()); return; }
    fprintf(f,"{\n");
    fprintf(f,"  \"mode\": \"single_slot\",\n");
    fprintf(f,"  \"batch_count\": %d,\n",   e.batchCount);
    fprintf(f,"  \"total_batches\": %d,\n", e.totalBatches);
    fprintf(f,"  \"j_sec\": %.2f,\n",       e.J_sec);
    fprintf(f,"  \"num_threads\": %d,\n",   e.numThreads);
    fprintf(f,"  \"chunk_start\": \"%s\",\n",i2h(e.chunkStart).c_str());
    fprintf(f,"  \"chunk_end\": \"%s\"\n",   i2h(e.chunkEnd).c_str());
    fprintf(f,"}\n");
    if(ferror(f)) printf("[Jerboa] WARNING: write error on %s — progress may be incomplete\n", path.c_str());
    if(fclose(f)!=0) printf("[Jerboa] WARNING: failed to flush/close %s\n", path.c_str());
}

void jerboa_save(const JerboaEngine& e, int gpuId)
{
    ensureProgressDir(e);
    string path=jsonFile(e, gpuId);
    FILE* f=fopen(path.c_str(),"w");
    if(!f){ printf("[Jerboa] WARNING: cannot open progress file %s\n", path.c_str()); return; }
    fprintf(f,"{\n");
    fprintf(f,"  \"pass_count\": %d,\n",      e.passCount);
    fprintf(f,"  \"batch_count\": %d,\n",     e.batchCount);
    fprintf(f,"  \"total_batches\": %d,\n",   e.totalBatches);
    fprintf(f,"  \"jump_cursor\": %d,\n",     e.jumpCursor);
    fprintf(f,"  \"jumps_done\": %d,\n",      e.jumpsDone);
    fprintf(f,"  \"total_positions\": %d,\n", e.totalPositions);
    fprintf(f,"  \"num_threads\": %d,\n",     e.numThreads);
    fprintf(f,"  \"j_sec\": %.2f,\n",         e.J_sec);
    fprintf(f,"  \"rand_slot\": %d,\n",       e.randSlotMode?1:0);
    fprintf(f,"  \"lcg_seed\": %d,\n",        e.slotLCGSeed);
    fprintf(f,"  \"lcg_step\": %d,\n",        e.slotLCGStep);
    fprintf(f,"  \"chunk_start\": \"%s\",\n", i2h(e.chunkStart).c_str());
    fprintf(f,"  \"chunk_end\": \"%s\",\n",   i2h(e.chunkEnd).c_str());
    // slot_batch_positions
    fprintf(f,"  \"slot_batch_positions\": [");
    for(int i=0;i<e.totalPositions;i++)
        fprintf(f,"%d%s",e.slotBatchPos[i],i<e.totalPositions-1?",":"");
    fprintf(f,"],\n");
    // jump_order
    fprintf(f,"  \"jump_order\": [");
    for(int i=0;i<e.totalPositions;i++)
        fprintf(f,"%d%s",e.jumpOrder[i],i<e.totalPositions-1?",":"");
    fprintf(f,"],\n");
    // visited (current pass)
    fprintf(f,"  \"visited\": [");
    for(int i=0;i<e.totalPositions;i++)
        fprintf(f,"%d%s",e.visitedPos[i]?1:0,i<e.totalPositions-1?",":"");
    fprintf(f,"]\n}\n");
    // FIX (3.4): detect a failed/partial write (e.g. disk full) instead of
    // silently losing progress.
    if(ferror(f)) printf("[Jerboa] WARNING: write error on %s — progress may be incomplete\n", path.c_str());
    if(fclose(f)!=0) printf("[Jerboa] WARNING: failed to flush/close %s\n", path.c_str());
}

// ── jerboa_save_launcher ──────────────────────────────────────────────────────
// Сохраняет настройки лаунчера в файл-сателлит .launcher рядом с .json
// Вызывается один раз при старте из main.cpp (если launcherJson непустой)
void jerboa_save_launcher(const JerboaEngine& e, int gpuId, const char* launcherJson)
{
    if (!launcherJson || launcherJson[0] == '\0') return;
    ensureProgressDir(e);
    bool isSingle = (e.J_sec >= 999999998.0);
    string fname = launcherFile(e, gpuId, isSingle);
    FILE* f = fopen(fname.c_str(), "w");
    if (!f) {
        printf("[Jerboa] WARNING: Cannot write launcher file: %s\n", fname.c_str());
        return;
    }
    fprintf(f, "%s\n", launcherJson);
    fclose(f);
    printf("[Jerboa] Launcher  : %s\n", fname.c_str());
}

bool jerboa_load(JerboaEngine& e, int gpuId, BITCRACK_PARAM* bc)
{
    // ── PATCH 2 (CRITICAL): Compute saveFileBase BEFORE attempting to open
    // file. Otherwise saveFileBase is empty and jsonFile returns wrong name,
    // causing resume to silently fail and lose all progress.
    computeSaveFileBase(e, bc);

    bool isSingle=(bc->jerboaJumpSec>=999999998.0);
    string fname=isSingle?jsonFileSingle(e,gpuId):jsonFile(e,gpuId);
    FILE* f=fopen(fname.c_str(),"r");
    if(!f) return false;
    fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f);
    string txt(sz,'\0'); fread(&txt[0],1,sz,f); fclose(f);

    auto ri=[&](const string& k,int& v)->bool{
        string p="\""+k+"\": ";
        size_t pos=txt.find(p); if(pos==string::npos)return false;
        v=atoi(txt.c_str()+pos+p.size()); return true;};
    auto rd=[&](const string& k,double& v)->bool{
        string p="\""+k+"\": ";
        size_t pos=txt.find(p); if(pos==string::npos)return false;
        v=atof(txt.c_str()+pos+p.size()); return true;};
    auto rs=[&](const string& k,string& v)->bool{
        string p="\""+k+"\": \"";
        size_t pos=txt.find(p); if(pos==string::npos)return false;
        size_t s=pos+p.size(),en=txt.find('"',s);
        if(en==string::npos)return false;
        v=txt.substr(s,en-s); return true;};

    int bc2=0,tb=1,jc=0,jd=0,tp=0,nt=0,pass=0;
    double jsec=10.0;
    string cs,ce;
    int rand_slot=0,lcg_seed=0,lcg_step=1;

    ri("pass_count",pass);
    ri("batch_count",bc2); ri("total_batches",tb);
    ri("jump_cursor",jc);  ri("jumps_done",jd);
    ri("total_positions",tp); ri("num_threads",nt);
    rd("j_sec",jsec); rs("chunk_start",cs); rs("chunk_end",ce);
    ri("rand_slot",rand_slot); ri("lcg_seed",lcg_seed); ri("lcg_step",lcg_step);
    if(!h2i(cs,e.chunkStart)||!h2i(ce,e.chunkEnd)) return false;

    auto parseIntArr=[&](const string& key, int* arr, int maxN)->int{
        string p="\""+key+"\": [";
        size_t pos=txt.find(p); if(pos==string::npos)return 0;
        size_t cur=pos+p.size(); int cnt=0;
        while(cnt<maxN&&cur<txt.size()&&txt[cur]!=']'){
            arr[cnt++]=atoi(txt.c_str()+cur);
            while(cur<txt.size()&&txt[cur]!=','&&txt[cur]!=']')cur++;
            if(cur<txt.size()&&txt[cur]==',')cur++;
        }
        return cnt;};

    // Load slot_batch_positions (new field — fall back to zeros if missing)
    int sbpArr[JERBOA_MAX_POS]={};
    parseIntArr("slot_batch_positions", sbpArr, JERBOA_MAX_POS);
    for(int i=0;i<JERBOA_MAX_POS;i++) e.slotBatchPos[i]=sbpArr[i];

    parseIntArr("jump_order", e.jumpOrder, JERBOA_MAX_POS);
    int visArr[JERBOA_MAX_POS]={};
    int vcnt=parseIntArr("visited", visArr, JERBOA_MAX_POS);
    for(int i=0;i<vcnt;i++) e.visitedPos[i]=(visArr[i]!=0);

    e.passCount=pass;
    e.batchCount=bc2; e.totalBatches=tb;
    e.randSlotMode=(rand_slot!=0); e.slotLCGSeed=lcg_seed; e.slotLCGStep=lcg_step;
    e.jumpCursor=jc; e.jumpsDone=jd;
    e.numThreads=nt; e.J_sec=jsec;

    analyzeSectors(e);
    int pos2=0;
    for(int z=0;z<e.numSectors;z++){
        for(int s=e.sectorFirstSlot[z];s<=e.sectorLastSlot[z];s++){
            e.posMap[pos2].sectorIdx=z; e.posMap[pos2].digit2=s;
            e.posFor[z][s]=pos2; pos2++;
        }
    }
    e.totalPositions=pos2;

    {
        Int chunkSize;
        chunkSize.Set(const_cast<Int*>(&e.chunkEnd));
        chunkSize.Sub(const_cast<Int*>(&e.chunkStart));
        chunkSize.AddOne();
        e.stripeWidth.Set(&chunkSize);
        Int tpI; tpI.SetInt32(e.totalPositions); e.stripeWidth.Div(&tpI);
    }
    {
        string sH_=trim0(i2h(e.chunkStart));
        string eH_=trim0(i2h(e.chunkEnd));
        while(sH_.size()<eH_.size()) sH_.insert(sH_.begin(),'0');
        while(eH_.size()<sH_.size()) eH_.insert(eH_.begin(),'0');
        int hexW=(int)eH_.size();
        Int unitWidth; unitWidth.SetInt32(1);
        for(int _k=0;_k<hexW-2;_k++) unitWidth.ShiftL(4);
        for(int _i=0;_i<e.totalPositions;_i++){
            int _z=e.posMap[_i].sectorIdx,_s=e.posMap[_i].digit2;
            int secD=e.hexFirstSector+_z;
            Int natS; natS.Set(&unitWidth);
            Int fac; fac.SetInt32(secD*16+_s); natS.Mult(&fac);
            Int natE; natE.Set(&natS); natE.Add(&unitWidth); natE.SubOne();
            if(natS.IsLower(const_cast<Int*>(&e.chunkStart)))
                e.posStart[_i].Set(const_cast<Int*>(&e.chunkStart));
            else e.posStart[_i].Set(&natS);
            Int clampE;
            if(natE.IsGreater(const_cast<Int*>(&e.chunkEnd)))
                clampE.Set(const_cast<Int*>(&e.chunkEnd));
            else clampE.Set(&natE);
            e.posSize[_i].Set(&clampE);
            e.posSize[_i].Sub(&e.posStart[_i]);
            e.posSize[_i].AddOne();
        }
    }
    {
        int curP=(e.jumpCursor<e.totalPositions)?e.jumpOrder[e.jumpCursor]:0;
        Int ntI; ntI.SetInt32(nt);
        Int stride; stride.Set(const_cast<Int*>(&e.posSize[curP])); stride.Div(&ntI);
        Int step((uint64_t)(unsigned)e.STEP_SIZE_val);
        Int m1; m1.Set(&step); m1.SubOne();
        Int num; num.Set(&stride); num.Add(&m1); num.Div(&step);
        uint64_t t=num.bits64[0];
        if(num.bits64[1]!=0||t>(uint64_t)INT_MAX)t=INT_MAX;
        e.totalBatches=(int)t; if(e.totalBatches<1)e.totalBatches=1;
    }

    _updateWindow(e,bc);
    bc->jerboaJumpSec=jsec;
    e.jumpStartTime=Timer::get_tick();
    e.speedMkey=1000.0;
    e.lastCounterSnap=0; e.lastSpeedTime=0.0;
    e.displayShown=false; e.inited=true;

    // ── PATCH 3: Restore scatter state for current slot
    // currentSection/Cycle/Micro are derived from slotBatchPos (visitCount).
    // Without this, display shows misleading "Sec 0 / Cyc 1" until first jump.
    if(e.J_sec < 999999998.0 && e.totalPositions > 0) {
        int curP = e.jumpOrder[e.jumpCursor % e.totalPositions];
        int vc = e.slotBatchPos[curP];
        int cycle = vc / e.depthSections;
        int pos   = vc % e.depthSections;
        // PATCH 7: per-slot phase shift (зеркально jerboa_do_jump)
        int slot_phase   = (int)(((uint64_t)(curP + 1) * DEPTH_GOLDEN) % e.depthSections);
        e.currentSection = (int)((((uint64_t)pos * DEPTH_GOLDEN) + slot_phase) % e.depthSections);
        e.currentMicro   = (int)((uint64_t)cycle * DEPTH_GOLDEN % e.depthSections);
        e.currentCycle   = cycle;
    } else {
        e.currentSection = 0;
        e.currentMicro   = 0;
        e.currentCycle   = 0;
    }

    return true;
}

// ── display ───────────────────────────────────────────────────────────────────

static string fmtComma(uint64_t n){
    string s=to_string(n);
    int pos=(int)s.size()-3;
    while(pos>0){s.insert(s.begin()+pos,',');pos-=3;}
    return s;
}

// Build one marker row for a given pass.
// passIdx < passCount  → completed row (all visited = '~')
// passIdx == passCount → current row   (₿ = current, ~ = visited, ' ' = not yet)
static string buildPassRow(const JerboaEngine& e, int passIdx, bool useCommas)
{
    bool isCurrent=(passIdx==e.passCount);
    int curGPos=isCurrent?
        ((e.jumpCursor<e.totalPositions)?e.jumpOrder[e.jumpCursor]:-1)
        :-1;

    // @ = Текущая позиция, ~ = Пройдено, ' ' = Ещё не посещали
    static const string MARK_CURRENT  = "@";   // current slot
    static const string MARK_VISITED  = "~";   // visited
    static const string MARK_EMPTY    = " ";   // not yet visited

    char label[12];
    int displayNum = passIdx + 1;  // показываем с 1
    snprintf(label, sizeof(label), "P:%-5d", displayNum);
    string row = string(label);  // ровно 7 символов всегда
    for(int z=0;z<e.numSectors;z++){
        row+="| ";
        if(useCommas){
            for(int s=e.sectorFirstSlot[z];s<=e.sectorLastSlot[z];s++){
                int pi=e.posFor[z][s];
                const string* m=&MARK_EMPTY;
                if(pi>=0){
                    if(isCurrent){
                        if(pi==curGPos)             m=&MARK_CURRENT;
                        else if(e.visitedPos[pi])   m=&MARK_VISITED;
                    } else {
                        m=&MARK_VISITED;  // completed pass — all slots done
                    }
                }
                row+=*m;
                if(s<e.sectorLastSlot[z]) row+=' ';
            }
        } else {
            for(int s=e.sectorFirstSlot[z];s<=e.sectorLastSlot[z];s++){
                int pi=e.posFor[z][s];
                const string* m=&MARK_EMPTY;
                if(pi>=0){
                    if(isCurrent){
                        if(pi==curGPos)             m=&MARK_CURRENT;
                        else if(e.visitedPos[pi])   m=&MARK_VISITED;
                    } else {
                        m=&MARK_VISITED;
                    }
                }
                row+=*m;
            }
        }
        row+=' ';
    }
    row+='|';
    return row;
}

void jerboa_display(JerboaEngine& e, uint64_t totalKeys)
{
    // Speed update
    {
        double now=Timer::get_tick();
        if(e.lastSpeedTime<1.0){
            e.lastCounterSnap=totalKeys;
            e.lastSpeedTime=Timer::get_tick();
        } else if(Timer::get_tick()-e.lastSpeedTime>=2.0&&totalKeys>e.lastCounterSnap){
            double dt=Timer::get_tick()-e.lastSpeedTime;
            double spd=(double)(totalKeys-e.lastCounterSnap)/dt/1e6;
            e.speedMkey=e.speedMkey*0.4+spd*0.6;
            e.lastCounterSnap=totalKeys;
            e.lastSpeedTime=Timer::get_tick();
        }
    }

    // ETA to next jump
    char etaBuf[16];
    {
        double el=Timer::get_tick()-e.jumpStartTime;
        double tl=e.J_sec-el; if(tl<0.0)tl=0.0;
        int t=(int)tl;
        snprintf(etaBuf,sizeof(etaBuf),"%dm%02ds",t/60,t%60);
    }

    int donePct=(e.totalPositions>0)?(e.jumpsDone*100/e.totalPositions):0;
    string curHex=trim0(i2h(e.windowStart));
    if((int)curHex.size()>20) curHex=curHex.substr(0,20);

    char spdBuf[24];
    if(e.speedMkey>=1000.0)
        snprintf(spdBuf,sizeof(spdBuf),"%.2f Gk/s",e.speedMkey/1000.0);
    else
        snprintf(spdBuf,sizeof(spdBuf),"%.1f Mk/s",e.speedMkey);

    int totalWidthCommas=1;
    for(int z=0;z<e.numSectors;z++)
        totalWidthCommas+=e.sectorSlotCount[z]*2+2;
    bool useCommas=(totalWidthCommas<=137);

    // Sample key
    {
        double now2=Timer::get_tick();
        if(e.lastSampleTime<1.0||now2-e.lastSampleTime>=0.5){
            e.lastSampleTime=now2;
            Int offset; offset.SetInt32(0);
            int _curP=(e.jumpCursor<e.totalPositions)?e.jumpOrder[e.jumpCursor]:0;
            uint64_t sw0=e.posSize[_curP].bits64[0];
            uint64_t sw1=e.posSize[_curP].bits64[1];
            uint64_t r0=jerboa_rand64(),r1=jerboa_rand64();
            if(sw1==0){ offset.bits64[0]=sw0>1?r0%sw0:0; offset.bits64[1]=0; }
            else { offset.bits64[0]=r0; offset.bits64[1]=r1%sw1; }
            e.sampleKey.Set(const_cast<Int*>(&e.windowStart));
            e.sampleKey.Add(&offset);
            if(!e.sampleKey.IsLower(const_cast<Int*>(&e.windowEnd))&&
               !e.sampleKey.IsEqual(const_cast<Int*>(&e.windowEnd)))
                e.sampleKey.Set(const_cast<Int*>(&e.windowEnd));
        }
    }
    string detailHex=trim0(i2h(e.sampleKey));
    if((int)detailHex.size()>20) detailHex=detailHex.substr(0,20);

    // ── How many completed passes to show ────────────────────────────────────
    // Total lines = 3 headers + showPasses + 1 current  (max 12)
    // L[4..12]=pass rows (до 9), L[13]=current
    // 10 видимых строк таблицы = 9 завершённых + 1 текущая
    // Итого max: 4+9+1=14 → _L[14] в Jerboa.h
    int showPasses=e.passCount<JERBOA_MAX_SHOW_PASSES?e.passCount:JERBOA_MAX_SHOW_PASSES;
    e._numDisplayLines=4+showPasses+1;  // 5..14

    // ── Build lines ──────────────────────────────────────────────────────────

    // L0: Двухуровневый scatter — Вариант А
    {
        char tmp2[220];
        // Mirror the FIX (3.3) scaling used in jerboa_do_jump so the displayed
        // "+N batches" is the real distance past this section's start.
        int tb = (e.totalBatches > 1) ? e.totalBatches : 1;
        int DS = e.depthSections;
        int64_t sectionStart = ((int64_t)e.currentSection * (int64_t)tb) / (int64_t)DS;
        int64_t sectionSpan  = (int64_t)tb / (int64_t)DS;
        int64_t microDepth   = (sectionSpan > 0)
                             ? ((int64_t)e.currentMicro * sectionSpan) / (int64_t)DS
                             : 0;
        int64_t base = sectionStart + microDepth;
        int intra = (int)((int64_t)e.batchCount - base);
        if(intra < 0) intra = 0;
        snprintf(tmp2,219,
            "[Jerboa] Scatter   : Sec %d/%d  Cyc %d  (+%d batches)",
            e.currentSection, e.depthSections, e.currentCycle + 1, intra);
        e._L[0]=tmp2;
    }

    // L1: строка статуса работы
    {
        char tmp[220];
        string keyStr=fmtComma(e.slotKeyCount);
        double depthPct=(e.totalBatches>0)
                    ?((double)e.batchCount*100.0/(double)e.totalBatches):0.0;
        snprintf(tmp,219,
            "[ Key: %s] [Pass:%d] [Done:%d%%] [Speed:%s] [Next jump:%s] [Depth:%.2f%%] [Current:0x%s] [Scatter P:%s]",
            keyStr.c_str(), e.passCount+1, donePct, spdBuf, etaBuf,
            depthPct, curHex.c_str(), e.scatterPosHex);
        e._L[1]=tmp;
    }

    // L2: sector labels (7 пробелов = ширина префикса "P:10000")
    e._L[2]="       ";
    for(int z=0;z<e.numSectors;z++){
        int secDigit=e.hexFirstSector+z;
        char sc=vc(secDigit);
        int contentW=useCommas?(e.sectorSlotCount[z]*2+1):(e.sectorSlotCount[z]+2);
        int lpad=contentW/2, rpad=contentW-1-lpad;
        e._L[2]+='|';
        e._L[2].append(lpad,' ');
        e._L[2]+=sc;
        e._L[2].append(rpad,' ');
    }
    e._L[2]+='|';

    // L3: slot labels
    e._L[3]="       ";
    for(int z=0;z<e.numSectors;z++){
        e._L[3]+="| ";
        if(useCommas){
            for(int s=e.sectorFirstSlot[z];s<=e.sectorLastSlot[z];s++){
                e._L[3]+=vc(s);
                if(s<e.sectorLastSlot[z]) e._L[3]+=',';
            }
        } else {
            for(int s=e.sectorFirstSlot[z];s<=e.sectorLastSlot[z];s++)
                e._L[3]+=vc(s);
        }
        e._L[3]+=' ';
    }
    e._L[3]+='|';

    // L4..4+showPasses-1: completed pass rows
    // Show the LAST showPasses completed passes (oldest at top, newest just above current)
    int firstShowPass=(e.passCount>showPasses)?(e.passCount-showPasses):0;
    for(int pi=0;pi<showPasses;pi++){
        int passIdx=firstShowPass+pi;
        e._L[4+pi]=buildPassRow(e,passIdx,useCommas);
    }

    // L4+showPasses: current pass row
    e._L[4+showPasses]=buildPassRow(e,e.passCount,useCommas);

    // Pad all lines to 189 chars
    for(int i=0;i<e._numDisplayLines;i++){
        if((int)e._L[i].size()<189) e._L[i].append(189-(int)e._L[i].size(),' ');
        e._L[i].resize(189);
    }

    // ── Write to console ─────────────────────────────────────────────────────
#if defined(_WIN32)||defined(_WIN64)
    static COORD  jerboa_dispRow={0,0};
    static HANDLE jerboa_hCon=INVALID_HANDLE_VALUE;
    if(jerboa_hCon==INVALID_HANDLE_VALUE)
        jerboa_hCon=CreateFileA("CONOUT$",GENERIC_READ|GENERIC_WRITE,
                                FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);

    if(jerboa_hCon!=INVALID_HANDLE_VALUE){
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        GetConsoleScreenBufferInfo(jerboa_hCon,&csbi);

        // Reinit display area when:
        // 1. First show
        // 2. Number of display lines increased (new pass completed)
        // 3. Display scrolled off screen
        // Relocate only on first show or truly off screen
        bool relocate=!e.displayShown||
                      jerboa_dispRow.Y<csbi.srWindow.Top||
                      jerboa_dispRow.Y+e._numDisplayLines-1>csbi.srWindow.Bottom;

        if(!relocate && e._numDisplayLines!=e._prevDisplayLines){
            // Display grew (new pass row added) — extend reserved area downward
            DWORD w;
            SHORT newRow=(SHORT)(jerboa_dispRow.Y+e._numDisplayLines-1);
            COORD lp={0,newRow};
            FillConsoleOutputCharacter(jerboa_hCon,' ',csbi.dwSize.X,lp,&w);
            COORD below={0,(SHORT)(jerboa_dispRow.Y+e._numDisplayLines)};
            SetConsoleCursorPosition(jerboa_hCon,below);
            e._prevDisplayLines=e._numDisplayLines;
        }

        if(relocate){
            fflush(stdout);
            GetConsoleScreenBufferInfo(jerboa_hCon,&csbi);
            jerboa_dispRow={0,csbi.dwCursorPosition.Y};
            DWORD w;
            for(int i=0;i<e._numDisplayLines;i++){
                COORD lp={0,(SHORT)(jerboa_dispRow.Y+i)};
                FillConsoleOutputCharacter(jerboa_hCon,' ',csbi.dwSize.X,lp,&w);
            }
            COORD below={0,(SHORT)(jerboa_dispRow.Y+e._numDisplayLines)};
            SetConsoleCursorPosition(jerboa_hCon,below);
            fflush(stdout);
            e._prevDisplayLines=e._numDisplayLines;
        }

        DWORD w;
        for(int i=0;i<e._numDisplayLines;i++){
            COORD pos={0,(SHORT)(jerboa_dispRow.Y+i)};
            WriteConsoleOutputCharacterA(jerboa_hCon,e._L[i].c_str(),189,pos,&w);
        }
    }
    e.displayShown=true;
#else
    if(!e.displayShown){printf("\033[s");fflush(stdout);}
    else printf("\033[u\r");
    e.displayShown=true;
    for(int i=0;i<e._numDisplayLines;i++) printf("%s\n",e._L[i].c_str());
    fflush(stdout);
#endif
}


// ── VanitySearch::findKeyGPU_Jerboa ──────────────────────────────────────────

void VanitySearch::findKeyGPU_Jerboa(TH_PARAM* ph)
{
    bool ok=true;
    vector<ITEM> found;
    ph->hasStarted=true;
    endOfSearch=false;

    static JerboaEngine eng;
    static bool setupDone=false;

    while(!endOfSearch){
        if(Pause){
            if(!Paused){
                printf("\n[Jerboa] Paused. Press 'p' to resume.\r");
                fflush(stdout); Paused=true;
            }
            Timer::SleepMillis(100); continue;
        }
        if(Paused){printf("[Jerboa] Resuming...\n");fflush(stdout);}

        GPUEngine g(ph->gpuId,maxFound);
        if(!g.IsInitialised()) break;

        if(!setupDone) printf("[+] GPU: %s\n",g.deviceName.c_str());

        if(searchType==PUBKEY)
            g.SetTargetPublicKey(targetPubKeyX,targetPubKeyParity);
        else{
            g.SetSearchType(searchType);
            g.SetAddress(usedAddressL,nbAddress);
            // OPT: Single-target fast path (puzzle hunting)
            // Активирует SINGLE_TARGET_MODE — регистровое сравнение hash160
            if(nbAddress==1&&onlyFull&&!usedAddress.empty()){
                int p=usedAddress[0];
                if(addresses[p].items&&!addresses[p].items->empty()){
                    uint8_t* h160=(*addresses[p].items)[0].hash160;
                    g.SetHash160Target(h160,true);
                }
            }
        }

        int numThreadsGPU=g.GetNbThread();
        int STEP_SIZE    =g.GetStepSize();
        int GROUP_SIZE   =g.GetGroupSize();

        if(!setupDone){
            setupDone=true;
            if(backupMode){
                if(jerboa_load(eng,ph->gpuId,bc)){
                    printf("[Jerboa] Resumed: pass %d  jump %d/%d  batch %d/%d\n",
                           eng.passCount+1,eng.jumpCursor,eng.totalPositions,
                           eng.batchCount,eng.totalBatches);
                } else {
                    printf("[Jerboa] No backup found, starting fresh.\n");
                }
            }
            if(!eng.inited){
                eng.STEP_SIZE_val=STEP_SIZE;
                jerboa_setup(eng,numThreadsGPU,STEP_SIZE,bc,ph->gpuId);
            }
            eng.randSlotMode=bc->randSlotMode;
            printf("[Jerboa] Positions : %d\n",eng.totalPositions);
            printf("[Jerboa] Batches   : %d per stripe  Jump: %.1f sec\n",
                   eng.totalBatches,eng.J_sec);
            if(eng.J_sec >= 999999998.0) {
                printf("[Jerboa] Mode      : NO-JUMP sequential\n");
                printf("[Jerboa] Scatter   : DISABLED\n");
            } else {
                printf("[Jerboa] Mode      : YES-JUMP multi-pass (runs until stopped)\n");
                printf("[Jerboa] Scatter   : ENABLED (phi=0.618 thread + depth distribution)\n");
            }
            fflush(stdout);
        }

        // OPT: сразу показываем scatter позицию нового слота
        if(setupDone){
            updateScatterPosHex(eng);
            jerboa_display(eng, counters[ph->threadId]);
        }

        {
            uint64_t progress=(uint64_t)eng.batchCount*(uint64_t)STEP_SIZE;
            Point* pk=new Point[numThreadsGPU];
            // Всегда подавляем "Setting starting keys..." после первого запуска
            // чтобы не ломать позиционирование дисплея при прыжках
            static bool jerboa_first_setup=true;
            if(!jerboa_first_setup){
#ifdef _WIN32
                HANDLE hNul=CreateFileA("NUL",GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
                HANDLE hSav=GetStdHandle(STD_OUTPUT_HANDLE);
                SetStdHandle(STD_OUTPUT_HANDLE,hNul);
                getGPUStartingKeys(bc->ksStart,bc->ksFinish,GROUP_SIZE,numThreadsGPU,pk,progress);
                SetStdHandle(STD_OUTPUT_HANDLE,hSav);
                CloseHandle(hNul);
#else
                // Linux: redirect stdout to /dev/null
                int devNull=open("/dev/null",O_WRONLY);
                int stdoutFd=dup(STDOUT_FILENO);
                dup2(devNull,STDOUT_FILENO);
                getGPUStartingKeys(bc->ksStart,bc->ksFinish,GROUP_SIZE,numThreadsGPU,pk,progress);
                dup2(stdoutFd,STDOUT_FILENO);
                close(devNull); close(stdoutFd);
#endif
            } else {
                // Первый запуск — показываем "Setting starting keys..."
                getGPUStartingKeys(bc->ksStart,bc->ksFinish,GROUP_SIZE,numThreadsGPU,pk,progress);
                jerboa_first_setup=false;
            }
            // SCATTER: YES-JUMP режим — рассеиваем потоки по φ
            if(eng.J_sec < 999999998.0)
                applyScatter(pk, numThreadsGPU);
            ok=g.SetKeys(pk);
            delete[] pk;
            eng.sectorLevel|=0x1000;
        }

        idxcount=eng.batchCount;
        Paused=false;
        double tLastSave=Timer::get_tick();

        while(ok&&!endOfSearch&&!Pause){
            ok=g.Launch(found,false);
            idxcount++;
            eng.batchCount++;
            counters[ph->threadId]+=(uint64_t)numThreadsGPU*STEP_SIZE;
            eng.slotKeyCount      +=(uint64_t)numThreadsGPU*STEP_SIZE;

            // Slot exhaustion check
            if(eng.batchCount>=eng.totalBatches&&
               eng.totalBatches<(INT_MAX-1)){
                if(backupMode){
                    if(eng.J_sec>=999999998.0)
                        jerboa_save_single(eng,ph->gpuId);
                    else
                        jerboa_save(eng,ph->gpuId);
                }
                if(eng.J_sec>=999999998.0){
                    // NO-JUMP mode: stop after exhaustion
                    printf("\n[Jerboa] Slot fully exhausted (No-jump mode). Done!\n");
                    fflush(stdout);
                    endOfSearch=true;
                } else {
                    // YES-JUMP multi-pass: slot exhausted → jump inline, no break
                    // YES-JUMP: jerboa_do_jump сам инкрементирует visit count
                    // NO-JUMP: нужно сохранить batch position перед вызовом
                    if(eng.J_sec >= 999999998.0)
                        eng.slotBatchPos[eng.jumpOrder[eng.jumpCursor]]=eng.batchCount;
                    if(!jerboa_do_jump(eng,bc)){
                        endOfSearch=true; break;
                    }
                    updateScatterPosHex(eng);
                    idxcount=eng.batchCount;
                    {
                        uint64_t progress2=(uint64_t)eng.batchCount*(uint64_t)STEP_SIZE;
                        Point* pk3=new Point[numThreadsGPU];
#ifdef _WIN32
                        HANDLE hN2=CreateFileA("NUL",GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
                        HANDLE hO2=GetStdHandle(STD_OUTPUT_HANDLE);
                        SetStdHandle(STD_OUTPUT_HANDLE,hN2);
                        getGPUStartingKeys(bc->ksStart,bc->ksFinish,
                                           GROUP_SIZE,numThreadsGPU,pk3,progress2);
                        SetStdHandle(STD_OUTPUT_HANDLE,hO2);
                        CloseHandle(hN2);
#else
                        int _devNull=open("/dev/null",O_WRONLY);
                        int _stdFd=dup(STDOUT_FILENO);
                        dup2(_devNull,STDOUT_FILENO);
                        getGPUStartingKeys(bc->ksStart,bc->ksFinish,
                                           GROUP_SIZE,numThreadsGPU,pk3,progress2);
                        dup2(_stdFd,STDOUT_FILENO);
                        close(_devNull); close(_stdFd);
#endif
                        // SCATTER: YES-JUMP inline jump — рассеиваем потоки по φ
                        applyScatter(pk3, numThreadsGPU);
                        ok=g.SetKeys(pk3);
                        delete[] pk3;
                    }
                }
            }

            // Private key reconstruction
            {
                Int stepThread;
                stepThread.Set(&bc->ksFinish);
                stepThread.Sub(&bc->ksStart);
                stepThread.AddOne();
                Int nt2; nt2.SetInt32(numThreadsGPU); stepThread.Div(&nt2);
                Int keycount((uint64_t)((uint64_t)(idxcount-1)*(uint64_t)STEP_SIZE));
                for(int i=0;i<(int)found.size()&&!endOfSearch;i++){
                    ITEM it=found[i];
                    // SCATTER FIX: поток it.thId стартовал с scatter_pos*stepThread
                    // scatter_pos = (thId * SCATTER_GOLDEN64) % N (thread scatter)
                    uint64_t scatter_pos = ((uint64_t)it.thId * SCATTER_GOLDEN64)
                                          % (uint64_t)numThreadsGPU;
                    // NO-JUMP: scatter_thread выключен → линейный порядок
                    if(eng.J_sec >= 999999998.0)
                        scatter_pos = (uint64_t)it.thId;
                    Int part; part.Set(&stepThread); part.Mult(scatter_pos);
                    Int privkey; privkey.Set(&bc->ksStart);
                    privkey.Add(&part); privkey.Add(&keycount);
                    if(searchType==PUBKEY){
                        Int k(&privkey);
                        if(it.incr<0){k.Add((uint64_t)(-it.incr));k.Neg();k.Add(&secp->order);}
                        else k.Add((uint64_t)it.incr);
                        output(inputAddresses[0],secp->GetPrivAddress(true,k),k.GetBase16());
                        nbFoundKey++; updateFound();
                    } else {
                        checkAddr(*(address_t*)(it.hash),it.hash,privkey,it.incr,it.endo,it.mode);
                    }
                }
            }

            jerboa_display(eng,counters[ph->threadId]);

            // Auto-save every 60s
            if(backupMode){
                double now=Timer::get_tick();
                if(now-tLastSave>=60.0){
                    if(eng.J_sec>=999999998.0)
                        jerboa_save_single(eng,ph->gpuId);
                    else
                        jerboa_save(eng,ph->gpuId);
                    tLastSave=now;
                }
            }

            // Time-based jump
            if(Timer::get_tick()-eng.jumpStartTime>=eng.J_sec){
                // YES-JUMP: jerboa_do_jump инкрементирует visit count
                // NO-JUMP: сохраняем позицию батча перед прыжком
                if(eng.J_sec >= 999999998.0)
                    eng.slotBatchPos[eng.jumpOrder[eng.jumpCursor]]=eng.batchCount;
                bool jumpOk = jerboa_do_jump(eng,bc);
                updateScatterPosHex(eng);
                if(backupMode){
                    if(eng.J_sec>=999999998.0)
                        jerboa_save_single(eng,ph->gpuId);
                    else
                        jerboa_save(eng,ph->gpuId);
                }
                if(!jumpOk){ endOfSearch=true; }
                idxcount=0; break;
            }
        }
        t_Paused+=0.0;
    }
    ph->isRunning=false;
}
