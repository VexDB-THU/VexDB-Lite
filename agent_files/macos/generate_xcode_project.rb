#!/usr/bin/env ruby
# frozen_string_literal: true

# 生成可提交的 Xcode 工程。工程本身不依赖这个脚本运行；脚本用于文件增减后重建。
require 'xcodeproj'

root = File.expand_path(__dir__)
project_path = File.join(root, 'VexFS.xcodeproj')
project = Xcodeproj::Project.new(project_path)
project.root_object.attributes['LastSwiftUpdateCheck'] = '2630'
project.root_object.attributes['LastUpgradeCheck'] = '2630'

app_group = project.main_group.new_group('VexFSApp', 'VexFSApp')
extension_group = project.main_group.new_group('VexFSAppEx', 'VexFSAppEx')

app = project.new_target(:application, 'VexFSApp', :osx, '26.0')
extension = project.new_target(:app_extension, 'VexFSAppEx', :osx, '26.0')
extension.product_type = 'com.apple.product-type.extensionkit-extension'
extension.product_reference.explicit_file_type = 'wrapper.extensionkit-extension'

%w[VexFSApp.swift ContentView.swift].each do |name|
  app.source_build_phase.add_file_reference(app_group.new_file(name))
end
app_group.new_file('VexFSApp.entitlements')

%w[
  VexFSAppEx.swift
  VexFSBackend.swift
  VexFSFileSystem.swift
  VexFSItem.swift
  VexFSVolume.swift
  VexFSVolume+Operations.swift
].each do |name|
  extension.source_build_phase.add_file_reference(extension_group.new_file(name))
end
extension_group.new_file('VexFS-Bridging-Header.h')
extension_group.new_file('VexFSAppEx.entitlements')
extension_group.new_file('Info.plist')

fskit = project.frameworks_group.new_file('System/Library/Frameworks/FSKit.framework')
fskit.source_tree = 'SDKROOT'
extension.frameworks_build_phase.add_file_reference(fskit)

build_core = extension.new_shell_script_build_phase('Build VexFS SQLite Core')
build_core.always_out_of_date = '1'
build_core.shell_script = <<~'SH'
  set -e
  CMAKE_BIN="$(command -v cmake || true)"
  [ -x "$CMAKE_BIN" ] || CMAKE_BIN=/opt/homebrew/bin/cmake
  [ -x "$CMAKE_BIN" ] || CMAKE_BIN=/usr/local/bin/cmake
  [ -x "$CMAKE_BIN" ] || { echo "cmake is required" >&2; exit 1; }
  BUILD_DIR="$SRCROOT/../../vexdb_sqlite/build-fskit"
  BUILD_JOBS="${VEXDB_LITE_BUILD_JOBS:-4}"
  case "$BUILD_JOBS" in *[!0-9]*|0) echo "VEXDB_LITE_BUILD_JOBS must be a positive integer" >&2; exit 2;; esac
  [ "$BUILD_JOBS" -le 4 ] || BUILD_JOBS=4
  "$CMAKE_BIN" -S "$SRCROOT/../../vexdb_sqlite" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$CONFIGURATION" -DVEXDB_SQLITE_BUILD_TESTS=OFF
  "$CMAKE_BIN" --build "$BUILD_DIR" --target vexfs_runtime -j "$BUILD_JOBS"
SH
extension.build_phases.move(build_core, 0)

embed = app.new_copy_files_build_phase('Embed ExtensionKit Extensions')
embed.dst_subfolder_spec = '16'
embed.dst_path = '$(EXTENSIONS_FOLDER_PATH)'
build_file = embed.add_file_reference(extension.product_reference, true)
build_file.settings = { 'ATTRIBUTES' => %w[RemoveHeadersOnCopy] }
app.add_dependency(extension)

project.build_configurations.each do |configuration|
  configuration.build_settings.merge!(
    'CLANG_ENABLE_MODULES' => 'YES',
    'MACOSX_DEPLOYMENT_TARGET' => '26.0',
    'SDKROOT' => 'macosx',
    'SWIFT_VERSION' => '5.0'
  )
end

app.build_configurations.each do |configuration|
  configuration.build_settings.merge!(
    'CODE_SIGN_ENTITLEMENTS' => 'VexFSApp/VexFSApp.entitlements',
    'CODE_SIGN_STYLE' => 'Automatic',
    'CURRENT_PROJECT_VERSION' => '1',
    'DEVELOPMENT_TEAM' => 'BB5VK42K87',
    'ENABLE_APP_SANDBOX' => 'YES',
    'ENABLE_HARDENED_RUNTIME' => 'YES',
    'GENERATE_INFOPLIST_FILE' => 'YES',
    'INFOPLIST_KEY_CFBundleDisplayName' => 'VexDB Lite',
    'LD_RUNPATH_SEARCH_PATHS' => '$(inherited) @executable_path/../Frameworks',
    'MARKETING_VERSION' => '0.1.0',
    'PRODUCT_BUNDLE_IDENTIFIER' => 'io.vexdb.vexfs',
    'PRODUCT_NAME' => 'VexDB Lite'
  )
end

extension.build_configurations.each do |configuration|
  configuration.build_settings.merge!(
    'APPLICATION_EXTENSION_API_ONLY' => 'YES',
    'CODE_SIGN_ENTITLEMENTS' => 'VexFSAppEx/VexFSAppEx.entitlements',
    'CODE_SIGN_STYLE' => 'Automatic',
    'CURRENT_PROJECT_VERSION' => '1',
    'DEVELOPMENT_TEAM' => 'BB5VK42K87',
    'ENABLE_APP_SANDBOX' => 'YES',
    'ENABLE_HARDENED_RUNTIME' => 'YES',
    'ENABLE_USER_SCRIPT_SANDBOXING' => 'NO',
    'GENERATE_INFOPLIST_FILE' => 'YES',
    'HEADER_SEARCH_PATHS' => '$(inherited) "$(SRCROOT)/../mount/common/include"',
    'INFOPLIST_FILE' => 'VexFSAppEx/Info.plist',
    'INFOPLIST_KEY_CFBundleDisplayName' => 'VexFS file system',
    'LD_RUNPATH_SEARCH_PATHS' => '$(inherited) @executable_path/../Frameworks @executable_path/../../../../Frameworks',
    'MARKETING_VERSION' => '0.1.0',
    'OTHER_LDFLAGS' => [
      '$(inherited)',
      '"$(SRCROOT)/../../vexdb_sqlite/build-fskit/libvexfs_runtime.a"',
      '"$(SRCROOT)/../../vexdb_sqlite/build-fskit/libvexdb_lite_static.a"',
      '-lsqlite3',
      '-lc++'
    ],
    'PRODUCT_BUNDLE_IDENTIFIER' => 'io.vexdb.vexfs.extension',
    'PRODUCT_NAME' => 'VexFSAppEx',
    'SKIP_INSTALL' => 'YES',
    'SWIFT_OBJC_BRIDGING_HEADER' => 'VexFSAppEx/VexFS-Bridging-Header.h'
  )
end

project.save
puts project_path
