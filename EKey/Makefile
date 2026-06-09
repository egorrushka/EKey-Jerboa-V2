# =====================================================================
#  EKey-Jerboa V2.0.0  --  Linux Makefile
#  Based on VanitySearch by Jean Luc PONS (GPLv3)
#  Author fork: egorrushka   |   Code by: Claude (Anthropic)
#  Written in Ukraine, Chernihiv.   Contact: egor.gr1@gmail.com
# =====================================================================
#
#  Requirements:
#    * NVIDIA CUDA Toolkit (provides nvcc)
#    * A C++17 host compiler (g++)
#
#  Usage:
#    make                      # build with default multi-arch fat binary
#    make CUDA_ARCH=sm_86      # build for a single architecture only
#    make CCAP="75 86 89"      # custom compute-capability list (multi-arch)
#    make clean                # remove the binary and intermediates
#    make run ARGS="-a <addr> -s 0x.. -e 0x.."
#
#  Notes:
#    * NVCC must be in PATH, or pass NVCC=/usr/local/cuda/bin/nvcc.
#    * Keyboard 'p' pause is Windows-only; the Linux build still runs and
#      saves/restores progress normally.
# =====================================================================

NVCC      ?= nvcc
TARGET    ?= EKey-Jerboa

# ---- Architecture selection ----------------------------------------
# Default: fat binary covering Turing/Ampere/Ada + forward-compat PTX.
# Override either with CUDA_ARCH (single) or CCAP (space-separated list).
CCAP      ?= 75 86 89

ifdef CUDA_ARCH
  # single arch, e.g. CUDA_ARCH=sm_86
  ARCH_NUM  := $(patsubst sm_%,%,$(CUDA_ARCH))
  GENCODE   := -gencode arch=compute_$(ARCH_NUM),code=sm_$(ARCH_NUM)
else
  # multi-arch fat binary from CCAP list, plus PTX of the highest for JIT
  GENCODE   := $(foreach cc,$(CCAP),-gencode arch=compute_$(cc),code=sm_$(cc))
  HICC      := $(lastword $(CCAP))
  GENCODE   += -gencode arch=compute_$(HICC),code=compute_$(HICC)
endif

# ---- Flags ----------------------------------------------------------
NVCCFLAGS := -O3 -m64 -DWITHGPU -std=c++17 $(GENCODE)
HOSTFLAGS := -Xcompiler "-O3 -fopenmp -Wno-write-strings"
INCLUDES  := -I.
LDFLAGS   := -Xcompiler "-fopenmp"

# ---- Sources --------------------------------------------------------
CPP_SRC := \
    main.cpp Vanity.cpp Jerboa.cpp \
    Int.cpp IntMod.cpp IntGroup.cpp \
    Point.cpp SECP256K1.cpp \
    Base58.cpp Bech32.cpp \
    Timer.cpp Random.cpp Wildcard.cpp \
    hash/sha256.cpp hash/sha256_sse.cpp \
    hash/ripemd160.cpp hash/ripemd160_sse.cpp \
    hash/sha512.cpp \
    GPU/GPUGenerate.cpp

CU_SRC  := GPU/GPUEngine.cu

SRC     := $(CPP_SRC) $(CU_SRC)

# =====================================================================
.PHONY: all clean run help

all: $(TARGET)

# Single nvcc invocation (mirrors compile_modern.bat); nvcc drives the
# host compiler for the .cpp files and compiles the .cu itself.
$(TARGET): $(SRC)
	$(NVCC) $(NVCCFLAGS) $(HOSTFLAGS) $(INCLUDES) $(SRC) $(LDFLAGS) -o $(TARGET)
	@echo
	@echo "[OK] $(TARGET) built."
	@echo "     arch: $(if $(CUDA_ARCH),$(CUDA_ARCH),fat [$(CCAP)] + PTX)"

run: $(TARGET)
	./$(TARGET) $(ARGS)

clean:
	rm -f $(TARGET) *.o GPU/*.o hash/*.o
	@echo "[OK] cleaned."

help:
	@echo "make                 - multi-arch fat binary (sm_75/86/89 + PTX)"
	@echo "make CUDA_ARCH=sm_86 - single architecture"
	@echo "make CCAP=\"75 86\"    - custom multi-arch list"
	@echo "make clean           - remove binary"
	@echo "make run ARGS=\"...\"  - build and run with arguments"
