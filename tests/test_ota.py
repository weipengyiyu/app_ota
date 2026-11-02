"""Run real signed-bundle/state-machine operations in disposable directories.

No real device, mounts, broker, production keys, or reboot is used.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
VERIFY = Path(os.environ.get('OTA_TEST_VERIFY', str(ROOT / '.build/native/stage/usr/bin/ota_verify_sig')))

class OTA(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not VERIFY.is_file():
            raise RuntimeError('Build the HOST verifier with make verify before make check')

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='app-ota-test-')
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        self.app = self.dir / 'app'
        self.app.mkdir()
        for name in ['demo_ai', 'config_1221.ini', 'X3C_imx728_96717_96712_30fps_8m.sh',
                     'ark7118.sh', 'ark7118_colors.sh', 'start_imx728_adas.sh']:
            (self.app / name).write_text('#!/bin/sh\nexit 0\n')
        for name in ['common', 'model', 'res']:
            (self.app / name).mkdir()
            (self.app / name / 'data.bin').write_bytes(b'fixture data\n')
        self.key = self.dir / 'test-key.pem'
        self.pub = self.dir / 'public.pem'
        self.command(['openssl', 'ecparam', '-name', 'prime256v1', '-genkey', '-noout', '-out', self.key])
        self.command(['openssl', 'pkey', '-in', self.key, '-pubout', '-out', self.pub])
        self.local = self.dir / 'out/factory'
        self.env = dict(os.environ, SIGN_KEY=str(self.key), OTA_WORK_ROOT=str(self.dir / 'work'),
                        OTA_PRODUCT_CONFIG=str(ROOT / 'profiles/gk1221/product.conf'),
                        OTA_HOST_CONFIG=str(self.dir / 'no-host-config'))
        self.command(['sh', ROOT / 'host/release/prepare_factory_image.sh', self.app, self.local], env=self.env)
        self.run_dir = self.dir / 'run'
        self.run_dir.mkdir()
        self.product = self.dir / 'product_id'
        self.product.write_text('GK1221-ADAS\n')
        self.abi = self.dir / 'rootfs_abi'
        self.abi.write_text('xm1221v105-r1\n')
        self.boot = ROOT / 'runtime/core/ota-bootctl'
        self.installer = ROOT / 'runtime/installer/ota-installerd'
        self.gate = ROOT / 'runtime/activation/ota-activation-gate'
        self.policy = self.dir / 'policy.conf'
        self.policy.write_text('SAFE_STATE_FILE=' + str(self.run_dir / 'safe') + '\n'
                               'UPDATE_INHIBIT_FILE=' + str(self.run_dir / 'inhibit') + '\n'
                               'SAFE_STATE_MAX_AGE_SEC=30\nMIN_FREE_KIB=0\n'
                               'REQUIRE_WATCHDOG=0\nREQUIRE_APP_HEALTH=0\n')
        self.env.update(OTA_LOCAL_DIR=str(self.local), OTA_RUN_DIR=str(self.run_dir),
                        OTA_LIB_DIR=str(ROOT / 'runtime/core'), OTA_VERIFY_BIN=str(VERIFY),
                        OTA_PUBKEY=str(self.pub), OTA_PRODUCT_FILE=str(self.product),
                        OTA_ROOTFS_ABI_FILE=str(self.abi), OTA_BOOTCTL=str(self.boot),
                        OTA_RESERVE_KIB='0', OTA_ACTIVATION_POLICY=str(self.policy),
                        OTA_HEALTH_ADAPTER=str(ROOT / 'adapters/gk1221/health-functions.sh'),
                        OTA_REBOOT_ON_HEALTH_FAIL='0', OTA_REBOOT_ON_FAILURE='0')

    def command(self, args, *, env=None, ok=True):
        result = subprocess.run([str(x) for x in args], env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        if ok and result.returncode:
            self.fail(f'{args}: rc={result.returncode}\n{result.stdout}\n{result.stderr}')
        if not ok and not result.returncode:
            self.fail(f'Unexpected success: {args}\n{result.stdout}')
        return result

    def ctl(self, *args, ok=True):
        return self.command([self.boot, *args], env=self.env, ok=ok)

    def status(self):
        return dict(line.split('=', 1) for line in self.ctl('status').stdout.splitlines())

    def bundle(self, counter=1, version='V1.0.0', **fields):
        dest = self.dir / ('bundle-' + str(counter))
        self.command(['sh', ROOT / 'host/release/build_app_bundle.sh', self.app, version,
                  str(counter), dest, 'campaign-test'], env=self.env)
        if fields:
            manifest = dest / 'manifest.env'
            lines = manifest.read_text().splitlines()
            manifest.write_text('\n'.join(key + '=' + fields.get(key, value)
                                          for key, value in (line.split('=', 1) for line in lines)) + '\n')
            self.resign(dest)
        return dest

    def resign(self, bundle):
        (bundle / 'signed-data.bin').write_bytes((bundle / 'manifest.env').read_bytes() +
                                                  (bundle / 'manifest.sha256').read_bytes())
        self.command(['openssl', 'dgst', '-sha256', '-sign', self.key, '-out', bundle / 'manifest.sig', bundle / 'signed-data.bin'])

    def install(self, bundle, ok=True):
        return self.command([self.installer, 'install', bundle], env=self.env, ok=ok)

    def authorize(self, ok=True):
        return self.command([self.gate, 'authorize'], env=self.env, ok=ok)

    def safe(self, extra=''):
        (self.run_dir / 'safe').write_text('SAFE=1\nSTATE=IDLE\nSPEED_MPS=0\nACTUATORS_DISABLED=1\nPOWER_OK=1\n' + extra)

    def test_signed_upgrade_and_confirmation(self):
        self.assertEqual(self.status()['active_slot'], 'A')
        self.install(self.bundle())
        self.assertEqual(self.status()['phase'], 'STAGED')
        self.assertIn('app_A/root', self.ctl('select').stdout)
        self.safe()
        self.authorize()
        self.assertEqual(self.status()['phase'], 'TRIAL_ARMED')
        self.assertIn('app_B/root', self.ctl('select').stdout)
        self.assertEqual(self.status()['phase'], 'TRIAL')
        self.ctl('mark-good')
        state = self.status()
        self.assertEqual((state['phase'], state['confirmed_slot'], state['active_counter']), ('ACTIVE', 'B', '1'))

    def test_trial_idempotence_and_exhaustion_rollback(self):
        self.install(self.bundle())
        self.safe()
        self.authorize()
        self.ctl('select')
        self.ctl('select')
        self.assertEqual(self.status()['boot_attempts'], '1')
        for _ in range(3):
            marker = self.run_dir / 'trial_attempt_recorded'
            if marker.exists():
                marker.unlink()
            self.ctl('select')
        state = self.status()
        self.assertEqual((state['phase'], state['confirmed_slot'], state['active_counter']), ('ROLLED_BACK', 'A', '0'))
        self.assertEqual(state['last_error'], 'BOOT_ATTEMPTS_EXCEEDED')

    def test_tampered_payload_rejected(self):
        bundle = self.bundle()
        (bundle / 'payload/demo_ai').write_text('tampered')
        self.install(bundle, ok=False)
        self.assertEqual(self.status()['phase'], 'ACTIVE')

    def test_signature_and_product_rejected(self):
        bundle = self.bundle()
        (bundle / 'manifest.sig').write_bytes(b'not a signature')
        self.install(bundle, ok=False)
        bundle = self.bundle(2, PRODUCT_ID='WrongProduct')
        self.install(bundle, ok=False)
        self.assertEqual(self.status()['active_slot'], 'A')

    def test_wrong_abi_hardware_and_format_rejected(self):
        for n, field in enumerate(['ROOTFS_ABI', 'HARDWARE_REVISION', 'FORMAT_VERSION'], 1):
            self.install(self.bundle(n, **{field: 'wrong'}), ok=False)
        self.assertEqual(self.status()['phase'], 'ACTIVE')

    def test_duplicate_manifest_field_rejected(self):
        bundle = self.bundle()
        with (bundle / 'manifest.env').open('a') as stream:
            stream.write('PRODUCT_ID=GK1221-ADAS\n')
        self.resign(bundle)
        self.install(bundle, ok=False)

    def test_unlisted_file_and_symlink_rejected(self):
        bundle = self.bundle()
        (bundle / 'payload/extra').write_text('extra')
        self.install(bundle, ok=False)
        (bundle / 'payload/extra').unlink()
        (bundle / 'payload/linked').symlink_to(self.pub)
        self.install(bundle, ok=False)

    def test_gate_missing_stale_moving_and_inhibit(self):
        self.install(self.bundle())
        self.authorize(ok=False)
        self.safe()
        os.utime(self.run_dir / 'safe', (1, 1))
        self.authorize(ok=False)
        self.safe('SPEED_MPS=1\n')
        # Duplicate safety values must fail closed rather than accepting first.
        self.authorize(ok=False)
        self.safe()
        (self.run_dir / 'inhibit').touch()
        self.authorize(ok=False)
        self.assertEqual(self.status()['phase'], 'STAGED')

    def test_no_ungated_activation(self):
        self.install(self.bundle())
        self.ctl('activate-internal', ok=False)
        (self.run_dir / 'activation.authorization').write_text('TRANSACTION_ID=wrong\n')
        self.ctl('activate-internal', ok=False)
        self.assertEqual(self.status()['phase'], 'STAGED')

    def test_trial_cannot_be_overwritten(self):
        self.install(self.bundle())
        self.safe()
        self.authorize()
        self.ctl('select')
        original = (self.local / 'slots/app_B/slot.meta').read_bytes()
        self.install(self.bundle(2), ok=False)
        self.assertEqual((self.local / 'slots/app_B/slot.meta').read_bytes(), original)
        self.assertEqual(self.status()['phase'], 'TRIAL')

    def test_counter_replay_and_pending_replay_rejected(self):
        first = self.bundle()
        self.install(first)
        self.install(first, ok=False)
        self.safe()
        self.authorize()
        self.ctl('select')
        self.ctl('mark-good')
        self.install(first, ok=False)
        self.install(self.bundle(2, version='V2.0.0'))
        self.assertEqual(self.status()['pending_counter'], '2')

    def test_state_corruption_fallback(self):
        self.ctl('status')
        state0 = self.local / 'ota_state/state.0'
        state0.write_text('torn state')
        self.assertEqual(self.status()['generation'], '1')
        # Correct checksum cannot make semantically incomplete state valid.
        state0.write_text('GENERATION=999\n')
        Path(str(state0) + '.sha256').write_text(hashlib.sha256(state0.read_bytes()).hexdigest() + '\n')
        self.assertEqual(self.status()['generation'], '1')

    def test_both_records_lost_preserves_newest_slot_counter(self):
        self.install(self.bundle())
        self.safe()
        self.authorize()
        self.ctl('select')
        self.ctl('mark-good')
        for name in ['state.0', 'state.1']:
            (self.local / 'ota_state' / name).write_text('torn')
        state = self.status()
        self.assertEqual((state['active_slot'], state['active_counter'], state['active_version']), ('B', '1', 'V1.0.0'))

    def test_recovery_rejects_quarantined_slot(self):
        self.install(self.bundle())
        self.safe()
        self.authorize()
        self.ctl('select')
        self.ctl('mark-bad', 'HEALTH_FAILED')
        for name in ['state.0', 'state.1']:
            (self.local / 'ota_state' / name).write_text('torn')
        state = self.status()
        self.assertEqual((state['active_slot'], state['active_counter']), ('A', '0'))

    def test_lock_rejects_competing_install_and_state_write(self):
        bundle = self.bundle()
        lock = self.run_dir / 'transaction.lock'
        lock.mkdir()
        (lock / 'owner').write_text(str(os.getpid()) + '\n')
        self.install(bundle, ok=False)
        self.ctl('select', ok=False)
        (lock / 'owner').unlink()
        lock.rmdir()
        self.install(bundle)
        self.assertFalse(lock.exists())

    def test_bundle_builder_refuses_existing_output_and_symlinks(self):
        bundle = self.bundle()
        original = (bundle / 'signed-data.bin').read_bytes()
        self.command(['sh', ROOT / 'host/release/build_app_bundle.sh', self.app, 'V1.0.0', '1', bundle], env=self.env, ok=False)
        self.assertEqual((bundle / 'signed-data.bin').read_bytes(), original)
        (self.app / 'link').symlink_to(self.pub)
        self.command(['sh', ROOT / 'host/release/build_app_bundle.sh', self.app, 'V1.0.0', '2', self.dir / 'new'], env=self.env, ok=False)

    def test_native_verifier_real_crypto(self):
        bundle = self.bundle()
        args = [VERIFY, self.pub, bundle / 'signed-data.bin', bundle / 'manifest.sig']
        self.command(args)
        with (bundle / 'signed-data.bin').open('ab') as stream:
            stream.write(b'tamper')
        self.command(args, ok=False)

    def test_health_adapter_confirms_advancing_frames(self):
        self.install(self.bundle())
        self.safe()
        self.authorize()
        self.ctl('select')
        log_dir = self.dir / 'health'
        log_dir.mkdir()
        worker = subprocess.Popen(['python3', '-c', '''
import os, pathlib, sys, time
pid = os.getpid()
path = pathlib.Path(sys.argv[1]) / ('demo_ai_test_%s.log' % pid)
for i in range(100):
    with path.open('a') as f:
        f.write('ALIVE pid=%s svp_ok=%s vpss_fail=0 svp_fail=0 \\n' % (pid, i))
    time.sleep(0.2)
''', str(log_dir)])
        self.addCleanup(lambda: worker.poll() is not None or worker.terminate())
        (self.run_dir / 'demo_ai.pid').write_text(str(worker.pid))
        env = dict(self.env, APP_HEALTH_LOG_DIR=str(log_dir), OTA_HEALTH_POLICY=str(self.dir / 'missing-policy'),
                   OTA_HEALTH_TIMEOUT='8', OTA_HEALTH_INTERVAL='1', OTA_HEALTH_MIN_GOOD='2')
        self.command([ROOT / 'adapters/gk1221/ota-healthd'], env=env)
        worker.terminate()
        worker.wait(timeout=5)
        self.assertEqual(self.status()['active_counter'], '1')

    def test_health_adapter_process_failure_rolls_back(self):
        self.install(self.bundle())
        self.safe()
        self.authorize()
        self.ctl('select')
        (self.run_dir / 'demo_ai.pid').write_text('99999999\n')
        env = dict(self.env, OTA_HEALTH_POLICY=str(self.dir / 'missing-policy'),
                   OTA_HEALTH_TIMEOUT='3', OTA_HEALTH_INTERVAL='1', OTA_HEALTH_MIN_GOOD='2')
        self.command([ROOT / 'adapters/gk1221/ota-healthd'], env=env, ok=False)
        self.assertEqual(self.status()['last_error'], 'PROCESS_EXITED')

    def test_global_release_counter_crosses_versions_and_reserves_failure(self):
        publisher = self.dir / 'publisher'
        calls = self.dir / 'publish.calls'
        publisher.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "' + str(calls) + '"\n')
        publisher.chmod(0o755)
        env = dict(self.env, OTA_APP_SOURCE=str(self.app), OTA_PUBLISH_COMMAND=str(publisher))
        release = ROOT / 'host/release/ota-release.sh'
        self.command([release, 'V1.0.0', '--device', 'test-device'], env=env)
        self.command([release, 'V2.0.0', '--device', 'test-device'], env=env)
        self.assertIn('V1.0.0 1 ', calls.read_text())
        self.assertIn('V2.0.0 2 ', calls.read_text())
        publisher.write_text('#!/bin/sh\nexit 1\n')
        self.command([release, 'V3.0.0', '--device', 'test-device'], env=env, ok=False)
        self.assertIn('LAST_RELEASE_COUNTER=3', (self.dir / 'work/release-index.env').read_text())
        self.assertFalse((self.dir / 'work/release.lock').exists())

    def test_empty_prod_target_does_not_publish(self):
        env = dict(self.env, OTA_APP_SOURCE=str(self.app), PROD_DEVICES='')
        self.command([ROOT / 'host/release/ota-release.sh', 'V1.0.0'], env=env, ok=False)
        self.assertFalse((self.dir / 'work/release-index.env').exists())

    def test_overlay_requires_explicit_key_and_has_all_components(self):
        stage = self.dir / 'staging/rootfs'
        native = ROOT / '.build/native/stage'
        args = ['python3', ROOT / 'tools/install_runtime.py', '--destdir', stage, '--native-stage', native]
        self.command(args, ok=False)
        if all((native / 'usr/bin' / n).is_file() for n in ['ota-mqtt-sub', 'ota-http']):
            self.command(args + ['--public-key', self.pub])
            self.assertTrue((stage / 'usr/sbin/ota-agentd').is_file())
            self.assertTrue((stage / 'usr/lib/ota/health-functions.sh').is_file())
            self.assertIn('ENABLED=0', (stage / 'etc/ota/agent.conf').read_text())
            self.assertEqual((stage / 'etc/ota/agent.conf').stat().st_mode & 0o777, 0o600)
        self.command(['python3', ROOT / 'tools/install_runtime.py', '--destdir', '/'], ok=False)

    def test_shell_syntax(self):
        for folder in ['runtime', 'host', 'compat', 'integration', 'adapters', 'lib']:
            for path in (ROOT / folder).rglob('*'):
                if path.is_file() and path.read_bytes().startswith(b'#!/bin/sh'):
                    self.command(['sh', '-n', path])

if __name__ == '__main__':
    unittest.main()
