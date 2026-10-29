idf_component_register(SRCS "host_stub.c"
                       INCLUDE_DIRS "../../../external/linux/include"
                       REQUIRES ulp)

set(ulp_app_name s31_lp_main)
set(ulp_lp_core_sources "lp_core/main.c")
set(ulp_exp_dep_srcs "host_stub.c")
ulp_embed_binary(${ulp_app_name} "${ulp_lp_core_sources}" "${ulp_exp_dep_srcs}")
