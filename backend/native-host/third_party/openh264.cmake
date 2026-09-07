# OpenH264 (BSD-2-Clause, on the allow-list in LICENSE.md), built from the
# pinned submodule as a static library — encoder only.
#
# ── Why this file exists ─────────────────────────────────────────────────────
#
# Upstream ships a Makefile and a meson build, no CMake. The source lists below
# are copied from codec/{common,processing,encoder}/targets.mk, which upstream
# generates from build/mktargets.py; if the submodule moves, those three files
# are the reference to re-check against. The decoder is left out entirely: the
# engine never decodes, and half the library is the half we do not want to
# carry, audit or warn about.
#
# ── The assembler question ───────────────────────────────────────────────────
#
# On x86-64 the hand-written NASM kernels are roughly 2–3× the speed of the C
# fall-backs, and a CPU encoder exists for machines with nothing to spare — so
# they are wanted. NASM is looked for on the PATH, in MW_NASM, and in vcpkg's
# tool cache; found → assembled, not found → the C paths are compiled and the
# configure says so LOUDLY, because a build that silently ships the slow encoder
# would be measured and misjudged.
#
# On ARM64 the kernels are GAS-syntax `.S` files. GCC and clang take them as-is
# (Linux), MSVC's armasm64 does not: on Windows-on-ARM built with MSVC this is
# the C-only encoder, and Media Foundation — one tier above it — is the path
# that machine is meant to take (MfEncoder.h).

set(MW_OPENH264_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/openh264)

if(NOT EXISTS ${MW_OPENH264_DIR}/codec/api/wels/codec_api.h)
    message(FATAL_ERROR "mw-native-host: the openh264 submodule is not checked out "
                        "(git submodule update --init backend/native-host/third_party/openh264)")
endif()

set(MW_OPENH264_COMMON_SRCS
    codec/common/src/common_tables.cpp
    codec/common/src/copy_mb.cpp
    codec/common/src/cpu.cpp
    codec/common/src/crt_util_safe_x.cpp
    codec/common/src/deblocking_common.cpp
    codec/common/src/expand_pic.cpp
    codec/common/src/intra_pred_common.cpp
    codec/common/src/mc.cpp
    codec/common/src/memory_align.cpp
    codec/common/src/sad_common.cpp
    codec/common/src/utils.cpp
    codec/common/src/welsCodecTrace.cpp
    codec/common/src/WelsTaskThread.cpp
    codec/common/src/WelsThread.cpp
    codec/common/src/WelsThreadLib.cpp
    codec/common/src/WelsThreadPool.cpp)

set(MW_OPENH264_PROCESSING_SRCS
    codec/processing/src/adaptivequantization/AdaptiveQuantization.cpp
    codec/processing/src/backgrounddetection/BackgroundDetection.cpp
    codec/processing/src/common/memory.cpp
    codec/processing/src/common/WelsFrameWork.cpp
    codec/processing/src/common/WelsFrameWorkEx.cpp
    codec/processing/src/complexityanalysis/ComplexityAnalysis.cpp
    codec/processing/src/denoise/denoise.cpp
    codec/processing/src/denoise/denoise_filter.cpp
    codec/processing/src/downsample/downsample.cpp
    codec/processing/src/downsample/downsamplefuncs.cpp
    codec/processing/src/imagerotate/imagerotate.cpp
    codec/processing/src/imagerotate/imagerotatefuncs.cpp
    codec/processing/src/scenechangedetection/SceneChangeDetection.cpp
    codec/processing/src/scrolldetection/ScrollDetection.cpp
    codec/processing/src/scrolldetection/ScrollDetectionFuncs.cpp
    codec/processing/src/vaacalc/vaacalcfuncs.cpp
    codec/processing/src/vaacalc/vaacalculation.cpp)

set(MW_OPENH264_ENCODER_SRCS
    codec/encoder/core/src/au_set.cpp
    codec/encoder/core/src/deblocking.cpp
    codec/encoder/core/src/decode_mb_aux.cpp
    codec/encoder/core/src/encode_mb_aux.cpp
    codec/encoder/core/src/encoder.cpp
    codec/encoder/core/src/encoder_data_tables.cpp
    codec/encoder/core/src/encoder_ext.cpp
    codec/encoder/core/src/get_intra_predictor.cpp
    codec/encoder/core/src/md.cpp
    codec/encoder/core/src/mv_pred.cpp
    codec/encoder/core/src/nal_encap.cpp
    codec/encoder/core/src/paraset_strategy.cpp
    codec/encoder/core/src/picture_handle.cpp
    codec/encoder/core/src/ratectl.cpp
    codec/encoder/core/src/ref_list_mgr_svc.cpp
    codec/encoder/core/src/sample.cpp
    codec/encoder/core/src/set_mb_syn_cabac.cpp
    codec/encoder/core/src/set_mb_syn_cavlc.cpp
    codec/encoder/core/src/slice_multi_threading.cpp
    codec/encoder/core/src/svc_base_layer_md.cpp
    codec/encoder/core/src/svc_enc_slice_segment.cpp
    codec/encoder/core/src/svc_encode_mb.cpp
    codec/encoder/core/src/svc_encode_slice.cpp
    codec/encoder/core/src/svc_mode_decision.cpp
    codec/encoder/core/src/svc_motion_estimate.cpp
    codec/encoder/core/src/svc_set_mb_syn_cabac.cpp
    codec/encoder/core/src/svc_set_mb_syn_cavlc.cpp
    codec/encoder/core/src/wels_preprocess.cpp
    codec/encoder/core/src/wels_task_base.cpp
    codec/encoder/core/src/wels_task_encoder.cpp
    codec/encoder/core/src/wels_task_management.cpp
    codec/encoder/plus/src/welsEncoderExt.cpp)

set(MW_OPENH264_X86_ASM
    codec/common/x86/cpuid.asm
    codec/common/x86/dct.asm
    codec/common/x86/deblock.asm
    codec/common/x86/expand_picture.asm
    codec/common/x86/intra_pred_com.asm
    codec/common/x86/mb_copy.asm
    codec/common/x86/mc_chroma.asm
    codec/common/x86/mc_luma.asm
    codec/common/x86/satd_sad.asm
    codec/common/x86/vaa.asm
    codec/processing/src/x86/denoisefilter.asm
    codec/processing/src/x86/downsample_bilinear.asm
    codec/processing/src/x86/vaa.asm
    codec/encoder/core/x86/coeff.asm
    codec/encoder/core/x86/dct.asm
    codec/encoder/core/x86/intra_pred.asm
    codec/encoder/core/x86/matrix_transpose.asm
    codec/encoder/core/x86/memzero.asm
    codec/encoder/core/x86/quant.asm
    codec/encoder/core/x86/sample_sc.asm
    codec/encoder/core/x86/score.asm)

set(MW_OPENH264_ARM64_ASM
    codec/common/arm64/copy_mb_aarch64_neon.S
    codec/common/arm64/deblocking_aarch64_neon.S
    codec/common/arm64/expand_picture_aarch64_neon.S
    codec/common/arm64/intra_pred_common_aarch64_neon.S
    codec/common/arm64/mc_aarch64_neon.S
    codec/processing/src/arm64/adaptive_quantization_aarch64_neon.S
    codec/processing/src/arm64/down_sample_aarch64_neon.S
    codec/processing/src/arm64/pixel_sad_aarch64_neon.S
    codec/processing/src/arm64/vaa_calc_aarch64_neon.S
    codec/encoder/core/arm64/intra_pred_aarch64_neon.S
    codec/encoder/core/arm64/intra_pred_sad_3_opt_aarch64_neon.S
    codec/encoder/core/arm64/memory_aarch64_neon.S
    codec/encoder/core/arm64/pixel_aarch64_neon.S
    codec/encoder/core/arm64/reconstruct_aarch64_neon.S
    codec/encoder/core/arm64/svc_motion_estimation_aarch64_neon.S)

set(MW_OPENH264_SRCS ${MW_OPENH264_COMMON_SRCS} ${MW_OPENH264_PROCESSING_SRCS}
                     ${MW_OPENH264_ENCODER_SRCS})
list(TRANSFORM MW_OPENH264_SRCS PREPEND ${MW_OPENH264_DIR}/)
list(TRANSFORM MW_OPENH264_X86_ASM PREPEND ${MW_OPENH264_DIR}/)
list(TRANSFORM MW_OPENH264_ARM64_ASM PREPEND ${MW_OPENH264_DIR}/)

# ── Which kernels, on this target ────────────────────────────────────────────
set(MW_OPENH264_ASM "none")
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _mw_oh264_cpu)
if(_mw_oh264_cpu MATCHES "^(x86_64|amd64)$")
    find_program(MW_NASM nasm
        HINTS $ENV{MW_NASM} ${MW_NASM_DIR}
        PATHS "C:/Program Files/NASM" "$ENV{LOCALAPPDATA}/bin/NASM")
    if(NOT MW_NASM AND DEFINED ENV{VCPKG_ROOT})
        file(GLOB _mw_vcpkg_nasm "$ENV{VCPKG_ROOT}/downloads/tools/nasm/*/nasm.exe")
        if(_mw_vcpkg_nasm)
            list(GET _mw_vcpkg_nasm 0 MW_NASM)
        endif()
    endif()
    if(NOT MW_NASM)
        # The build tree's own vcpkg, when the environment does not name one.
        file(GLOB _mw_vcpkg_nasm "E:/vcpkg/downloads/tools/nasm/*/nasm.exe"
                                 "C:/vcpkg/downloads/tools/nasm/*/nasm.exe")
        if(_mw_vcpkg_nasm)
            list(GET _mw_vcpkg_nasm 0 MW_NASM)
        endif()
    endif()
    if(MW_NASM)
        set(CMAKE_ASM_NASM_COMPILER ${MW_NASM})
        enable_language(ASM_NASM)
        set(MW_OPENH264_ASM "x86-64 NASM (${MW_NASM})")
    endif()
elseif(_mw_oh264_cpu MATCHES "^(aarch64|arm64)$" AND NOT MSVC)
    enable_language(ASM)
    set(MW_OPENH264_ASM "AArch64 NEON (GAS)")
endif()

add_library(mw-openh264 STATIC ${MW_OPENH264_SRCS})
set_target_properties(mw-openh264 PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_include_directories(mw-openh264 SYSTEM PUBLIC ${MW_OPENH264_DIR}/codec/api/wels)
target_include_directories(mw-openh264 PRIVATE
    ${MW_OPENH264_DIR}/codec/common/inc
    ${MW_OPENH264_DIR}/codec/processing/interface
    ${MW_OPENH264_DIR}/codec/processing/src/common
    ${MW_OPENH264_DIR}/codec/encoder/core/inc
    ${MW_OPENH264_DIR}/codec/encoder/plus/inc)
target_compile_features(mw-openh264 PRIVATE cxx_std_11)
target_compile_definitions(mw-openh264 PRIVATE NDEBUG)

if(MW_OPENH264_ASM MATCHES "NASM")
    target_sources(mw-openh264 PRIVATE ${MW_OPENH264_X86_ASM})
    target_compile_definitions(mw-openh264 PRIVATE X86_ASM HAVE_AVX2)
    # NASM prepends -I paths literally, so the trailing slash is not optional.
    if(WIN32)
        set(CMAKE_ASM_NASM_OBJECT_FORMAT win64)
        set(_mw_nasm_platform -DWIN64)
    elseif(APPLE)
        set(CMAKE_ASM_NASM_OBJECT_FORMAT macho64)
        set(_mw_nasm_platform -DUNIX64 -DPREFIX)
    else()
        set(CMAKE_ASM_NASM_OBJECT_FORMAT elf64)
        set(_mw_nasm_platform -DUNIX64)
    endif()
    set_source_files_properties(${MW_OPENH264_X86_ASM} PROPERTIES LANGUAGE ASM_NASM
        COMPILE_OPTIONS "${_mw_nasm_platform};-DHAVE_AVX2;-I${MW_OPENH264_DIR}/codec/common/x86/")
elseif(MW_OPENH264_ASM MATCHES "AArch64")
    target_sources(mw-openh264 PRIVATE ${MW_OPENH264_ARM64_ASM})
    target_compile_definitions(mw-openh264 PRIVATE HAVE_NEON_AARCH64)
    target_include_directories(mw-openh264 PRIVATE ${MW_OPENH264_DIR}/codec/common/arm64)
endif()

# Compiler options are for the C++ compiler only: target_compile_options would
# otherwise hand `/W0` to NASM, which rejects it.
if(MSVC)
    target_compile_definitions(mw-openh264 PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(mw-openh264 PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W0;/EHsc>)
else()
    target_compile_options(mw-openh264 PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-w;-fno-strict-aliasing>)
    find_package(Threads REQUIRED)
    target_link_libraries(mw-openh264 PUBLIC Threads::Threads)
endif()

message(STATUS "mw-native-host: OpenH264 2.6.0 — kernels: ${MW_OPENH264_ASM}")
if(MW_OPENH264_ASM STREQUAL "none")
    message(WARNING "mw-native-host: OpenH264 built WITHOUT SIMD kernels — the CPU encoder will be "
                    "2–3× slower than it should be (x86-64: install NASM or set MW_NASM)")
endif()
