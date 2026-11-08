cmake_minimum_required(VERSION 3.7...3.28)
project(putty LANGUAGES C)

set(CMAKE_C_STANDARD 99)

include(cmake/setup.cmake)

# Scan the docs directory first, so that when we start calling
# installed_program(), we'll know if we have man pages available
add_subdirectory(doc)

add_compile_definitions(HAVE_CMAKE_H)

include_directories(terminal)

add_library(utils STATIC
  ${GENERATED_COMMIT_C})
add_dependencies(utils cmake_commit_c)
add_subdirectory(utils)
add_subdirectory(stubs)

add_library(logging OBJECT
  logging.c utils/logeventf.c)

add_library(eventloop STATIC
  callback.c timing.c)

add_library(console STATIC
  clicons.c console.c)

add_library(settings STATIC
  cmdline.c settings.c)

add_library(crypto STATIC
  proxy/cproxy.c proxy/sshproxy.c)
add_subdirectory(crypto)

add_library(network STATIC
  errsock.c x11disp.c
  $<TARGET_OBJECTS:logging>
  proxy/proxy.c
  proxy/http.c
  proxy/socks4.c
  proxy/socks5.c
  proxy/newmain.c
  proxy/local.c
  proxy/interactor.c)

add_library(keygen STATIC
  import.c)
add_subdirectory(keygen)

add_library(agent STATIC
  sshpubk.c pageant.c aqsync.c)

add_library(guiterminal STATIC
  terminal/terminal.c terminal/bidi.c
  ldisc.c terminal/lineedit.c config.c dialog.c
  $<TARGET_OBJECTS:logging>)

add_library(noterminal STATIC
  stubs/no-term.c ldisc.c)

add_library(all-backends OBJECT
  pinger.c)

add_library(sftpclient STATIC
  psftpcommon.c)
add_subdirectory(ssh)

add_library(otherbackends STATIC
  $<TARGET_OBJECTS:all-backends>
  $<TARGET_OBJECTS:logging>)
add_subdirectory(otherbackends)

add_executable(testcrypt
  test/testcrypt.c sshpubk.c ssh/crc-attack-detector.c)
target_link_libraries(testcrypt
  keygen crypto utils ${platform_libraries})

add_executable(test_bpp
  test/test_bpp.c
  test/test_random.c
  callback.c
  stubs/no-logging.c
  stubs/no-network.c
  stubs/no-agent.c
  stubs/no-ldisc.c
  stubs/no-timing.c
  stubs/no-storage.c
  stubs/no-ca-config.c
  proxy/noproxy.c)
target_link_libraries(test_bpp
  sshclient crypto network utils ${platform_libraries})

add_executable(test_host_strfoo
  utils/host_strchr_internal.c)
target_compile_definitions(test_host_strfoo PRIVATE TEST)
target_link_libraries(test_host_strfoo utils ${platform_libraries})

add_executable(test_decode_utf8
  utils/decode_utf8.c)
target_compile_definitions(test_decode_utf8 PRIVATE TEST)
target_link_libraries(test_decode_utf8 utils ${platform_libraries})

add_executable(test_unicode_norm
  utils/unicode-norm.c)
target_compile_definitions(test_unicode_norm PRIVATE TEST)
target_link_libraries(test_unicode_norm utils ${platform_libraries})

add_executable(test_tree234
  utils/tree234.c)
target_compile_definitions(test_tree234 PRIVATE TEST)
target_link_libraries(test_tree234 utils ${platform_libraries})

add_executable(test_wildcard
  utils/wildcard.c)
target_compile_definitions(test_wildcard PRIVATE TEST)
target_link_libraries(test_wildcard utils ${platform_libraries})

add_executable(test_cluster_groups
  test/test_cluster_groups.c
  cluster-groups.c)
target_link_libraries(test_cluster_groups utils ${platform_libraries})

add_executable(test_cluster_layout
  test/test_cluster_layout.c
  cluster-layout.c)
target_link_libraries(test_cluster_layout utils ${platform_libraries})

add_executable(test_cert_expr
  utils/cert-expr.c)
target_compile_definitions(test_cert_expr PRIVATE TEST)
target_link_libraries(test_cert_expr utils ${platform_libraries})

add_executable(bidi_gettype
  terminal/bidi_gettype.c)
target_link_libraries(bidi_gettype guiterminal utils ${platform_libraries})

add_executable(bidi_test
  terminal/bidi_test.c)
target_link_libraries(bidi_test guiterminal utils ${platform_libraries})

add_executable(plink
  ${platform}/plink.c
  stubs/no-lineedit.c)
be_list(plink Plink SSH)
target_link_libraries(plink
  eventloop noterminal console sshclient otherbackends settings network crypto
  utils
  ${platform_libraries})
installed_program(plink)

add_executable(pscp
  pscp.c)
be_list(pscp PSCP SSH)
target_link_libraries(pscp
  sftpclient eventloop console sshclient settings network crypto utils
  ${platform_libraries})
installed_program(pscp)

add_executable(psftp
  psftp.c)
be_list(psftp PSFTP SSH)
target_link_libraries(psftp
  sftpclient eventloop console sshclient settings network crypto utils
  ${platform_libraries})
installed_program(psftp)

add_executable(psocks
  ${platform}/psocks.c
  psocks.c
  stubs/no-rand.c
  proxy/nocproxy.c
  proxy/nosshproxy.c
  ssh/portfwd.c)
target_link_libraries(psocks
  eventloop console network utils
  ${platform_libraries})

add_executable(test_conf
  test/test_conf.c
  stubs/no-agent.c
  stubs/no-callback.c
  stubs/no-gss.c
  stubs/no-ldisc.c
  stubs/no-network.c
  stubs/no-timing.c
  proxy/noproxy.c # FIXME: move this to stubs
)
be_list(test_conf TestConf SSH)
target_link_libraries(test_conf sshclient otherbackends settings network crypto utils ${platform_libraries})

foreach(subdir ${platform} ${extra_dirs})
  add_subdirectory(${subdir})
endforeach()

# Nasty bodge: we'd like to run this command inside unix/CMakeLists,
# adding the 'charset' library to everything that links with utils.
# But that wasn't allowed until cmake 3.13 (see cmake policy CMP0073),
# and we still have a min cmake version less than that. So we do it
# here instead.
if(platform STREQUAL unix)
  target_link_libraries(utils charset)
endif()

configure_file(cmake/cmake.h.in ${GENERATED_SOURCES_DIR}/cmake.h)

# Crypto code is built the way upstream ships it (-O2, no LTO; see
# putty_crypto_build_settings in cmake/setup.cmake). The same applies
# to the constant-time comparison and memory wiping helpers in utils,
# which LTO could otherwise inline into callers. Everything else keeps
# -O3 and LTO.
putty_crypto_build_settings(crypto)
if(putty_crypto_opt)
  set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/utils/smemeq.c
    ${CMAKE_SOURCE_DIR}/utils/smemclr.c
    PROPERTIES COMPILE_OPTIONS "${putty_crypto_opt};-fno-lto")
endif()

# Link-time optimisation would inline sk_addrcopy (unix/network.c) into
# the SOCKS 5 proxy code, where GCC can't see that the caller has
# already checked the address type, and so warns of a 16-byte write
# into a 4-byte buffer. That can't happen, so build network.c without
# LTO to avoid the false alarm. None of its code is speed-critical.
if(platform STREQUAL unix AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(${CMAKE_SOURCE_DIR}/unix/network.c
    PROPERTIES COMPILE_OPTIONS "-fno-lto")
endif()
