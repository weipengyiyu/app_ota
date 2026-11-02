#!/usr/bin/env python3
"""Stage a rootfs overlay; never install onto the execution host's root."""
import argparse
import pathlib
import shutil

ROOT = pathlib.Path(__file__).resolve().parents[1]
BINARIES = {
    'ota-bootctl': 'runtime/core',
    'ota-installerd': 'runtime/installer',
    'ota-migrate-legacy': 'runtime/installer',
    'ota-agentd': 'runtime/agent',
    'ota-activation-gate': 'runtime/activation',
    'ota-app-supervisor': 'runtime/supervisor',
    'app-recovery': 'runtime/recovery',
    'ota-healthd': 'adapters/gk1221',
}

def put(src, dst, mode):
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dst)
    dst.chmod(mode)

def checked_dest(value):
    if not value:
        raise ValueError('an explicit staging directory is required')
    dest = pathlib.Path(value).resolve()
    if dest == pathlib.Path('/') or dest == ROOT or ROOT in dest.parents and '.build' not in dest.parts:
        raise ValueError('use a staging directory, not / or project source')
    # Require a dedicated staging directory. Avoid /usr, /etc, /home, etc.
    if len(dest.parts) < 4:
        raise ValueError('staging path must be a dedicated nested directory')
    return dest

def install(dest, native=None, public_key=None, lab=False):
    if not lab:
        if not native:
            raise ValueError('build native tools first, then pass --native-stage')
        for name in ['ota_verify_sig', 'ota-mqtt-sub', 'ota-http']:
            path = native / 'usr/bin' / name
            if not path.is_file():
                raise ValueError('native executable missing: ' + str(path))
        if not public_key or not public_key.is_file():
            raise ValueError('an explicit trusted release public key is required')
    for name, sub in BINARIES.items():
        put(ROOT / sub / name, dest / (name if lab else 'usr/sbin/' + name), 0o755)
    for src, name in [('runtime/core/ota-state-lib.sh', 'ota-state-lib.sh'),
                      ('adapters/gk1221/health-functions.sh', 'health-functions.sh')]:
        put(ROOT / src, dest / (('lib/' if lab else 'usr/lib/ota/') + name), 0o644)
    for src, name, mode in [('config/agent.conf.example', 'agent.conf', 0o600),
                            ('config/activation-policy.conf', 'activation-policy.conf', 0o644),
                            ('profiles/gk1221/product.conf', 'product.conf', 0o644)]:
        put(ROOT / src, dest / (('etc/' if lab else 'etc/ota/') + name), mode)
    etc = dest / ('etc' if lab else 'etc/ota')
    etc.mkdir(parents=True, exist_ok=True)
    (etc / 'product_id').write_text('GK1221-ADAS\n')
    (etc / 'rootfs_abi').write_text('xm1221v105-r1\n')
    if lab:
        put(ROOT / 'compat/lab/ota-bind-mounts.sh', dest / 'ota-bind-mounts.sh', 0o755)
        return
    for name in ['ota_verify_sig', 'ota-mqtt-sub', 'ota-http']:
        put(native / 'usr/bin' / name, dest / 'usr/bin' / name, 0o755)
    put(public_key, dest / 'etc/ota_pubkey.pem', 0o644)
    put(ROOT / 'integration/init/ota-start', dest / 'usr/sbin/ota-start', 0o755)
    pki = dest / 'etc/ota/pki'
    pki.mkdir(exist_ok=True)
    pki.chmod(0o700)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--destdir')
    p.add_argument('--native-stage', type=pathlib.Path)
    p.add_argument('--public-key', type=pathlib.Path)
    p.add_argument('--lab-control')
    args = p.parse_args()
    if bool(args.destdir) == bool(args.lab_control):
        p.error('choose exactly one of --destdir or --lab-control')
    try:
        dest = checked_dest(args.destdir or args.lab_control)
        install(dest, args.native_stage, args.public_key, bool(args.lab_control))
    except ValueError as error:
        p.error(str(error))
    print('Staged APP-OTA files at ' + str(dest))

if __name__ == '__main__':
    main()
