#!/usr/bin/env python3
"""Build a separate, native-library-free companion fixture APK for an emulator.

    python3 build_preview.py --out /tmp/companion-preview.apk [--android-sdk SDK] [--assets ROOT]
--assets is the generated root containing companion/; all game data stays outside the source tree.
"""
import argparse
import io
from pathlib import Path
import tempfile
import zipfile
import build_apk
import build_dex

HERE = Path(__file__).resolve().parent
PACKAGE = 'com.aure.totk.companionpreview'
ACTIVITY = PACKAGE + '.PreviewActivity'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--android-sdk', type=Path)
    parser.add_argument('--assets', type=Path, help='generated asset root containing companion/')
    parser.add_argument('--key', type=Path, help='default: preview-key.pem beside output')
    args = parser.parse_args(argv)
    args.out = args.out.expanduser().resolve()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='totk-preview-') as tmp:
        dex = build_dex.build(Path(tmp) / 'classes.dex', args.android_sdk, [HERE / 'preview/java'])
        A = True
        tree = ('manifest', [(False, 'package', 'str', PACKAGE), (A, 'versionCode', 'int', 1), (A, 'versionName', 'str', '1')], [
            ('uses-sdk', [(A, 'minSdkVersion', 'int', 33), (A, 'targetSdkVersion', 'int', 33)], []),
            ('application', [(A, 'label', 'str', 'Purah Pad Preview'), (A, 'hasCode', 'bool', True), (A, 'debuggable', 'bool', True),
                             (A, 'theme', 'ref', build_apk.THEME_NOTITLEBAR_FULLSCREEN)], [
                ('activity', [(A, 'name', 'str', ACTIVITY), (A, 'label', 'str', 'Purah Pad Preview'), (A, 'exported', 'bool', True),
                              (A, 'launchMode', 'int', build_apk.SINGLE_TASK), (A, 'configChanges', 'hex', build_apk.CONFIG_CHANGES)], [
                    ('intent-filter', [], [
                        ('action', [(A, 'name', 'str', 'android.intent.action.MAIN')], []),
                        ('category', [(A, 'name', 'str', 'android.intent.category.LAUNCHER')], []),
                    ]),
                ]),
            ]),
        ])
        entries = [('AndroidManifest.xml', build_apk.binary_xml(tree)), ('classes.dex', dex.read_bytes())]
        entries += [(f'assets/fixtures/{p.name}', p.read_bytes()) for p in sorted((HERE / 'preview/fixtures').glob('*.json'))]
        if args.assets:
            root = args.assets.expanduser().resolve()
            if not (root / 'companion/index.json').is_file():
                parser.error('--assets must contain companion/index.json')
            entries += [(f'assets/{p.relative_to(root).as_posix()}', p.read_bytes())
                        for p in sorted((root / 'companion').rglob('*')) if p.is_file()]
        key, cert = build_apk.load_key(args.key or args.out.parent / 'preview-key.pem', make=True)
        entries = build_apk.v1_files(entries, key, cert) + entries
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, 'w') as archive:
            for name, data in entries:
                info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_STORED if Path(name).suffix.lower() in ('.png', '.webp') else zipfile.ZIP_DEFLATED
                archive.writestr(info, data)
        args.out.write_bytes(build_apk.v2_sign(buf.getvalue(), key, cert))
    print(f'wrote {args.out}; launch {PACKAGE}/{ACTIVITY} with --ei page 0|1|2 --es fixture ready|title|starting|unsupported|sky|depths|unavailable|loading|motion|nativebuff|actions|event|menuonly|aiming [--ei delayMs N] [--ez here true]')


if __name__ == '__main__':
    main()
