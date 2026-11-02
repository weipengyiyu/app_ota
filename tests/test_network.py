"""Exercise compiled TLS clients against loopback-only ephemeral servers."""
import http.server
from pathlib import Path
import shutil
import socket
import ssl
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / '.build/native/stage/usr/bin'

class TLS(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='app-ota-tls-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.dir = Path(cls.tmp.name)
        def openssl(*args):
            result = subprocess.run(['openssl', *map(str, args)], capture_output=True, timeout=20)
            if result.returncode:
                raise RuntimeError(result.stderr.decode())
        openssl('req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:prime256v1',
                '-nodes', '-keyout', cls.dir / 'ca.key', '-out', cls.dir / 'ca.pem',
                '-days', '1', '-subj', '/CN=OTA-test-CA')
        for name, cn, usage in [('server', 'localhost', 'serverAuth'), ('client', 'device-test', 'clientAuth')]:
            openssl('req', '-new', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:prime256v1',
                    '-nodes', '-keyout', cls.dir / (name + '.key'), '-out', cls.dir / (name + '.csr'),
                    '-subj', '/CN=' + cn)
            ext = cls.dir / (name + '.ext')
            ext.write_text('basicConstraints=CA:FALSE\nextendedKeyUsage=' + usage +
                           ('\nsubjectAltName=DNS:localhost\n' if name == 'server' else '\n'))
            openssl('x509', '-req', '-in', cls.dir / (name + '.csr'), '-CA', cls.dir / 'ca.pem',
                    '-CAkey', cls.dir / 'ca.key', '-CAcreateserial', '-out', cls.dir / (name + '.pem'),
                    '-days', '1', '-extfile', ext)

    def test_https_mtls_and_hostname_verification(self):
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                body = b'authenticated bundle data\n'
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            def log_message(self, *_):
                pass
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        ctx.load_cert_chain(self.dir / 'server.pem', self.dir / 'server.key')
        ctx.load_verify_locations(self.dir / 'ca.pem')
        ctx.verify_mode = ssl.CERT_REQUIRED
        server.socket = ctx.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, kwargs={'poll_interval': 0.1}, daemon=True)
        thread.start()
        try:
            base = [str(BIN / 'ota-http'), '--silent', '--show-error', '--fail', '--max-time', '5',
                    '--proto', '=https', '--cacert', str(self.dir / 'ca.pem')]
            certs = ['--cert', str(self.dir / 'client.pem'), '--key', str(self.dir / 'client.key')]
            url = 'https://localhost:%d/bundle' % server.server_port
            good = subprocess.run(base + certs + [url], capture_output=True, timeout=10)
            self.assertEqual(good.returncode, 0, good.stderr)
            self.assertEqual(good.stdout, b'authenticated bundle data\n')
            no_cert = subprocess.run(base + [url], capture_output=True, timeout=10)
            self.assertNotEqual(no_cert.returncode, 0)
            wrong_name = subprocess.run(base + certs + [url.replace('localhost', '127.0.0.1')], capture_output=True, timeout=10)
            self.assertNotEqual(wrong_name.returncode, 0)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)

    @unittest.skipUnless(shutil.which('mosquitto') and shutil.which('mosquitto_pub'),
                         'loopback broker integration requires mosquitto + mosquitto_pub')
    def test_mqtt_tls_persistent_campaign_notification(self):
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            port = probe.getsockname()[1]
        with tempfile.TemporaryDirectory(dir=self.dir, prefix='mqtt-') as tmp:
            work = Path(tmp)
            config = work / 'mosquitto.conf'
            config.write_text('listener %s 127.0.0.1\nallow_anonymous true\n' % port +
                              'cafile %s\ncertfile %s\nkeyfile %s\n' % (self.dir / 'ca.pem', self.dir / 'server.pem', self.dir / 'server.key') +
                              'require_certificate true\nuse_identity_as_username true\npersistence false\n')
            with (work / 'broker.log').open('w') as log:
                broker = subprocess.Popen(['mosquitto', '-c', str(config)], stdout=log, stderr=log)
                sub = None
                try:
                    ready = work / 'ready'
                    notice = work / 'notice'
                    # Both server and client validate actual test certificates.
                    args = [str(BIN / 'ota-mqtt-sub'), '--host', 'localhost', '--port', str(port),
                            '--topic', 'ota/GK1221-ADAS/device-test/campaign', '--client-id', 'app-ota-test',
                            '--ca', str(self.dir / 'ca.pem'), '--cert', str(self.dir / 'client.pem'),
                            '--key', str(self.dir / 'client.key'), '--output', str(notice),
                            '--continuous', '--ready-file', str(ready)]
                    deadline = time.monotonic() + 5
                    while time.monotonic() < deadline:
                        try:
                            with socket.create_connection(('127.0.0.1', port), timeout=0.1):
                                break
                        except OSError:
                            time.sleep(0.05)
                    sub = subprocess.Popen(args, stdout=log, stderr=log)
                    while not ready.exists() and sub.poll() is None and time.monotonic() < deadline:
                        time.sleep(0.05)
                    self.assertTrue(ready.exists(), (work / 'broker.log').read_text())
                    content = 'FORMAT_VERSION=1\nCAMPAIGN_ID=loopback-test\n'
                    publish = subprocess.run(['mosquitto_pub', '-h', 'localhost', '-p', str(port),
                                              '--cafile', str(self.dir / 'ca.pem'), '--cert', str(self.dir / 'client.pem'),
                                              '--key', str(self.dir / 'client.key'), '-t', 'ota/GK1221-ADAS/device-test/campaign',
                                              '-q', '1', '-r', '-m', content], capture_output=True, timeout=10)
                    self.assertEqual(publish.returncode, 0, publish.stderr)
                    deadline = time.monotonic() + 5
                    while not notice.exists() and time.monotonic() < deadline:
                        time.sleep(0.05)
                    self.assertTrue(notice.exists(), (work / 'broker.log').read_text())
                    fields = [line for line in notice.read_text().splitlines() if line]
                    self.assertEqual(fields, content.splitlines())
                    self.assertIsNone(sub.poll(), 'persistent subscriber unexpectedly exited')
                finally:
                    if sub and sub.poll() is None:
                        sub.terminate()
                        sub.wait(timeout=5)
                    broker.terminate()
                    broker.wait(timeout=5)
