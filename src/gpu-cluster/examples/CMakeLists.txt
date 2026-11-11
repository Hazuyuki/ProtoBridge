build_lib_example(
  NAME protocol-profile-demo
  SOURCE_FILES protocol-profile-demo.cc
  LIBRARIES_TO_LINK ${libgpu-cluster}
)

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

build_lib_example(
  NAME custom-network-topology
  SOURCE_FILES custom-network-topology.cc
  LIBRARIES_TO_LINK ${libgpu-cluster}
                  ${libpoint-to-point}
                  ${libnetwork}
                  ${libcore}
)

build_lib_example(
  NAME custom-pcie-topology
  SOURCE_FILES custom-pcie-topology.cc
  LIBRARIES_TO_LINK ${libgpu-cluster}
                  ${libcore}
)
