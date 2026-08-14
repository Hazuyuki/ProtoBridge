build_lib_example(
  NAME gpu-cluster-example
  SOURCE_FILES gpu-cluster-example.cc
  LIBRARIES_TO_LINK ${libgpu-cluster}
                  ${libpoint-to-point}
                  ${libnetwork}
                  ${libcore}
)

build_lib_example(
  NAME ring-topology-example
  SOURCE_FILES ring-topology-example.cc
  LIBRARIES_TO_LINK ${libgpu-cluster}
                  ${libpoint-to-point}
                  ${libnetwork}
                  ${libcore}
)

build_lib_example(
  NAME hybrid-cluster-example
  SOURCE_FILES hybrid-cluster-example.cc
  LIBRARIES_TO_LINK ${libgpu-cluster}
                  ${libpoint-to-point}
                  ${libnetwork}
                  ${libcore}
)
