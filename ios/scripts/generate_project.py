#!/usr/bin/env python3
"""Generate a small, deterministic Xcode project without third-party tools."""
from pathlib import Path
import hashlib
import plistlib
import json

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT / 'Talkbot.xcodeproj'

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
products = add('group:products', 'PBXGroup', children=[app_product, core_product], name='Products', sourceTree='<group>')
app_group = add('group:app', 'PBXGroup', children=[source_refs[s.relative_to(ROOT).as_posix()] for s in app_sources], name='Talkbot', sourceTree='<group>')
core_group = add('group:core', 'PBXGroup', children=[source_refs[s.relative_to(ROOT).as_posix()] for s in core_sources], name='TalkbotCore', sourceTree='<group>')
main_group = add('group:root', 'PBXGroup', children=[app_group, core_group, products], sourceTree='<group>')

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
app_frameworks = add('phase:frameworks:app', 'PBXFrameworksBuildPhase', buildActionMask=2147483647, files=[link_core], runOnlyForDeploymentPostprocessing=0)
core_frameworks = add('phase:frameworks:core', 'PBXFrameworksBuildPhase', buildActionMask=2147483647, files=[], runOnlyForDeploymentPostprocessing=0)
embed = add('phase:embed', 'PBXCopyFilesBuildPhase', buildActionMask=2147483647, dstPath='', dstSubfolderSpec=10, files=[embed_core], name='Embed Frameworks', runOnlyForDeploymentPostprocessing=0)
resources = add('phase:resources', 'PBXResourcesBuildPhase', buildActionMask=2147483647, files=[], runOnlyForDeploymentPostprocessing=0)

common = {'SWIFT_VERSION': '5.0', 'IPHONEOS_DEPLOYMENT_TARGET': '16.0', 'SDKROOT': 'iphoneos',
          'TARGETED_DEVICE_FAMILY': '1,2', 'CLANG_ENABLE_MODULES': 'YES', 'SWIFT_STRICT_CONCURRENCY': 'targeted'}

def configurations(name, extra):
    configs = []
    for mode in ['Debug', 'Release']:
        settings = {**common, **extra}
        settings.update({'SWIFT_OPTIMIZATION_LEVEL': '-Onone' if mode == 'Debug' else '-O',
                         'SWIFT_ACTIVE_COMPILATION_CONDITIONS': 'DEBUG' if mode == 'Debug' else '',
                         'DEBUG_INFORMATION_FORMAT': 'dwarf' if mode == 'Debug' else 'dwarf-with-dsym'})
        configs.append(add('config:' + name + ':' + mode, 'XCBuildConfiguration', buildSettings=settings, name=mode))
    return add('configs:' + name, 'XCConfigurationList', buildConfigurations=configs, defaultConfigurationIsVisible=0, defaultConfigurationName='Release')

project_configs = configurations('project', {'CODE_SIGN_STYLE': 'Automatic', 'ENABLE_USER_SCRIPT_SANDBOXING': 'YES'})
app_configs = configurations('app', {'PRODUCT_BUNDLE_IDENTIFIER': 'org.talkbot.companion', 'PRODUCT_NAME': '$(TARGET_NAME)',
    'INFOPLIST_FILE': 'Talkbot/Info.plist', 'GENERATE_INFOPLIST_FILE': 'NO', 'MARKETING_VERSION': '0.2.0', 'CURRENT_PROJECT_VERSION': '1',
    'LD_RUNPATH_SEARCH_PATHS': '$(inherited) @executable_path/Frameworks', 'SUPPORTED_PLATFORMS': 'iphoneos iphonesimulator'})
core_configs = configurations('core', {'PRODUCT_BUNDLE_IDENTIFIER': 'org.talkbot.core', 'PRODUCT_NAME': '$(TARGET_NAME)',
    'DEFINES_MODULE': 'YES', 'GENERATE_INFOPLIST_FILE': 'YES', 'SKIP_INSTALL': 'YES', 'SUPPORTED_PLATFORMS': 'iphoneos iphonesimulator'})
core_target = add('target:core', 'PBXNativeTarget', buildConfigurationList=core_configs, buildPhases=[core_source_phase, core_frameworks], buildRules=[], dependencies=[], name='TalkbotCore', productName='TalkbotCore', productReference=core_product, productType='com.apple.product-type.framework')
proxy = add('proxy:core', 'PBXContainerItemProxy', containerPortal=ident('project'), proxyType=1, remoteGlobalIDString=core_target, remoteInfo='TalkbotCore')
dependency = add('dependency:core', 'PBXTargetDependency', target=core_target, targetProxy=proxy)
app_target = add('target:app', 'PBXNativeTarget', buildConfigurationList=app_configs, buildPhases=[app_source_phase, app_frameworks, resources, embed], buildRules=[], dependencies=[dependency], name='Talkbot', productName='Talkbot', productReference=app_product, productType='com.apple.product-type.application')
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
        'UISupportedInterfaceOrientations': ['UIInterfaceOrientationLandscapeLeft', 'UIInterfaceOrientationLandscapeRight'],
        'UISupportedInterfaceOrientations~ipad': ['UIInterfaceOrientationLandscapeLeft', 'UIInterfaceOrientationLandscapeRight'],
        'UIRequiresFullScreen': True,
        'NSMicrophoneUsageDescription': '디노와 음성으로 대화하려면 마이크가 필요해요. 음성은 대화를 위해 음성 API로 전송됩니다.',
        'NSCameraUsageDescription': '디노가 아이의 얼굴 위치와 시선을 따라봐요. 영상은 기기 안에서만 처리하며 전송하거나 저장하지 않아요.',
        'NSFaceIDUsageDescription': '보호자 설정을 열 때 기기 소유자를 확인해요.'}
with (ROOT / 'Talkbot' / 'Info.plist').open('wb') as handle:
    plistlib.dump(info, handle, sort_keys=False)
print(f'Generated {len(app_sources)} app sources + {len(core_sources)} core sources')
