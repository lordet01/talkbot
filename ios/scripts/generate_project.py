#!/usr/bin/env python3
"""Generate a small, deterministic Xcode project without third-party tools."""
from pathlib import Path
import hashlib
import plistlib
import json

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT / 'Talkbot.xcodeproj'
generated = ROOT / 'Talkbot' / 'Generated' / 'BundledSecrets.swift'
generated.parent.mkdir(parents=True, exist_ok=True)
if not generated.exists():
    generated.write_text('enum BundledSecrets {\n    static let openAIAPIKey = ""\n}\n')

def ident(label):
    return hashlib.sha1(label.encode()).hexdigest()[:24].upper()

def quote(value):
    return json.dumps(str(value), ensure_ascii=False)

objects = {}
def add(label, isa, **fields):
    key = ident(label)
    objects[key] = {'isa': isa, **fields}
    return key

app_sources = sorted((ROOT / 'Talkbot').rglob('*.swift'))
core_sources = sorted((ROOT / 'TalkbotCore' / 'Sources').rglob('*.swift'))
source_refs = {}
for source in app_sources + core_sources:
    relative = source.relative_to(ROOT).as_posix()
    source_refs[relative] = add('file:' + relative, 'PBXFileReference', lastKnownFileType='sourcecode.swift', path=relative, sourceTree='<group>')

app_product = add('product:app', 'PBXFileReference', explicitFileType='wrapper.application', path='Talkbot.app', sourceTree='BUILT_PRODUCTS_DIR')
core_product = add('product:core', 'PBXFileReference', explicitFileType='wrapper.framework', path='TalkbotCore.framework', sourceTree='BUILT_PRODUCTS_DIR')
vendor_xcs = sorted((ROOT / 'Vendor').glob('*.xcframework'))
vendor_refs = {}
for xc in vendor_xcs:
    rel = xc.relative_to(ROOT).as_posix()
    vendor_refs[rel] = add('file:' + rel, 'PBXFileReference', lastKnownFileType='wrapper.xcframework', path=rel, sourceTree='<group>')
assets_ref = add('file:Talkbot/Assets.xcassets', 'PBXFileReference', lastKnownFileType='folder.assetcatalog', path='Talkbot/Assets.xcassets', sourceTree='<group>')
products = add('group:products', 'PBXGroup', children=[app_product, core_product], name='Products', sourceTree='<group>')
app_group = add('group:app', 'PBXGroup', children=[source_refs[s.relative_to(ROOT).as_posix()] for s in app_sources] + [assets_ref], name='Talkbot', sourceTree='<group>')
core_group = add('group:core', 'PBXGroup', children=[source_refs[s.relative_to(ROOT).as_posix()] for s in core_sources], name='TalkbotCore', sourceTree='<group>')
vendor_group = add('group:vendor', 'PBXGroup', children=list(vendor_refs.values()), name='Vendor', sourceTree='<group>')
main_group = add('group:root', 'PBXGroup', children=[app_group, core_group, vendor_group, products], sourceTree='<group>')

def source_phase(name, sources):
    files = []
    for source in sources:
        path = source.relative_to(ROOT).as_posix()
        files.append(add('build:' + path, 'PBXBuildFile', fileRef=source_refs[path]))
    return add('phase:sources:' + name, 'PBXSourcesBuildPhase', buildActionMask=2147483647, files=files, runOnlyForDeploymentPostprocessing=0)

app_source_phase = source_phase('app', app_sources)
core_source_phase = source_phase('core', core_sources)
link_core = add('build:link-core', 'PBXBuildFile', fileRef=core_product)
embed_core = add('build:embed-core', 'PBXBuildFile', fileRef=core_product, settings={'ATTRIBUTES': ['CodeSignOnCopy', 'RemoveHeadersOnCopy']})
vendor_link = []
vendor_embed = []
for rel, ref in vendor_refs.items():
    vendor_link.append(add('build:link-' + rel, 'PBXBuildFile', fileRef=ref))
    vendor_embed.append(add('build:embed-' + rel, 'PBXBuildFile', fileRef=ref, settings={'ATTRIBUTES': ['CodeSignOnCopy', 'RemoveHeadersOnCopy']}))
app_frameworks = add('phase:frameworks:app', 'PBXFrameworksBuildPhase', buildActionMask=2147483647, files=[link_core] + vendor_link, runOnlyForDeploymentPostprocessing=0)
core_frameworks = add('phase:frameworks:core', 'PBXFrameworksBuildPhase', buildActionMask=2147483647, files=[], runOnlyForDeploymentPostprocessing=0)
embed = add('phase:embed', 'PBXCopyFilesBuildPhase', buildActionMask=2147483647, dstPath='', dstSubfolderSpec=10, files=[embed_core] + vendor_embed, name='Embed Frameworks', runOnlyForDeploymentPostprocessing=0)
assets_build = add('build:Talkbot/Assets.xcassets', 'PBXBuildFile', fileRef=assets_ref)
resources = add('phase:resources', 'PBXResourcesBuildPhase', buildActionMask=2147483647, files=[assets_build], runOnlyForDeploymentPostprocessing=0)
secrets = add('phase:secrets', 'PBXShellScriptBuildPhase', alwaysOutOfDate=1, buildActionMask=2147483647, files=[],
    inputFileListPaths=[], inputPaths=['$(SRCROOT)/../.env', '$(SRCROOT)/scripts/embed_env_secrets.py'],
    name='Embed .env OpenAI key', outputFileListPaths=[],
    outputPaths=['$(SRCROOT)/Talkbot/Generated/BundledSecrets.swift'],
    runOnlyForDeploymentPostprocessing=0, shellPath='/bin/sh',
    shellScript='python3 "${SRCROOT}/scripts/embed_env_secrets.py"\n')

common = {'SWIFT_VERSION': '5.0', 'IPHONEOS_DEPLOYMENT_TARGET': '16.0', 'SDKROOT': 'iphoneos',
          'TARGETED_DEVICE_FAMILY': '1,2', 'CLANG_ENABLE_MODULES': 'YES', 'SWIFT_STRICT_CONCURRENCY': 'targeted',
          'CLANG_CXX_LANGUAGE_STANDARD': 'gnu++17'}

def configurations(name, extra):
    configs = []
    for mode in ['Debug', 'Release']:
        settings = {**common, **extra}
        settings.update({'SWIFT_OPTIMIZATION_LEVEL': '-Onone' if mode == 'Debug' else '-O',
                         'SWIFT_ACTIVE_COMPILATION_CONDITIONS': 'DEBUG' if mode == 'Debug' else '',
                         'DEBUG_INFORMATION_FORMAT': 'dwarf' if mode == 'Debug' else 'dwarf-with-dsym'})
        configs.append(add('config:' + name + ':' + mode, 'XCBuildConfiguration', buildSettings=settings, name=mode))
    return add('configs:' + name, 'XCConfigurationList', buildConfigurations=configs, defaultConfigurationIsVisible=0, defaultConfigurationName='Release')

project_configs = configurations('project', {'CODE_SIGN_STYLE': 'Automatic', 'ENABLE_USER_SCRIPT_SANDBOXING': 'NO',
    'DEVELOPMENT_TEAM': 'ZAQK54P5DT'})
app_configs = configurations('app', {'PRODUCT_BUNDLE_IDENTIFIER': 'org.talkbot.companion', 'PRODUCT_NAME': '$(TARGET_NAME)',
    'INFOPLIST_FILE': 'Talkbot/Info.plist', 'GENERATE_INFOPLIST_FILE': 'NO', 'MARKETING_VERSION': '0.3.1', 'CURRENT_PROJECT_VERSION': '1',
    'LD_RUNPATH_SEARCH_PATHS': '$(inherited) @executable_path/Frameworks', 'SUPPORTED_PLATFORMS': 'iphoneos',
    'ASSETCATALOG_COMPILER_APPICON_NAME': 'AppIcon', 'ENABLE_DEBUG_DYLIB': 'NO',
    'FRAMEWORK_SEARCH_PATHS': '$(inherited) $(PROJECT_DIR)/Vendor',
    'OTHER_LDFLAGS': '$(inherited) -lc++'})
core_configs = configurations('core', {'PRODUCT_BUNDLE_IDENTIFIER': 'org.talkbot.core', 'PRODUCT_NAME': '$(TARGET_NAME)',
    'DEFINES_MODULE': 'YES', 'GENERATE_INFOPLIST_FILE': 'YES', 'SKIP_INSTALL': 'YES', 'SUPPORTED_PLATFORMS': 'iphoneos iphonesimulator',
    'DYLIB_INSTALL_NAME_BASE': '@rpath', 'LD_RUNPATH_SEARCH_PATHS': '$(inherited) @executable_path/Frameworks @loader_path/Frameworks'})
core_target = add('target:core', 'PBXNativeTarget', buildConfigurationList=core_configs, buildPhases=[core_source_phase, core_frameworks], buildRules=[], dependencies=[], name='TalkbotCore', productName='TalkbotCore', productReference=core_product, productType='com.apple.product-type.framework')
proxy = add('proxy:core', 'PBXContainerItemProxy', containerPortal=ident('project'), proxyType=1, remoteGlobalIDString=core_target, remoteInfo='TalkbotCore')
dependency = add('dependency:core', 'PBXTargetDependency', target=core_target, targetProxy=proxy)
app_target = add('target:app', 'PBXNativeTarget', buildConfigurationList=app_configs, buildPhases=[secrets, app_source_phase, app_frameworks, resources, embed], buildRules=[], dependencies=[dependency], name='Talkbot', productName='Talkbot', productReference=app_product, productType='com.apple.product-type.application')
project = add('project', 'PBXProject', attributes={'LastUpgradeCheck': '1600', 'TargetAttributes': {app_target: {'CreatedOnToolsVersion': '16.0'}, core_target: {'CreatedOnToolsVersion': '16.0'}}}, buildConfigurationList=project_configs, compatibilityVersion='Xcode 14.0', developmentRegion='ko', hasScannedForEncodings=0, knownRegions=['ko', 'en', 'Base'], mainGroup=main_group, productRefGroup=products, projectDirPath='', projectRoot='', targets=[app_target, core_target])

# JSON-style strings are accepted by the OpenStep plist parser; IDs must remain
# unquoted when used as keys, but may be quoted when used as references.
def render(value, depth=0):
    if isinstance(value, dict):
        return '{\n' + ''.join('\t' * (depth + 1) + quote(k) + ' = ' + render(v, depth + 1) + ';\n' for k, v in value.items()) + '\t' * depth + '}'
    if isinstance(value, list):
        return '(' + ', '.join(render(v, depth) for v in value) + (',' if value else '') + ')'
    if isinstance(value, int): return str(value)
    return quote(value)

PROJECT.mkdir(exist_ok=True)
(PROJECT / 'project.pbxproj').write_text('// !$*UTF8*$!\n' + render({'archiveVersion': 1, 'classes': {}, 'objectVersion': 56, 'objects': objects, 'rootObject': project}) + '\n')
schemes = PROJECT / 'xcshareddata' / 'xcschemes'
schemes.mkdir(parents=True, exist_ok=True)
(schemes / 'Talkbot.xcscheme').write_text(f'''<?xml version="1.0" encoding="UTF-8"?>
<Scheme LastUpgradeVersion="1600" version="1.3">
  <BuildAction parallelizeBuildables="YES" buildImplicitDependencies="YES">
    <BuildActionEntries><BuildActionEntry buildForTesting="YES" buildForRunning="YES" buildForProfiling="YES" buildForArchiving="YES" buildForAnalyzing="YES">
      <BuildableReference BuildableIdentifier="primary" BlueprintIdentifier="{app_target}" BuildableName="Talkbot.app" BlueprintName="Talkbot" ReferencedContainer="container:Talkbot.xcodeproj"/>
    </BuildActionEntry></BuildActionEntries>
  </BuildAction>
  <LaunchAction buildConfiguration="Debug" selectedDebuggerIdentifier="Xcode.DebuggerFoundation.Debugger.LLDB" selectedLauncherIdentifier="Xcode.IDEFoundation.Launcher.LLDB" launchStyle="0" useCustomWorkingDirectory="NO" ignoresPersistentStateOnLaunch="NO" debugDocumentVersioning="YES" debugServiceExtension="internal" allowLocationSimulation="YES">
    <BuildableProductRunnable runnableDebuggingMode="0"><BuildableReference BuildableIdentifier="primary" BlueprintIdentifier="{app_target}" BuildableName="Talkbot.app" BlueprintName="Talkbot" ReferencedContainer="container:Talkbot.xcodeproj"/></BuildableProductRunnable>
  </LaunchAction>
  <ProfileAction buildConfiguration="Release" shouldUseLaunchSchemeArgsEnv="YES" savedToolIdentifier="" useCustomWorkingDirectory="NO" debugDocumentVersioning="YES"><BuildableProductRunnable runnableDebuggingMode="0"><BuildableReference BuildableIdentifier="primary" BlueprintIdentifier="{app_target}" BuildableName="Talkbot.app" BlueprintName="Talkbot" ReferencedContainer="container:Talkbot.xcodeproj"/></BuildableProductRunnable></ProfileAction>
  <AnalyzeAction buildConfiguration="Debug"/>
  <ArchiveAction buildConfiguration="Release" revealArchiveInOrganizer="YES"/>
</Scheme>
''')
info = {'CFBundleDevelopmentRegion': 'ko', 'CFBundleDisplayName': '디노', 'CFBundleExecutable': '$(EXECUTABLE_NAME)',
        'CFBundleIdentifier': '$(PRODUCT_BUNDLE_IDENTIFIER)', 'CFBundleInfoDictionaryVersion': '6.0', 'CFBundleName': '$(PRODUCT_NAME)',
        'CFBundlePackageType': 'APPL', 'CFBundleShortVersionString': '$(MARKETING_VERSION)', 'CFBundleVersion': '$(CURRENT_PROJECT_VERSION)',
        'LSRequiresIPhoneOS': True, 'UILaunchScreen': {}, 'UIApplicationSupportsIndirectInputEvents': True,
        'UISupportedInterfaceOrientations': ['UIInterfaceOrientationPortrait'],
        'UISupportedInterfaceOrientations~ipad': ['UIInterfaceOrientationPortrait', 'UIInterfaceOrientationPortraitUpsideDown'],
        'UIRequiresFullScreen': True,
        'NSMicrophoneUsageDescription': '디노와 이 아이폰 안에서 한국어로 대화하려면 마이크가 필요해요. 음성은 기기를 나가지 않아요.',
        'NSSpeechRecognitionUsageDescription': '아이가 한 말을 이 아이폰에서 알아들어요. 인식은 온디바이스로만 하고 서버로 보내지 않아요.',
        'NSCameraUsageDescription': '디노가 아이의 얼굴 위치와 시선을 따라봐요. 영상은 기기 안에서만 처리하며 전송하거나 저장하지 않아요.',
        'NSFaceIDUsageDescription': '보호자 설정을 열 때 기기 소유자를 확인해요.'}
with (ROOT / 'Talkbot' / 'Info.plist').open('wb') as handle:
    plistlib.dump(info, handle, sort_keys=False)
print(f'Generated {len(app_sources)} app sources + {len(core_sources)} core sources')
