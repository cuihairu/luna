[0m
> build · gpt-6-luna
[0m→ [0mSkill "architecture-review" 
> build · gpt-6-luna
[0m→ [0mSkill "code-review" 
> build · gpt-6-luna
[0m✱ [0mGlob "**/*" [90m500 matches[0m
[0m✱ [0mGrep "TODO|FIXME|XXX|HACK" [90min . · 300 matches[0m
[0m$ [0mgit status --short && git log -1 --oneline
850c1dd docs: drop duplicate nav github link
> build · gpt-6-luna
[0m→ [0mRead todo.md 
[0m→ [0mRead README.md 
[0m→ [0mRead README.zh.md 
[0m→ [0mRead CMakeLists.txt 
[0m$ [0mfind docs -type f -not -path '*/.git/*' | sort && find src lua/modules lua/luna test -maxdepth 2 -type f | sort
docs/architecture.en.md
docs/architecture.md
docs/.gitignore
docs/guide/cli-repl.md
docs/guide/examples.md
docs/guide/getting-started.md
docs/guide/introduction.md
docs/guide/kernel.md
docs/guide/loop.md
docs/guide/modules.md
docs/guide/plugins.md
docs/guide/rocks.md
docs/guide/sandbox.md
docs/guide/tasks.md
docs/index.en.md
docs/index.md
docs/loop-backend-design.en.md
docs/loop-backend-design.md
docs/node-parity.md
docs/other/build.md
docs/other/config.md
docs/other/errors.md
docs/other/faq.md
docs/package.json
docs/package-lock.json
docs/pnpm-lock.yaml
docs/pnpm-workspace.yaml
docs/public/logo.svg
docs/public/terminal.svg
docs/stdlib/crypto.md
docs/stdlib/csv.md
docs/stdlib/events.md
docs/stdlib/fs.md
docs/stdlib/http.md
docs/stdlib/index.md
docs/stdlib/ini.md
docs/stdlib/json.md
docs/stdlib/logging.md
docs/stdlib/net.md
docs/stdlib/path.md
docs/stdlib/stream.md
docs/stdlib/toml.md
docs/stdlib/util.md
docs/stdlib/xml.md
docs/stdlib/yaml.md
docs/stdlib/zlib.md
docs/.vitepress/cache/deps/_metadata.json
docs/.vitepress/cache/deps/package.json
docs/.vitepress/cache/deps/vitepress_n_mark__js_src_vanilla__js.js
docs/.vitepress/cache/deps/vitepress_n_mark__js_src_vanilla__js.js.map
docs/.vitepress/cache/deps/vitepress_n_minisearch.js
docs/.vitepress/cache/deps/vitepress_n_minisearch.js.map
docs/.vitepress/cache/deps/vitepress_n_@vue_devtools-api.js
docs/.vitepress/cache/deps/vitepress_n_@vue_devtools-api.js.map
docs/.vitepress/cache/deps/vitepress_n_@vueuse_core.js
docs/.vitepress/cache/deps/vitepress_n_@vueuse_core.js.map
docs/.vitepress/cache/deps/vitepress_n_@vueuse_integrations_useFocusTrap.js
docs/.vitepress/cache/deps/vitepress_n_@vueuse_integrations_useFocusTrap.js.map
docs/.vitepress/cache/deps/vue.js
docs/.vitepress/cache/deps/vue.runtime.esm-bundler-Bo_ScjpA.js
docs/.vitepress/cache/deps/vue.runtime.esm-bundler-Bo_ScjpA.js.map
docs/.vitepress/config.mts
docs/.vitepress/dist/404.html
docs/.vitepress/dist/architecture.html
docs/.vitepress/dist/assets/app.Bc-WPGbF.js
docs/.vitepress/dist/assets/architecture.md.Cro0Fo3G.js
docs/.vitepress/dist/assets/architecture.md.Cro0Fo3G.lean.js
docs/.vitepress/dist/assets/chunks/assets.k0JU5wSp.js
docs/.vitepress/dist/assets/chunks/framework.DXG-kv6m.js
docs/.vitepress/dist/assets/chunks/@localSearchIndexroot.B8bGXUpc.js
docs/.vitepress/dist/assets/chunks/metadata.c1d2499e.js
docs/.vitepress/dist/assets/chunks/theme.BjlToea6.js
docs/.vitepress/dist/assets/chunks/VPLocalSearchBox.-8ZGjDNx.js
docs/.vitepress/dist/assets/guide_cli-repl.md.gQ7W8CsM.js
docs/.vitepress/dist/assets/guide_cli-repl.md.gQ7W8CsM.lean.js
docs/.vitepress/dist/assets/guide_examples.md.DihN2HqE.js
docs/.vitepress/dist/assets/guide_examples.md.DihN2HqE.lean.js
docs/.vitepress/dist/assets/guide_getting-started.md._coq93BX.js
docs/.vitepress/dist/assets/guide_getting-started.md._coq93BX.lean.js
docs/.vitepress/dist/assets/guide_introduction.md.wb_0UjUM.js
docs/.vitepress/dist/assets/guide_introduction.md.wb_0UjUM.lean.js
docs/.vitepress/dist/assets/guide_kernel.md.CNvCDN_W.js
docs/.vitepress/dist/assets/guide_kernel.md.CNvCDN_W.lean.js
docs/.vitepress/dist/assets/guide_loop.md.DHUMI0a7.js
docs/.vitepress/dist/assets/guide_loop.md.DHUMI0a7.lean.js
docs/.vitepress/dist/assets/guide_modules.md.DlHdRcQm.js
docs/.vitepress/dist/assets/guide_modules.md.DlHdRcQm.lean.js
docs/.vitepress/dist/assets/guide_plugins.md.BK1FgJXp.js
docs/.vitepress/dist/assets/guide_plugins.md.BK1FgJXp.lean.js
docs/.vitepress/dist/assets/guide_rocks.md.BnH0mQIY.js
docs/.vitepress/dist/assets/guide_rocks.md.BnH0mQIY.lean.js
docs/.vitepress/dist/assets/guide_sandbox.md.Dy5b48LA.js
docs/.vitepress/dist/assets/guide_sandbox.md.Dy5b48LA.lean.js
docs/.vitepress/dist/assets/guide_tasks.md.wgzJ05vZ.js
docs/.vitepress/dist/assets/guide_tasks.md.wgzJ05vZ.lean.js
docs/.vitepress/dist/assets/index.md.BJUl4aKc.js
docs/.vitepress/dist/assets/index.md.BJUl4aKc.lean.js
docs/.vitepress/dist/assets/loop-backend-design.md.CgWNCQ0l.js
docs/.vitepress/dist/assets/loop-backend-design.md.CgWNCQ0l.lean.js
docs/.vitepress/dist/assets/node-parity.md.Bhmv3gqz.js
docs/.vitepress/dist/assets/node-parity.md.Bhmv3gqz.lean.js
docs/.vitepress/dist/assets/other_build.md.D8Y4Mk61.js
docs/.vitepress/dist/assets/other_build.md.D8Y4Mk61.lean.js
docs/.vitepress/dist/assets/other_config.md.B7SqW19Q.js
docs/.vitepress/dist/assets/other_config.md.B7SqW19Q.lean.js
docs/.vitepress/dist/assets/other_errors.md.B9qSee6O.js
docs/.vitepress/dist/assets/other_errors.md.B9qSee6O.lean.js
docs/.vitepress/dist/assets/other_faq.md.DmOrYxcG.js
docs/.vitepress/dist/assets/other_faq.md.DmOrYxcG.lean.js
docs/.vitepress/dist/assets/stdlib_crypto.md.DKTJvW_l.js
docs/.vitepress/dist/assets/stdlib_crypto.md.DKTJvW_l.lean.js
docs/.vitepress/dist/assets/stdlib_csv.md.KzzkeF88.js
docs/.vitepress/dist/assets/stdlib_csv.md.KzzkeF88.lean.js
docs/.vitepress/dist/assets/stdlib_events.md.DaIVfTEe.js
docs/.vitepress/dist/assets/stdlib_events.md.DaIVfTEe.lean.js
docs/.vitepress/dist/assets/stdlib_fs.md.D-TawP0w.js
docs/.vitepress/dist/assets/stdlib_fs.md.D-TawP0w.lean.js
docs/.vitepress/dist/assets/stdlib_http.md.ChTLwrwT.js
docs/.vitepress/dist/assets/stdlib_http.md.ChTLwrwT.lean.js
docs/.vitepress/dist/assets/stdlib_index.md.DOrHVI49.js
docs/.vitepress/dist/assets/stdlib_index.md.DOrHVI49.lean.js
docs/.vitepress/dist/assets/stdlib_ini.md.QFB6Zgga.js
docs/.vitepress/dist/assets/stdlib_ini.md.QFB6Zgga.lean.js
docs/.vitepress/dist/assets/stdlib_json.md.BBtpIQUa.js
docs/.vitepress/dist/assets/stdlib_json.md.BBtpIQUa.lean.js
docs/.vitepress/dist/assets/stdlib_logging.md.CLRhzkGM.js
docs/.vitepress/dist/assets/stdlib_logging.md.CLRhzkGM.lean.js
docs/.vitepress/dist/assets/stdlib_net.md.B908ewm2.js
docs/.vitepress/dist/assets/stdlib_net.md.B908ewm2.lean.js
docs/.vitepress/dist/assets/stdlib_path.md.Df-lTmHJ.js
docs/.vitepress/dist/assets/stdlib_path.md.Df-lTmHJ.lean.js
docs/.vitepress/dist/assets/stdlib_stream.md.DDJcQ0xS.js
docs/.vitepress/dist/assets/stdlib_stream.md.DDJcQ0xS.lean.js
docs/.vitepress/dist/assets/stdlib_toml.md.zi-6Lw_h.js
docs/.vitepress/dist/assets/stdlib_toml.md.zi-6Lw_h.lean.js
docs/.vitepress/dist/assets/stdlib_util.md.EDlx9pFo.js
docs/.vitepress/dist/assets/stdlib_util.md.EDlx9pFo.lean.js
docs/.vitepress/dist/assets/stdlib_xml.md.ByIWCMak.js
docs/.vitepress/dist/assets/stdlib_xml.md.ByIWCMak.lean.js
docs/.vitepress/dist/assets/stdlib_yaml.md.CXH-EJUx.js
docs/.vitepress/dist/assets/stdlib_yaml.md.CXH-EJUx.lean.js
docs/.vitepress/dist/assets/stdlib_zlib.md.DL2Yn1Ww.js
docs/.vitepress/dist/assets/stdlib_zlib.md.DL2Yn1Ww.lean.js
docs/.vitepress/dist/assets/style.DbxX6Dod.css
docs/.vitepress/dist/assets/vp-icons.139f4833.css
docs/.vitepress/dist/assets/审计-文档一致性.md.cJP3i4tZ.js
docs/.vitepress/dist/assets/审计-文档一致性.md.cJP3i4tZ.lean.js
docs/.vitepress/dist/guide/cli-repl.html
docs/.vitepress/dist/guide/examples.html
docs/.vitepress/dist/guide/getting-started.html
docs/.vitepress/dist/guide/introduction.html
docs/.vitepress/dist/guide/kernel.html
docs/.vitepress/dist/guide/loop.html
docs/.vitepress/dist/guide/modules.html
docs/.vitepress/dist/guide/plugins.html
docs/.vitepress/dist/guide/rocks.html
docs/.vitepress/dist/guide/sandbox.html
docs/.vitepress/dist/guide/tasks.html
docs/.vitepress/dist/hashmap.json
docs/.vitepress/dist/index.html
docs/.vitepress/dist/logo.svg
docs/.vitepress/dist/loop-backend-design.html
docs/.vitepress/dist/node-parity.html
docs/.vitepress/dist/other/build.html
docs/.vitepress/dist/other/config.html
docs/.vitepress/dist/other/errors.html
docs/.vitepress/dist/other/faq.html
docs/.vitepress/dist/stdlib/crypto.html
docs/.vitepress/dist/stdlib/csv.html
docs/.vitepress/dist/stdlib/events.html
docs/.vitepress/dist/stdlib/fs.html
docs/.vitepress/dist/stdlib/http.html
docs/.vitepress/dist/stdlib/index.html
docs/.vitepress/dist/stdlib/ini.html
docs/.vitepress/dist/stdlib/json.html
docs/.vitepress/dist/stdlib/logging.html
docs/.vitepress/dist/stdlib/net.html
docs/.vitepress/dist/stdlib/path.html
docs/.vitepress/dist/stdlib/stream.html
docs/.vitepress/dist/stdlib/toml.html
docs/.vitepress/dist/stdlib/util.html
docs/.vitepress/dist/stdlib/xml.html
docs/.vitepress/dist/stdlib/yaml.html
docs/.vitepress/dist/stdlib/zlib.html
docs/.vitepress/dist/terminal.svg
docs/.vitepress/dist/审计-文档一致性.html
docs/.vitepress/theme/custom.css
docs/.vitepress/theme/index.mts
docs/审计-文档一致性.en.md
docs/审计-文档一致性.md
lua/luna/complete.lua
lua/luna/highlight.lua
lua/luna/introspect.lua
lua/luna/magic.lua
lua/luna/modules.lua
lua/luna/new.lua
lua/luna/plugins.lua
lua/luna/repl.lua
lua/luna/rocks.lua
lua/luna/sandbox.lua
lua/luna/serve.lua
lua/modules/assert/init.lua
lua/modules/console/init.lua
lua/modules/crypto/init.lua
lua/modules/csv/init.lua
lua/modules/events/init.lua
lua/modules/fs/init.lua
lua/modules/http/init.lua
lua/modules/ini/init.lua
lua/modules/json/init.lua
lua/modules/loop/http.lua
lua/modules/net/init.lua
lua/modules/path/init.lua
lua/modules/process/init.lua
lua/modules/querystring/init.lua
lua/modules/stream/init.lua
lua/modules/task/init.lua
lua/modules/toml/init.lua
lua/modules/url/init.lua
lua/modules/util/init.lua
lua/modules/xml/init.lua
lua/modules/yaml/explicit.lua
lua/modules/yaml/functional.lua
lua/modules/yaml/implicit.lua
lua/modules/yaml/init.lua
lua/modules/zlib/init.lua
src/luna.c
src/luna.h
src/luna_hook.c
src/luna_hook.h
src/luna_kernel.c
src/luna_kernel.h
src/luna_line.c
src/luna_line.h
src/luna_loop.c
src/luna_loop.h
src/luna_main.c
src/luna_toml.c
test/CMakeLists.txt
test/fixtures/attach_target.lua
test/fixtures/bad.lua
test/fixtures/badpoll.lua
test/fixtures/busy.lua
test/fixtures/covsum_report.fixture
test/fixtures/drain_timer.lua
test/fixtures/drain_unref.lua
test/fixtures/drain_wait.lua
test/fixtures/hello.lua
test/fixtures/init_state.lua
test/fixtures/non_string_error.lua
test/fixtures/spin.lua
test/fixtures/tostring_message.lua
test/fixtures/tostring_returns_non_string.lua
test/luna_cov.h
test/luna_test_cli.c
test/luna_test_complete.c
test/luna_test_covsum.c
test/luna_test_highlight.c
test/luna_test_introspect.c
test/luna_test_line.c
test/luna_test_linedit.c
test/luna_test_loop.c
test/luna_test_magic.c
test/luna_test_main.c
test/luna_test_modules.c
test/luna_test_plugins.c
test/luna_test_repl.c
test/luna_test_rocks.c
test/luna_test_serve.c
test/replxx_completions_shim.cxx
test/test_main.c
> build · gpt-6-luna
[0m→ [0mRead lua/luna/sandbox.lua 
[0m→ [0mRead docs/审计-文档一致性.md 
[0m→ [0mRead docs/architecture.md 
[0m→ [0mRead docs/guide/sandbox.md 
[0m→ [0mRead docs/guide/loop.md 
[0m→ [0mRead docs/other/build.md 
> build · gpt-6-luna
[0m✱ [0mGrep "function M\.|M\.[A-Za-z_]+\s*=" [90min lua/modules/fs/init.lua · 0 matches[0m
[0m✱ [0mGrep "function M\.|max.*[Bb]ody|content%-length|body|function .*parse" [90min lua/modules/loop/http.lua · 71 matches[0m
[0m✱ [0mGrep "stdout|stderr|on_exit|uv_spawn|execSync|process_run|proc" [90min src/luna_loop.c · 156 matches[0m
[0m→ [0mRead docs/architecture.en.md 
[0m$ [0mfind .github/workflows -maxdepth 1 -type f -print -exec sed -n '1,240p' {} \;
.github/workflows/nightly.yml
name: Daily Build

# 每日构建流水线：滚动 nightly Release 是唯一交付面（舰队规范，同
# chirp/soar 的每日构建）。固定 tag `nightly` 的 Pre-release，每次构建把
# tag 重指本次 commit、清旧资产传新——全仓库只保留一份"最新每日构建"，
# 重跑幂等。Actions artifact 只是 run 页面的临时副本，不算交付。固定 tag
# 是分发通道，不对应版本：版本发布禁令仍然有效。公开仓库 Release 资产
# 匿名可下，一键安装无需任何凭据。
#
# 资产名即安装器的坐标：luna-nightly-<os>-<arch>.zip（+ .sha256 侧车，
# sha256sum 格式），zip 根平铺二进制 + luna_modules/ 模块侧车——二进制
# 的模块查找先是编译期目录（只在构建机上存在），再落可执行文件同级
# luna_modules/，产物不带侧车装到任何机器都会 module 'argparse' not
# found（2026-10-02 实测踩中）。install.ps1 / install.sh 按 OS+arch 组名
# 直拉 releases/download/nightly/<资产名>。
#
# 与 push 触发的 CMake CI 的差异只有三处：构建类型 Release（产物给人
# 运行）；关闭单元测试（夜间验证由 daily.yml 的全量测试矩阵负责，
# 2026-10-09 起四腿全转正；本管线不把 rocks 组的网络腿带进每日窗口）；
# 最后打包发布。
#
# Windows 腿保留 continue-on-error 属交付健壮性口径：item 4 已于
# 2026-10-05 解冻、Windows 构建连日绿，红不拦发布是免单腿抖动冻结
# 全平台交付（转正拉响在验证面 daily.yml）。zlib 走 vcpkg 静态三元组
# 是探针口径沿用（runner 预装 vcpkg，zlib 静态链入），源码 vendor 与
# 否是设计裁定，挂 todo 转正清单待决。平台面与 README/architecture 的
# 口径一致：luna 是单二进制（依赖全部静态链，系统侧只留 zlib 与
# OpenSSL 的动态库），Linux 用发行版包（runner 镜像自带 libssl-dev /
# zlib1g-dev），macOS 用 Homebrew 的 keg-only openssl@3。

on:
  schedule:
    # UTC 01:23 = 北京时间 09:23；错开 soar 每日构建的 00:40 UTC，
    # 也避开整点 runner 高峰
    - cron: '23 1 * * *'
  workflow_dispatch:

permissions:
  contents: read
  actions: write # collect job 删除历史 daily-build artifact 需要

concurrency:
  group: daily-build
  cancel-in-progress: true

jobs:
  build:
    name: Build (${{ matrix.name }})
    runs-on: ${{ matrix.os }}
    timeout-minutes: 60
    # Windows 腿红不拖垮夜间绿:交付健壮性(验证面 daily.yml 已转正拉响)
    continue-on-error: ${{ matrix.experimental }}

    strategy:
      fail-fast: false
      matrix:
        include:
          - os: ubuntu-latest
            name: linux-x86_64
            title: Linux x64
            experimental: false
          - os: ubuntu-24.04-arm
            name: linux-aarch64
            title: Linux arm64
            experimental: false
          - os: macos-latest
            name: macos-aarch64
            title: macOS (Apple Silicon)
            experimental: false
          # 交付面保留豁免:红不拦发布(验证面已转正,见头注)
          - os: windows-latest
            name: windows-x86_64
            title: Windows x64
            experimental: true

    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive

      - name: Install Ninja (Linux)
        if: runner.os == 'Linux'
        run: |
          sudo apt-get update
          sudo apt-get install -y ninja-build

      # pkg-config 顺手装上（FindOpenSSL 会拿它做提示）；
      # openssl@3 是 keg-only，下面 Configure 里显式指给 CMake。
      - name: Install Ninja & OpenSSL (macOS)
        if: runner.os == 'macOS'
        run: brew install ninja pkg-config openssl@3

      # Windows 依赖装配：runner 预装 vcpkg，静态三元组装 zlib 与 openssl
      # (zlib 走 vcpkg 静态三元组是设计裁定，未 vendor 源码；见 build.md)
      - name: Provision zlib + openssl (Windows)
        if: runner.os == 'Windows'
        run: vcpkg install zlib:x64-windows-static openssl:x64-windows-static

      - name: Configure
        if: runner.os != 'Windows'
        shell: bash
        run: |
          if [ "$RUNNER_OS" = "macOS" ]; then
            cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
              -DENABLE_UNIT_TESTS=OFF \
              -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
          else
            cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
              -DENABLE_UNIT_TESTS=OFF
          fi

      - name: Configure (Windows)
        if: runner.os == 'Windows'
        shell: bash
        run: |
          cmake -B build \
            -DCMAKE_BUILD_TYPE=Release -DENABLE_UNIT_TESTS=OFF \
            -DCMAKE_TOOLCHAIN_FILE="$VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake" \
            -DVCPKG_TARGET_TRIPLET=x64-windows-static

      - name: Build
        if: runner.os != 'Windows'
        run: cmake --build build

      - name: Build (Windows)
        if: runner.os == 'Windows'
        run: cmake --build build --config Release -j

      # 冒烟：单二进制自证。--help 出用法；-e 出 Out[1]: 42；loop 定时器
      # 不调 run 也触发——脚本尾部自动排水是 libuv + 排水全栈的端到端
      # 证明。看门狗两层：5s 的 os.exit(99) 必须 :unref()（ref'd 看门狗
      # 自己就是活句柄，会把排水段撑到自己响——挂起定时器留住进程正是
      # Node 语义）；外层 perl alarm 20 兜住循环彻底卡死不转的形态
      # （unref 定时器只在循环转身时响，卡死的循环它到不了）。
      - name: Smoke
        if: runner.os != 'Windows'
        shell: bash
        run: |
          set -e -o pipefail
          ./build/luna --help
          ./build/luna -e '6*7' | grep -q 'Out\[1\]: 42'
          perl -e 'alarm 20; exec @ARGV' \
            ./build/luna -e 'local l = require("loop") l.setTimeout(function() print("drain-ok") end, 50) l.setTimeout(function() os.exit(99) end, 5000):unref()' \
            | grep -q drain-ok

      - name: Smoke (Windows)
        if: runner.os == 'Windows'
        shell: bash
        run: ./build/Release/luna.exe --version

      # 资产坐标 zip：根平铺二进制 + luna_modules/ 侧车 + 平台说明，附
      # .sha256 侧车（sha256sum 格式，安装器装前比对）。可执行位由安装
      # 脚本 chmod 补，zip 是否带着都无所谓。
      - name: Package
        if: runner.os != 'Windows'
        shell: bash
        run: |
          set -e
          ASSET=luna-nightly-${{ matrix.name }}
          mkdir -p stage
          cp build/luna stage/luna
          cp -r build/luna_modules stage/luna_modules
          cat > stage/PLATFORM-NOTES.txt <<NOTES
          luna 每日构建 - ${{ matrix.title }}
          包含: luna 单二进制（Lua 5.5 / libuv / replxx / 全部格式后端已静态链入）
                + luna_modules/ 模块侧车（Lua 策略层，与二进制同目录解析，整包拷走即用）

          验证: 终端运行
            ./luna --help
            ./luna -e '6*7'          # 应答 Out[1]: 42
            ./luna                    # 进入交互式 REPL（^D 退出）
          NOTES
          if [ "$RUNNER_OS" = "macOS" ]; then
            cat >> stage/PLATFORM-NOTES.txt <<'NOTES'

          crypto 模块说明: 本包动态链接 Homebrew 的 OpenSSL（keg-only，不进
          默认路径）。目标机如未装，`brew install openssl@3` 后 crypto 与
          loop 的 TLS 面即可用；其余全部功能无此依赖。
          首次运行如被 Gatekeeper 拦截: xattr -cr luna（每日构建未做签名）。
          NOTES
          else
            cat >> stage/PLATFORM-NOTES.txt <<NOTES

          运行时依赖: libz 与 libssl/libcrypto（发行版默认自带；如缺按
          发行版装 openssl 即可，只为 crypto 与 loop 的 TLS 面）。
          NOTES
          fi
          (cd stage && zip -qr ../$ASSET.zip luna luna_modules PLATFORM-NOTES.txt)
          sha256sum "$ASSET.zip" > "$ASSET.zip.sha256"

      - name: Package (Windows)
        if: runner.os == 'Windows'
        shell: pwsh
        run: |
          $ErrorActionPreference = "Stop"
          $Asset = "luna-nightly-${{ matrix.name }}"
          New-Item -ItemType Directory -Force -Path stage | Out-Null
          Copy-Item build/Release/luna.exe stage/luna.exe
          Copy-Item build/luna_modules stage/luna_modules -Recurse
          @'
          luna 每日构建 - Windows x64
          包含: luna.exe 单二进制（Lua 5.5 / libuv / replxx / 全部格式后端已静态链入）
                + luna_modules\ 模块侧车（Lua 策略层，与二进制同目录解析，整包拷走即用）

          验证: 终端运行
            .\luna.exe --help
            .\luna.exe -e '6*7'      # 应答 Out[1]: 42
            .\luna.exe               # 进入交互式 REPL（^C 退出）
          '@ | Set-Content -Encoding UTF8 stage/PLATFORM-NOTES.txt
          Compress-Archive -Path stage/* -DestinationPath "$Asset.zip"
          "$((Get-FileHash "$Asset.zip" -Algorithm SHA256).Hash.ToLower())  $Asset.zip" |
            Set-Content "$Asset.zip.sha256"

      - name: Upload platform package
        uses: actions/upload-artifact@v4
        with:
          name: pkg-${{ matrix.name }}
          path: |
            luna-nightly-*.zip
            luna-nightly-*.zip.sha256
          retention-days: 3
          if-no-files-found: error

  # 汇总 + 发布：平台资产合成 SHA256SUMS 与 BUILD_INFO/VERIFY，先更新
  # 滚动 nightly Release（交付面），再把同一份东西留档 daily-build
  # artifact（run 页副本，不算交付——v4 产物不可变，上传前先删历史同名）。
  collect:
    name: Collect & publish nightly
    needs: build
    runs-on: ubuntu-latest
    timeout-minutes: 15
    permissions:
      contents: write # 滚动 nightly Release（建/移 tag/清旧传新）需要
      actions: write # 删除历史 daily-build artifact 需要
    steps:
      - name: Download platform packages
        uses: actions/download-artifact@v4
        with:
          pattern: pkg-*
          merge-multiple: true
.github/workflows/daily.yml
name: daily-build

# 每日构建:夜间定时 + 手动可触发。产物走 artifact(保留 14 天),
# 不打 tag、不发 release——发布是显式动作,不归机器管。
# 产物名即安装器的坐标:luna-nightly-<os>-<arch> 内含裸二进制,
# install.sh / install.ps1 按 OS+uname -m 组名下载。
# 双管线注记(2026-10-10 记录,2026-10-14 裁定):本工作流是四平台
# 测试矩阵(每腿全量 ctest,红构建/红测试即拉响),产物只是
# per-run 副本;nightly Release 交付面由 daily-build.yml(纯产物
# 管线,01:23 UTC)清旧传新。裁定:两条管线角色互补,不退役任一条
# (退役门禁腿丢红报警,退役交付腿丢唯一交付通道);角色分界与裁定
# 记录见 docs/other/build.md。上句「不发 release」指本工作流自身,
# 机器发布的唯一交付面在 daily-build.yml。
on:
  schedule:
    # 21:13 UTC ≈ 北京时间次日 05:13,错开整点高峰
    - cron: "13 21 * * *"
  workflow_dispatch:

env:
  BUILD_TYPE: Release

jobs:
  nightly:
    name: ${{ matrix.artifact }}
    runs-on: ${{ matrix.os }}
    # 全矩阵已转正(Windows 2026-10-08 12c90bd,macOS 2026-10-09):
    # 构建或测试红回归直接拉响夜间,不再有试运行豁免腿
    continue-on-error: ${{ matrix.experimental }}
    strategy:
      fail-fast: false
      matrix:
        include:
          - os: ubuntu-latest
            artifact: luna-nightly-linux-x86_64
            binary: luna
            experimental: false
          - os: ubuntu-24.04-arm
            artifact: luna-nightly-linux-aarch64
            binary: luna
            experimental: false
          # macOS 已转正(2026-10-09):运行期病根三项根因级修复——
          # serve 组的 unixstream SUN_LEN(c557baf)、fs.watch 首监视器
          # 对 FSEvents 流启动的竞态(d468676)、linedit 的 RLIMIT_NPROC
          # 注入搬 fork 子进程(c9987e7),run 37865418197 mac 16/16 实录
          - os: macos-latest
            artifact: luna-nightly-macos-aarch64
            binary: luna
            experimental: false
          - os: windows-latest
            artifact: luna-nightly-windows-x86_64
            binary: luna.exe
            experimental: false

    steps:
      - uses: actions/checkout@v4

      - name: Configure
        if: runner.os != 'Windows'
        run: cmake -B build -DCMAKE_BUILD_TYPE=$BUILD_TYPE

      # zlib 与 openssl 走 runner 预装 vcpkg 的静态三元组——正式口径
      # (源码 vendor 与否)是设计裁定,待决前维持 vcpkg(见 todo 转正清单)
      - name: Provision zlib + openssl (Windows)
        if: runner.os == 'Windows'
        run: vcpkg install zlib:x64-windows-static openssl:x64-windows-static

      - name: Configure (Windows)
        if: runner.os == 'Windows'
        run: cmake -B build -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static

      - name: Build
        if: runner.os != 'Windows'
        run: cmake --build build -j"$(nproc)"

      - name: Build (Windows)
        if: runner.os == 'Windows'
        run: cmake --build build --config Release -j

      # macOS 曾以试运行带产物(测试红不拦 Stage);转正后测试红
      # 与构建红同权拉响
      - name: Test
        if: runner.os != 'Windows'
        working-directory: build
        continue-on-error: ${{ matrix.experimental }}
        run: ctest -C $BUILD_TYPE --output-on-failure

      - name: Smoke
        if: runner.os != 'Windows'
        run: ./build/luna --version

      - name: Smoke (Windows)
        if: runner.os == 'Windows'
        run: ./build/Release/luna.exe --version

      # 二进制 + luna_modules 侧车树进 artifact(zip 一层,装机侧只解
      # zip):二进制的模块查找先是编译期目录(只在构建机上存在),再落
      # 可执行文件同级 luna_modules/——产物不带侧车,装到任何机器都会
      # module 'argparse' not found(2026-10-02 实测踩中)。可执行位由
      # 安装脚本 chmod 补,zip 是否带着都无所谓
      - name: Stage
        shell: bash
        run: |
          mkdir stage
          if [ "$RUNNER_OS" = "Windows" ]; then
            cp build/Release/luna.exe stage/
          else
            cp build/luna stage/
          fi
          cp -r build/luna_modules stage/luna_modules

      - uses: actions/upload-artifact@v4
        with:
          name: ${{ matrix.artifact }}
          path: stage/
          retention-days: 14
.github/workflows/docs.yml
name: docs

on:
  push:
    branches: [main]
    paths:
      - 'docs/**'
      - '.github/workflows/docs.yml'
  workflow_dispatch:

permissions:
  contents: read
  pages: write
  id-token: write

concurrency:
  group: pages
  cancel-in-progress: true

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v7
      - uses: actions/setup-node@v7
        with:
          node-version: 24
      - uses: pnpm/action-setup@v4
        with:
          version: 10
      - name: install
        run: pnpm --dir docs install --frozen-lockfile
      - name: build
        run: pnpm --dir docs run docs:build
      - uses: actions/configure-pages@v6
      - uses: actions/upload-pages-artifact@v5
        with:
          path: docs/.vitepress/dist

  deploy:
    needs: build
    runs-on: ubuntu-latest
    environment:
      name: github-pages
      url: ${{ steps.deployment.outputs.page_url }}
    steps:
      - id: deployment
        uses: actions/deploy-pages@v5
.github/workflows/cmake.yml
name: CMake

on:
  push:
    branches: [ "main" ]
    paths-ignore:
      - '**.md'
      - 'docs/**'
      - 'LICENSE'
      - '.gitignore'
      - '*.svg'
      - '.editorconfig'
  pull_request:
    branches: [ "main" ]
    paths-ignore:
      - '**.md'
      - 'docs/**'
      - 'LICENSE'
      - '.gitignore'
      - '*.svg'
      - '.editorconfig'

env:
  # Customize the CMake build type here (Release, Debug, RelWithDebInfo, etc.)
  BUILD_TYPE: Release

jobs:
  build:
    strategy:
      fail-fast: false
      matrix:
        include:
          - os: ubuntu-latest
          - os: windows-latest
    runs-on: ${{ matrix.os }}

    steps:
    - uses: actions/checkout@v3

    - name: Install zlib + openssl (vcpkg, static)
      if: runner.os == 'Windows'
      # lua-zlib needs zlib headers and the TLS layer needs OpenSSL; the
      # nightly Windows stage uses the same static triplet
      run: vcpkg install zlib:x64-windows-static openssl:x64-windows-static

    - name: Configure CMake
      # Single-config generators (Visual Studio) need -C on build/ctest;
      # the vcpkg toolchain only exists on the Windows leg.
      if: runner.os == 'Windows'
      run: cmake -B ${{github.workspace}}/build -DCMAKE_BUILD_TYPE=${{env.BUILD_TYPE}} "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static

    - name: Configure CMake
      if: runner.os != 'Windows'
      run: cmake -B ${{github.workspace}}/build -DCMAKE_BUILD_TYPE=${{env.BUILD_TYPE}}

    - name: Build
      # Build your program with the given configuration
      run: cmake --build ${{github.workspace}}/build --config ${{env.BUILD_TYPE}}

    - name: Test
      working-directory: ${{github.workspace}}/build
      # POSIX-harness groups (cli/rocks/covsum/line/linedit/serve)
      # compile to empty suites on Windows and pass vacuously; the loop
      # group runs its Windows smoke suite, the rest run in full.
      run: ctest -C ${{env.BUILD_TYPE}} --output-on-failure
> build · gpt-6-luna
[0m→ [0mRead lua/modules/fs/init.lua 
> build · gpt-6-luna
[0m✱ [0mGrep "sandbox|register_c_modules|lfs|luaopen_lfs" [90min src/luna_main.c · 5 matches[0m
[0m→ [0mRead src/luna_loop.c [90m[offset=4110, limit=250][0m
[0m✱ [0mGrep "luaopen_lfs|lfs" [90min src · 2 matches[0m
[0m→ [0mRead README.zh.md [90m[offset=68, limit=12][0m
[0m→ [0mRead lua/modules/loop/http.lua [90m[offset=270, limit=220][0m
[0m✱ [0mGrep "function lfs\.|mkdir|rmdir|link|symlink|touch|lock" [90min deps/luafilesystem/src · 97 matches[0m
> build · gpt-6-luna
[0m→ [0mRead src/luna_main.c [90m[offset=195, limit=40][0m
[0m→ [0mRead src/luna_main.c [90m[offset=378, limit=65][0m
[0m✱ [0mGrep "sandbox.install|--sandbox|sandbox" [90min lua/luna.lua · 5 matches[0m
[0m✱ [0mGrep "lfs|LUNA_SANDBOX_MODULES|writeFileSync|mkdirSync|link" [90min test · 99 matches[0m
[0m✱ [0mGrep "function M\.|M\.[A-Za-z_]+" [90min lua/modules/loop/http.lua · 3 matches[0m
> build · gpt-6-luna
[0m→ [0mRead lua/modules/loop/http.lua [90m[offset=540, limit=135][0m
> build · gpt-6-luna
[0m→ [0mRead deps/luafilesystem/src/lfs.c [90m[offset=1138, limit=22][0m
[0m→ [0mRead deps/libuv/include/uv/version.h 
[0m→ [0mRead lua/luna.lua [90m[offset=120, limit=25][0m
[0m→ [0mRead docs/loop-backend-design.md [90m[limit=100][0m
[0m$ [0mgit submodule status deps/libuv deps/luafilesystem deps/lua
840404ce8ba7cc0204be52389a6cfff9f2c90fb6 deps/libuv (v1.53.0)
 7579fc9d7ed90240487251dfb69168f8e64e9294 deps/lua (v5.4-beta-676-g7579fc9d)
 a186cca5833691e830ed255e38ace8ff6b870dbf deps/luafilesystem (v1_9_0)
[0m✱ [0mGrep "1\.44\.2|1\.53\.0|6\.2\.8|6\.2\.9|26 submodules|试运行豁免|继续.*豁免|Windows.*未|Windows.*不在|C 编译器|C compiler" [90min . · 46 matches[0m
> build · gpt-6-luna
[0m→ [0mRead src/luna_main.c [90m[offset=250, limit=50][0m
> build · gpt-6-luna
[0m✱ [0mGrep ""_openssl\." [90min src/luna_main.c · 26 matches[0m
> build · gpt-6-luna
[0m→ [0mRead docs/guide/tasks.md [90m[limit=170][0m
> build · gpt-6-luna
[0m→ [0mRead src/luna_loop.c [90m[offset=4637, limit=55][0m
> build · gpt-6-luna
[0m✱ [0mGrep "POSIX-harness groups|compile to empty suites|Windows 源码构建同面支持|Windows 项构建|windows-latest" [90min .github/workflows/cmake.yml · 3 matches[0m
[0m✱ [0mGrep "loop 的四面|四面\(timer/fs/net/signal\)|九个面" [90min docs/loop-backend-design.md · 2 matches[0m
[0m✱ [0mGrep "CMake ≥ 3.14|project\(luna LANGUAGES C CXX\)|find_package\(ZLIB REQUIRED\)" [90min . · 5 matches[0m
> build · gpt-6-luna
[0m✱ [0mGrep "experimental: false|continue-on-error|macOS 已转正|windows-latest" [90min .github/workflows/daily.yml · 8 matches[0m
[0m✱ [0mGrep "Windows 腿红|experimental: true|continue-on-error" [90min .github/workflows/nightly.yml · 4 matches[0m
[0m✱ [0mGrep "试运行豁免|运行期测试按平台移植清单收敛中|Windows 项构建与产物" [90min README.zh.md · 1 match[0m
> build · gpt-6-luna
## 1）最薄弱的 3 处（按风险排序）

1. **P0 — 沙箱可绕过，不能按当前文档承诺保护不可信 Lua。** `src/luna_main.c:399-402` 在沙箱安装前加载 `loop`，`lua/luna/sandbox.lua:69-76` 又把所有已加载模块加入白名单；因此脚本可经 `require("loop").process.execSync(...)` 调用子进程（`src/luna_loop.c:4641-4651`）。同理，预加载的 `lfs` 可直接调用写操作（`src/luna_main.c:205,258-260`；`deps/luafilesystem/src/lfs.c:1146-1154`）。这与“无子进程、无文件写”的说明冲突（`docs/guide/sandbox.md:58-75`），属于安全边界失效，不只是缺少 OS 级隔离。

2. **P1 — `loop.http` 服务端对请求体没有大小上限或读取超时。** `lua/modules/loop/http.lua:609-620` 持续把输入拼进 `buf`；`645-654` 直接接受 `Content-Length` 并等完整请求体到达才调用 handler。头部虽有限制，body 没有；慢速或超大请求能长期占连接并耗尽内存。

3. **P1 — 子进程输出捕获无上限，内存不足时静默丢数据。** `src/luna_loop.c:4211-4231` 用 `malloc/realloc` 持续扩容 stdout/stderr 缓冲，无大小上限；`realloc` 失败后丢弃数据，最后仍可能在 `4283-4287` 以成功结果交付不完整输出。高输出量命令既可能耗尽进程内存，也可能让调用者误信结果完整。

## 2）最值得补的 3 项新能力（及当前为何没有）

1. **真正隔离的不可信代码运行器。** 在独立进程中运行沙箱，并由 OS 限制文件、网络、子进程和资源，而非只改同一个 Lua state。当前文档明确选择“同一个 Lua 态、不是第二台 VM”，并把 OS 隔离划为边界（`docs/guide/sandbox.md:111-117`）；但现有预加载模块绕过使这个边界比文档描述更弱。

2. **带背压与限额的 HTTP 请求体流式处理。** 支持 chunked request、边收边交给 handler、连接/请求超时和最大 body 限制。当前 `loop.http` 明确是一连接一请求、请求体完整落地后才调 handler，且不支持 chunked request（`lua/modules/loop/http.lua:542-551,645-654`），这是简化协议面换来的实现成本控制。

3. **统一的异步取消与 deadline。** 为网络、文件、子进程等操作提供一致的取消句柄和超时语义，并让 task 取消能传递到底层操作。现在 `task.cancel()` 只对挂起任务协作生效（`docs/guide/tasks.md:92-106`）；loop API 主要依赖各句柄自行 `close`/`clear`，没有统一取消契约（`docs/guide/loop.md:19-31`）。目前各 C 句柄生命周期独立，缺少统一语义，解释了为何尚未形成这项能力。

## 3）文档与实现可能对不上的地方

- **中文 README 的平台状态过时：** `README.zh.md:72` 仍称 macOS 测试在迁移、Windows 仍是“试运行豁免”；但测试矩阵四个平台 `experimental: false`（`.github/workflows/daily.yml:42-53`）。交付 workflow 的 Windows `continue-on-error` 仍在，但它被定义为交付健壮性，不等于测试腿仍处于试运行（`.github/workflows/nightly.yml:22,51-52,74`）。
- **构建依赖说明不完整：** `README.md:73`、`README.zh.md:73`、`docs/guide/getting-started.md:31` 和 `docs/other/build.md:5` 说只需 CMake 和 C 编译器；实现要求 C 与 C++（`CMakeLists.txt:2`），还要求 `find_package(ZLIB REQUIRED)`（`:86`）。`deps/` 并未消除 zlib 系统依赖。
- **事件循环设计文档内部口径过时：** `docs/loop-backend-design.md:50` 列出九个面，`:75` 又称“四面（timer/fs/net/signal）”；后者与前文及当前实现不一致。
- **Windows 测试覆盖容易被“全量 ctest”高估：** `docs/other/build.md:37` 描述 Windows 跑全量 ctest，但 CI 注释说明 `cli/rocks/covsum/line/linedit/serve` 等 POSIX harness 在 Windows 编译为空测试并 vacuously pass（`.github/workflows/cmake.yml:62-64`）。命令确实跑完，不代表这些组在 Windows 上得到验证。
