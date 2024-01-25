#Allow overriding XTensor versions
if (NOT XTL_GIT_TAG)
  set(XTL_GIT_TAG 0.7.4)
endif()
if (NOT XSIMD_GIT_TAG)
  set(XSIMD_GIT_TAG 8.1.0)
endif()
if (NOT XTENSOR_GIT_TAG)
  set(XTENSOR_GIT_TAG 0.24.2)
endif()

include(FetchContent)

FetchContent_Declare(
  xtl
  GIT_REPOSITORY https://github.com/xtensor-stack/xtl.git
  GIT_SHALLOW TRUE
  GIT_TAG ${XTL_GIT_TAG})
FetchContent_Declare(
  xsimd
  GIT_REPOSITORY https://github.com/xtensor-stack/xsimd.git
  GIT_SHALLOW TRUE
  GIT_TAG ${XSMID_GIT_TAG})
FetchContent_Declare(
  xtensor
  GIT_REPOSITORY https://github.com/xtensor-stack/xtensor.git
  GIT_SHALLOW TRUE
  GIT_TAG ${XTENSOR_GIT_TAG})

# Ensure XTensor headers are included as system headers.
foreach(LIB xtl;xsimd;xtensor)
  FetchContent_MakeAvailable(${LIB})
  get_target_property(IID ${LIB} INTERFACE_INCLUDE_DIRECTORIES)
  set_target_properties(${LIB} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${IID}")
endforeach()