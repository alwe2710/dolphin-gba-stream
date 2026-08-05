add_dolphin_test(MMIOTest MMIOTest.cpp)
add_dolphin_test(PageFaultTest PageFaultTest.cpp)
add_dolphin_test(CoreTimingTest CoreTimingTest.cpp)
add_dolphin_test(PatchAllowlistTest PatchAllowlistTest.cpp)
add_dolphin_test(GBAStreamHandshakeTest GBAStreamHandshakeTest.cpp)

# Real WS+app-handshake client (see the test file's own top comment) goes
# through unison_core directly rather than duplicating GBAStreamHandshake.cpp's
# own picojson-based logic -- unlike GBAStreamHandshakeTest above (which
# only needs picojson, already linked into `core`), this needs unison_core's
# websocket.c/handshake.c/json.c compiled in too.
set(UNISON_CORE_DIR ${CMAKE_SOURCE_DIR}/Externals/unison/core)
add_dolphin_test(GBAStreamMultiHostTest
  GBAStreamMultiHostTest.cpp
  ${UNISON_CORE_DIR}/src/websocket.c
  ${UNISON_CORE_DIR}/src/handshake.c
  ${UNISON_CORE_DIR}/src/json.c
  ${UNISON_CORE_DIR}/third_party/teeny-sha1/teeny-sha1.c
)
target_include_directories(GBAStreamMultiHostTest PRIVATE
  ${UNISON_CORE_DIR}/include
  ${UNISON_CORE_DIR}/third_party/teeny-sha1
)

add_dolphin_test(DSPAcceleratorTest DSP/DSPAcceleratorTest.cpp)
add_dolphin_test(DSPAssemblyTest
  DSP/DSPAssemblyTest.cpp
  DSP/DSPTestBinary.cpp
  DSP/DSPTestText.cpp
  DSP/HermesBinary.cpp
  DSP/HermesText.cpp
)

add_dolphin_test(ESFormatsTest IOS/ES/FormatsTest.cpp)

add_dolphin_test(FileSystemTest IOS/FS/FileSystemTest.cpp)

add_dolphin_test(SkylandersTest IOS/USB/SkylandersTest.cpp)

if(_M_X86_64)
  add_dolphin_test(PowerPCTest
    PowerPC/DivUtilsTest.cpp
    PowerPC/PageTableHostMappingTest.cpp
    PowerPC/Jit64Common/ConvertDoubleToSingle.cpp
    PowerPC/Jit64Common/Fres.cpp
    PowerPC/Jit64Common/Frsqrte.cpp
  )
elseif(_M_ARM_64)
  add_dolphin_test(PowerPCTest
    PowerPC/DivUtilsTest.cpp
    PowerPC/PageTableHostMappingTest.cpp
    PowerPC/JitArm64/ConvertSingleDouble.cpp
    PowerPC/JitArm64/FPRF.cpp
    PowerPC/JitArm64/Fres.cpp
    PowerPC/JitArm64/Frsqrte.cpp
    PowerPC/JitArm64/MovI2R.cpp
  )
else()
  add_dolphin_test(PowerPCTest
    PowerPC/DivUtilsTest.cpp
    PowerPC/PageTableHostMappingTest.cpp
  )
endif()

target_sources(PowerPCTest PRIVATE
  PowerPC/TestValues.h
  StubJit.h
)
