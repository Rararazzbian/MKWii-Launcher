# Opus, for lobby voice chat.
#
# Upstream ships its own CMake build, so this only turns off the parts Dolphin
# has no use for and hands the target on under the namespaced name the rest of
# the tree expects.
#
# EXCLUDE_FROM_ALL keeps opus's install() rules out of Dolphin's install stage
# without having to pick through them; the library is still built, because core
# links it explicitly.

set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)

add_subdirectory(opus EXCLUDE_FROM_ALL)

# Upstream already defines the Opus::opus alias, so only the warning silencing
# is left to do here - opus is not Dolphin's code and its warnings are noise in
# Dolphin's build log.
dolphin_disable_warnings(opus)
