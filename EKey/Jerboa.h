// Jerboa.h
// EKey-Jerboa V2.0.0  -  Single-point interleave across chunk positions
// Author fork: egorrushka
// Based on VanitySearch by Jean Luc PONS (GPLv3)
#pragma once
#include "Vanity.h"
#include "Int.h"
#include <string>

// ── Engine ────────────────────────────────────────────────────────────────────

struct JerboaPos {
    int sectorIdx;
    int digit2;
};

#define JERBOA_MAX_POS        256
#define JERBOA_MAX_SHOW_PASSES  9   // max completed-pass rows shown in console
                                    // total display lines = 4 headers + 9 + 1 current = 14

// ── Depth Scatter — Вариант А: двухуровневое золотое сечение ───────────────
// MAX_DEPTH_SECTIONS: слот делится на N равных секций (макро-уровень).
// DEPTH_GOLDEN:       шаг по золотому сечению, gcd(6181,N)=1 → биекция.
//   Уровень 1 (макро): какую секцию посетить
//     slot_phase = ((slotIdx+1) * 6181) % N          (сдвиг на слот)
//     section    = (posInCycle * 6181 + slot_phase) % N
//   Уровень 2 (микро): глубина внутри секции, цикл за циклом
//     micro      = (cycle * 6181) % N
//   Абсолютный батч масштабируется на размер слота (см. jerboa_do_jump,
//   FIX 3.3): при tb < N система деградирует плавно, без коллапса в 0.
//   Первые секции: 0%, 61.8%, 23.6%, 85.4%, 47.2%, 9.0%, 70.9%, 32.7%...
#define MAX_DEPTH_SECTIONS  10000
#define DEPTH_GOLDEN        6181ULL

struct JerboaEngine {
    Int  chunkStart, chunkEnd;
    int  sectorLevel;
    int  numSectors;
    int  hexFirstSector;

    int  sectorFirstSlot[16];
    int  sectorLastSlot[16];
    int  sectorSlotCount[16];

    int       totalPositions;
    JerboaPos posMap[JERBOA_MAX_POS];
    int       posFor[16][16];

    int  jumpOrder[JERBOA_MAX_POS];
    int  jumpCursor;
    int  jumpsDone;
    bool visitedPos[JERBOA_MAX_POS];

    // ── Multi-pass ────────────────────────────────────────────────────────────
    int  passCount;                     // completed passes (0 = first pass running)
    int  slotBatchPos[JERBOA_MAX_POS];  // accumulated batches per slot across all passes

    Int  posStart[JERBOA_MAX_POS];
    Int  posSize [JERBOA_MAX_POS];

    Int  stripeWidth;
    Int  windowStart;
    Int  windowEnd;

    int     batchCount;
    int     totalBatches;
    int     numThreads;
    int     STEP_SIZE_val;

    double  J_sec;
    double  jumpStartTime;

    double   speedMkey;
    uint64_t slotKeyCount;
    char scatterPosHex[80];        // hex ключ scatter позиции для текущего визита
    int  currentSection;           // текущая макро-секция [0..9999]
    int  currentCycle;             // текущий цикл (0=первый, 1=второй...)
    int  currentMicro;             // микро-смещение внутри секции [0..9999]
    int  depthSections;             // активное число секций: 100/500/1000/5000/10000
    char saveFileBase[200];        // база имени файла: jerboa_{puz}_{lo}_{hi}_gpu{N}
    char progressDir[300];         // папка прогресса: p{puz}_{start}-{end}_{mode}
    uint64_t lastCounterSnap;
    double   lastSpeedTime;

    Int    sampleKey;
    double lastSampleTime;

    int    _numDisplayLines;   // current rows written to console (4..12)
    int    _prevDisplayLines;  // previous value; triggers reinit on change

    bool     randSlotMode;
    int      slotLCGSeed;
    int      slotLCGStep;
    bool     displayShown;
    bool     inited;

    // 13 line buffers: (L[0]=depth, L[1]=status, L[2]=sectors, L[3]=slots, L[4..11]=passes, L[12]=current)
    // [0]     status
    // [1]     sector labels
    // [2]     slot labels
    // [4..12] completed pass rows (up to JERBOA_MAX_SHOW_PASSES=9)
    // [13]    current pass row
    std::string _L[14];

    JerboaEngine() :
        sectorLevel(0), numSectors(1), hexFirstSector(0),
        totalPositions(0), jumpCursor(0), jumpsDone(0),
        passCount(0),
        batchCount(0), totalBatches(1), numThreads(1), STEP_SIZE_val(1024),
        J_sec(10.0), jumpStartTime(0.0),
        slotKeyCount(0), speedMkey(1000.0),
        lastCounterSnap(0), lastSpeedTime(0.0), lastSampleTime(0.0),
        _numDisplayLines(4), _prevDisplayLines(0),
        randSlotMode(false), slotLCGSeed(0), slotLCGStep(1),
        displayShown(false), inited(false), depthSections(10000)
    {
        for(int i=0;i<16;i++){sectorFirstSlot[i]=0;sectorLastSlot[i]=15;sectorSlotCount[i]=16;}
        for(int i=0;i<JERBOA_MAX_POS;i++){
            jumpOrder[i]=i; visitedPos[i]=false; slotBatchPos[i]=0;
        }
        for(int z=0;z<16;z++) for(int s=0;s<16;s++) posFor[z][s]=-1;
    }
};

// ── API ───────────────────────────────────────────────────────────────────────
void jerboa_setup      (JerboaEngine& e, int numThreads, int STEP_SIZE, BITCRACK_PARAM* bc, int gpuId=0);
bool jerboa_do_jump    (JerboaEngine& e, BITCRACK_PARAM* bc);
void jerboa_save       (const JerboaEngine& e, int gpuId);
void jerboa_save_single(const JerboaEngine& e, int gpuId);
bool jerboa_load       (JerboaEngine& e, int gpuId, BITCRACK_PARAM* bc);
void jerboa_display    (JerboaEngine& e, uint64_t totalKeys);
void jerboa_save_launcher(const JerboaEngine& e, int gpuId, const char* launcherJson);
