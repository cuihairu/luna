include(FetchContent)

# 上游 git.cryptomilk.org 间歇不可达(CI run 36800109762 clone 三连败),
# 换官方 GitLab 镜像;tag 与上游逐字同一对象(cmocka-1.1.5 = 注释 tag
# 56eb3a18 → commit f5e2cd77,两源 ls-remote 核对一致)。tag 是注释 tag,
# GIT_SHALLOW 按拉取不按 commit 钉(shallow + hash 需服务端
# allowAnySHA1InWant,镜像不保证),钉版凭 tag 与此处注记。
FetchContent_Declare(
        cmocka
        GIT_REPOSITORY https://gitlab.com/cmocka/cmocka.git
        GIT_TAG        cmocka-1.1.5
        GIT_SHALLOW    1
)

set(WITH_STATIC_LIB ON CACHE BOOL "CMocka: Build with a static library" FORCE)
set(WITH_CMOCKERY_SUPPORT OFF CACHE BOOL "CMocka: Install a cmockery header" FORCE)
set(WITH_EXAMPLES OFF CACHE BOOL "CMocka: Build examples" FORCE)
set(UNIT_TESTING OFF CACHE BOOL "CMocka: Build with unit testing" FORCE)
set(PICKY_DEVELOPER OFF CACHE BOOL "CMocka: Build with picky developer flags" FORCE)

FetchContent_MakeAvailable(cmocka)