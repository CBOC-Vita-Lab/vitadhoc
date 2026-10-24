"""Exercise the real worker and HTTP parser against synthetic files and a local server."""
import base64
import contextlib
import http.server
import json
import os
import struct
from pathlib import Path
import subprocess
import tempfile
import threading
import unittest
import urllib.parse
import zlib

HERE = Path(__file__).resolve().parent
VITA = Path(os.environ.get('AF_SOURCE', HERE.parents[1]))
PNG = base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aL1kAAAAASUVORK5CYII=')
GBA = '/Nintendo - Game Boy Advance/'
GBC = '/Nintendo - Game Boy Color/'


def solid_png(width, height, rgb):
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data))

    pixels = (b'\0' + bytes(rgb) * width) * height
    return (b'\x89PNG\r\n\x1a\n' +
            chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(pixels)) + chunk(b'IEND', b''))


@contextlib.contextmanager
def server(routes):
    requests = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            path = urllib.parse.unquote(self.path)
            requests.append(path)
            result = routes.get(path, (404, b'not found'))
            if callable(result):
                result = result()
            status, body = result
            self.send_response(status)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *_):
            pass

    httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    try:
        yield f'http://127.0.0.1:{httpd.server_port}', requests
    finally:
        httpd.shutdown()
        httpd.server_close()
        thread.join()


def compile_runner(out, sanitize=False, legacy=False):
    cc = os.environ.get('CC', 'cc')
    flags = ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
             '-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=0',
             '-I', str(VITA), '-I', str(VITA.parent / 'psp')]
    if sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    if legacy:
        flags += ['-DAF_LEGACY_WORKER']
    objects = []
    for name in ['artfetch', 'gamedb']:
        obj = out / (name + '.o')
        subprocess.run([cc, *flags, '-Dfopen=af_fopen', '-Dfread=af_fread',
                        '-Dfclose=af_fclose', '-Dconnect=af_connect',
                        '-include', str(HERE / 'runner_io.h'),
                        '-c', str(VITA / (name + '.c')), '-o', str(obj)], check=True)
        objects.append(str(obj))
    binary = out / 'artfetch_runner'
    subprocess.run([cc, *flags, str(HERE / 'runner.c'),
                    str(VITA.parent / 'psp/stb_impl.c'), *objects,
                    '-pthread', '-lm', '-o', str(binary)], check=True)
    return binary


class ArtworkWorkerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='artfetch-build-')
        cls.runner = compile_runner(Path(cls.build.name), bool(os.environ.get('AF_SANITIZE')))

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='artfetch-test-')
        self.root = Path(self.temp.name)
        for folder in ['boxart', 'titles', 'roms']:
            (self.root / folder).mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def run_worker(self, names, routes, titles=True, mode='all', size=0, delay=0,
                   paths=None, timeout_ms=15000, offline=False):
        listing = self.root / 'list.txt'
        for name in names:
            rom = self.root / 'roms' / name
            if not rom.exists():
                with rom.open('wb') as file:
                    file.truncate(size)
        listing.write_text(''.join(
            f'{name}\t{paths.get(name, self.root / "roms" / name) if paths else self.root / "roms" / name}\n'
            for name in names))
        with server(routes) as (url, requests):
            process = subprocess.run([str(self.runner), str(self.root), url, str(listing),
                                      str(int(titles)), mode, str(timeout_ms)],
                                     capture_output=True, text=True, timeout=20,
                                     env={**os.environ, 'AF_IO_DELAY_US': str(delay),
                                          **({'AF_OFFLINE': '1'} if offline else {})})
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        self.assertNotIn('ERROR: AddressSanitizer', process.stderr)
        self.assertNotIn('runtime error:', process.stderr)
        stats = json.loads(process.stdout)
        if mode.startswith('hold'):
            print('STRESS ' + json.dumps({'mode': mode, 'stats': stats,
                  'requests_observed': len(requests),
                  'covers_on_disk': len(list((self.root / 'boxart').glob('*.png'))),
                  'thumbnails_on_disk': len(list((self.root / 'boxart').glob('*.t2.565')))}), flush=True)
        return stats, requests

    def routes(self, name, system=GBA):
        stem = Path(name).stem
        return {system + kind + '/' + stem + '.png': (200, PNG)
                for kind in ['Named_Boxarts', 'Named_Titles']}

    def test_uppercase_gba_is_routed_to_gba(self):
        name = 'Library Example.GBA'
        stats, requests = self.run_worker([name], self.routes(name))
        self.assertTrue((self.root / 'boxart' / 'Library Example.png').is_file(), requests)
        self.assertEqual((self.root / 'boxart' / 'Library Example.png').read_bytes(), PNG)
        self.assertEqual(stats['have'], 1)
        self.assertTrue(all(path.startswith(GBA) for path in requests))

    def test_uppercase_gbc_tries_color_before_monochrome(self):
        name = 'Color Example.GBC'
        _, requests = self.run_worker([name], self.routes(name, GBC))
        self.assertTrue(requests[0].startswith(GBC))
        self.assertEqual((self.root / 'titles' / 'Color Example.png').read_bytes(), PNG)

    def test_first_cover_does_not_wait_for_thousands_of_negative_opens(self):
        names = [f'Library {i:04}.gba' for i in range(900)]
        stats, _ = self.run_worker(names, self.routes(names[0]), titles=False, mode='first', delay=100)
        self.assertLess(stats['first_art_opens'], 30, stats)
        self.assertLess(stats['first_art_stats'], 30, stats)

    def test_download_writes_a_readable_thumbnail(self):
        name = 'Thumbnail Example.gba'
        self.run_worker([name], self.routes(name), titles=False)
        thumb = (self.root / 'boxart' / 'Thumbnail Example.t2.565').read_bytes()
        self.assertEqual(struct.unpack_from('<IHHH', thumb), (0x52353635, 1, 1, 1))
        self.assertEqual(len(thumb), 18)

    def test_existing_cover_warms_offline_and_notifies_the_ui(self):
        name = 'Offline Example.gba'
        (self.root / 'boxart' / 'Offline Example.png').write_bytes(PNG)
        stats, requests = self.run_worker([name], {}, offline=True, timeout_ms=700)
        self.assertEqual(requests, [])
        self.assertTrue((self.root / 'boxart' / 'Offline Example.t2.565').is_file())
        self.assertGreaterEqual(stats['done'], 1, stats)

    def test_thumbnail_channels_match_the_vita_texture_loader(self):
        for name, rgb, packed in [('Red', (255, 0, 0), 0x001F),
                                   ('Blue', (0, 0, 255), 0xF800)]:
            with self.subTest(colour=name):
                route = GBA + 'Named_Boxarts/' + name + '.png'
                self.run_worker([name + '.gba'], {route: (200, solid_png(1, 1, rgb))},
                                titles=False)
                thumb = (self.root / 'boxart' / (name + '.t2.565')).read_bytes()
                self.assertEqual(struct.unpack_from('<H', thumb, 16)[0], packed)

    def test_thumbnail_fits_its_box_and_preserves_original_cover(self):
        name = 'Wide Cover.gba'
        payload = solid_png(320, 160, (255, 0, 0))
        self.run_worker([name], {GBA + 'Named_Boxarts/Wide Cover.png': (200, payload)},
                        titles=False)
        thumb = (self.root / 'boxart' / 'Wide Cover.t2.565').read_bytes()
        self.assertEqual(struct.unpack_from('<IHHH', thumb), (0x52353635, 160, 80, 160))
        self.assertEqual(len(thumb), 16 + 160 * 80 * 2)
        self.assertEqual((self.root / 'boxart' / 'Wide Cover.png').read_bytes(), payload)

    def test_old_reversed_channel_cache_is_rebuilt_without_deleting_it(self):
        (self.root / 'boxart' / 'Old Thumbnail.png').write_bytes(solid_png(1, 1, (255, 0, 0)))
        old = self.root / 'boxart' / 'Old Thumbnail.t565'
        old.write_bytes(b'old cached pixels')
        self.run_worker(['Old Thumbnail.gba'], {}, offline=True, timeout_ms=700)
        thumb = (self.root / 'boxart' / 'Old Thumbnail.t2.565').read_bytes()
        self.assertEqual(struct.unpack_from('<H', thumb, 16)[0], 0x001F)
        self.assertEqual(old.read_bytes(), b'old cached pixels')

    def test_oversized_thumbnail_decode_is_skipped_without_losing_the_cover(self):
        payload = solid_png(1601, 1600, (255, 0, 0))
        self.run_worker(['Huge.gba'], {GBA + 'Named_Boxarts/Huge.png': (200, payload)},
                        titles=False)
        self.assertFalse((self.root / 'boxart' / 'Huge.t2.565').exists())
        self.assertEqual((self.root / 'boxart' / 'Huge.png').read_bytes(), payload)

    def test_cover_that_cannot_be_thumbnailed_is_not_read_again(self):
        (self.root / 'boxart' / 'Broken.png').write_bytes(b'\x89PNG\r\n\x1a\nnot really')
        (self.root / 'boxart' / 'Good.png').write_bytes(PNG)
        stats, requests = self.run_worker(['Broken.gba', 'Good.gba'], {}, offline=True,
                                          mode='relist', timeout_ms=3000)
        self.assertTrue(stats['command_sent'], stats)
        self.assertTrue((self.root / 'boxart' / 'Good.t2.565').is_file())
        self.assertFalse((self.root / 'boxart' / 'Broken.t2.565').exists())
        self.assertEqual((self.root / 'boxart' / 'Broken.png').read_bytes()[:8], b'\x89PNG\r\n\x1a\n')
        self.assertEqual(stats['relist_opens'], 0, stats)

    def test_server_address_is_looked_up_once_per_pass(self):
        names = ['Lookup %d.gba' % i for i in range(6)]
        routes = {}
        for name in names:
            routes.update(self.routes(name))
        stats, requests = self.run_worker(names, routes)
        self.assertGreaterEqual(len(requests), 6, stats)
        self.assertEqual(stats['resolves'], 1, stats)

    def test_downloaded_png_that_does_not_decode_is_not_kept(self):
        name = 'Truncated.gba'
        cut = solid_png(40, 40, (1, 2, 3))[:60]       # a PNG's head, cut short
        routes = {GBA + 'Named_Boxarts/Truncated.png': (200, cut)}
        stats, _ = self.run_worker([name], routes, titles=False)
        self.assertFalse((self.root / 'boxart' / 'Truncated.png').exists())
        miss = self.root / 'boxart' / '.missing.txt'
        self.assertFalse(miss.exists() and 'Truncated' in miss.read_text())

    def test_exact_filename_success_does_not_hash_entire_file(self):
        name = 'Filename Example.gba'
        stats, _ = self.run_worker([name], self.routes(name), size=1024 * 1024)
        self.assertLessEqual(stats['rom_bytes'], 4, stats)
        self.assertEqual(stats['have'], 1)

    def test_crc_fallback_reads_file_only_once_for_box_and_title(self):
        stats, _ = self.run_worker(['Unknown.gba'], {}, size=1024 * 1024)
        self.assertLessEqual(stats['rom_bytes'], 1024 * 1024 + 4, stats)

    def test_old_negative_cache_does_not_keep_misrouted_game_missing(self):
        for folder in ['boxart', 'titles']:
            (self.root / folder / '.missing.txt').write_text('Old Miss\n')
        name = 'Old Miss.GBA'
        stats, requests = self.run_worker([name], self.routes(name))
        self.assertTrue(requests)
        self.assertEqual(stats['have'], 1)
        self.assertEqual((self.root / 'titles' / 'Old Miss.png').read_bytes(), PNG)

    def test_existing_user_art_and_save_are_not_overwritten(self):
        art = self.root / 'boxart' / 'Keep Me.jpg'
        art.write_bytes(b'owner supplied art')
        save = self.root / 'roms' / 'Keep Me.sav'
        save.write_bytes(b'owner save bytes')
        stats, requests = self.run_worker(['Keep Me.gba'], {}, titles=False)
        self.assertEqual(stats['have'], 1)
        self.assertEqual(requests, [])
        self.assertEqual(art.read_bytes(), b'owner supplied art')
        self.assertEqual(save.read_bytes(), b'owner save bytes')

    def test_server_errors_and_bad_png_are_not_permanent_misses(self):
        for status, body in [(500, b'failed'), (200, b'<html>portal</html>')]:
            with self.subTest(status=status):
                name = f'Error {status}.gba'
                routes = {path: (status, body) for path in self.routes(name)}
                self.run_worker([name], routes)
                for folder in ['boxart', 'titles']:
                    miss = self.root / folder / '.missing.txt'
                    self.assertFalse(miss.exists() and Path(name).stem in miss.read_text())

    def test_true_missing_is_cached_and_explicit_retry_refetches(self):
        name = 'Retry Example.gba'
        self.run_worker([name], {}, titles=False)
        stats, requests = self.run_worker([name], self.routes(name), titles=False)
        self.assertEqual(requests, [])
        self.assertEqual(stats['have'], 0)
        stats, requests = self.run_worker([name], self.routes(name), titles=False, mode='retry')
        self.assertTrue(requests)
        self.assertEqual(stats['have'], 1)

    def test_large_current_miss_cache_does_not_probe_every_art_extension(self):
        names = [f'Cached {i:04}.gba' for i in range(900)]
        (self.root / 'boxart' / '.missing.txt').write_text(
            '# VitAdhoc artwork misses v2\n' + ''.join(f'{Path(name).stem}\n' for name in names))
        stats, requests = self.run_worker(names, {}, titles=False)
        self.assertEqual(requests, [])
        self.assertEqual(stats['have'], 0)
        self.assertLess(stats['art_opens'], 30, stats)

    def test_user_art_added_during_response_is_preserved(self):
        art = self.root / 'boxart' / 'Arrived.jpg'

        def response():
            art.write_bytes(b'new owner artwork')
            return 200, PNG

        routes = {GBA + 'Named_Boxarts/Arrived.png': response}
        stats, _ = self.run_worker(['Arrived.gba'], routes, titles=False)
        self.assertEqual(stats['have'], 1)
        self.assertEqual(art.read_bytes(), b'new owner artwork')
        self.assertFalse((self.root / 'boxart' / 'Arrived.png').exists())

    def test_directory_inventory_counts_duplicate_art_extensions_once(self):
        names = [f'Existing {i}.gba' for i in range(3)]
        for name in names:
            for ext in ['png', 'jpg']:
                (self.root / 'boxart' / (Path(name).stem + '.' + ext)).write_bytes(PNG)
        stats, requests = self.run_worker(names, {}, titles=False)
        self.assertEqual((stats['have'], requests), (3, []))

    def test_recognized_header_is_still_preferred_for_renamed_file(self):
        payload = bytearray(1024)
        payload[0xAC:0xB0] = b'ABCD'
        (self.root / 'roms' / 'Renamed.gba').write_bytes(payload)
        (self.root / 'gamedb.txt').write_text('SABCD\tHeader Match\nZABCD\t1024\n')
        stats, requests = self.run_worker(['Renamed.gba'], self.routes('Header Match.gba'))
        self.assertTrue(all(path.endswith('/Header Match.png') for path in requests))
        self.assertEqual(stats['rom_bytes'], 4)
        self.assertEqual((self.root / 'boxart' / 'Renamed.png').read_bytes(), PNG)

    def test_crc_fallback_finds_renamed_file_for_both_art_kinds(self):
        payload = bytes(1024 * 1024)
        (self.root / 'roms' / 'Renamed.gba').write_bytes(payload)
        crc = zlib.crc32(payload)
        (self.root / 'gamedb.txt').write_text(f'C{crc:08X}\tCRC Match\n')
        stats, requests = self.run_worker(['Renamed.gba'], self.routes('CRC Match.gba'))
        self.assertTrue(any(path.endswith('/CRC Match.png') for path in requests))
        self.assertEqual(stats['rom_bytes'], len(payload) + 4)
        self.assertEqual((self.root / 'titles' / 'Renamed.png').read_bytes(), PNG)

    def test_unavailable_local_roms_do_not_trigger_network_backoff(self):
        unavailable = [f'Unavailable {i}.gba' for i in range(3)]
        valid = 'Ready Example.gba'
        stats, requests = self.run_worker(
            unavailable + [valid], self.routes(valid),
            paths={name: '' for name in unavailable}, timeout_ms=1500)
        self.assertEqual(stats['seen'], 4, stats)
        self.assertEqual((self.root / 'boxart' / 'Ready Example.png').read_bytes(), PNG)
        self.assertEqual((self.root / 'titles' / 'Ready Example.png').read_bytes(), PNG)
        self.assertEqual(stats['have'], 1, requests)

    def test_unavailable_identity_is_not_cached_as_a_permanent_miss(self):
        name = 'Temporarily Unavailable.gba'
        self.run_worker([name], {}, paths={name: ''})
        for folder in ['boxart', 'titles']:
            missing = self.root / folder / '.missing.txt'
            self.assertFalse(missing.exists() and Path(name).stem in missing.read_text())
        payload = bytes(1024)
        (self.root / 'roms' / name).write_bytes(payload)
        crc = zlib.crc32(payload)
        (self.root / 'gamedb.txt').write_text(f'C{crc:08X}\tRecovered Identity\n')
        stats, _ = self.run_worker([name], self.routes('Recovered Identity.gba'))
        self.assertEqual(stats['have'], 1)
        self.assertEqual((self.root / 'titles' / (Path(name).stem + '.png')).read_bytes(), PNG)

    def test_slow_consumer_does_not_stall_cover_downloads(self):
        names = [f'Batch {i:02}.gba' for i in range(40)]
        routes = {GBA + 'Named_Boxarts/' + Path(name).stem + '.png': (200, PNG)
                  for name in names}
        stats, requests = self.run_worker(names, routes, titles=False, mode='hold', timeout_ms=2000)
        self.assertEqual(stats['seen'], 40, stats)
        self.assertEqual(len(list((self.root / 'boxart').glob('*.png'))), 40, requests)
        self.assertEqual(len(list((self.root / 'boxart').glob('*.t2.565'))), 40)
        self.assertEqual((stats['resync'], stats['resync_cleared']), (1, 1), stats)
        self.assertLess(stats['stop_us'], 750000, stats)

    def test_missing_cover_retry_is_serviced_without_draining_notifications(self):
        names = [f'Retry Batch {i:02}.gba' for i in range(40)]
        (self.root / 'boxart' / '.missing.txt').write_text(
            '# VitAdhoc artwork misses v2\n' +
            ''.join(Path(name).stem + '\n' for name in names[-3:]))
        routes = {GBA + 'Named_Boxarts/' + Path(name).stem + '.png': (200, PNG)
                  for name in names}
        stats, _ = self.run_worker(names, routes, titles=False, mode='hold_retry', timeout_ms=2500)
        self.assertEqual(stats['command_sent'], 1, stats)
        self.assertEqual(stats['retry_seen'], 1, stats)
        self.assertEqual(len(list((self.root / 'boxart').glob('*.png'))), 40, stats)

    def test_new_list_supersedes_backpressured_work(self):
        names = [f'Replace Batch {i:02}.gba' for i in range(40)]
        routes = {GBA + 'Named_Boxarts/' + Path(name).stem + '.png': (200, PNG)
                  for name in names}
        stats, _ = self.run_worker(names, routes, titles=False, mode='hold_replace', timeout_ms=2000)
        self.assertEqual(stats['command_sent'], 1, stats)
        self.assertEqual((stats['total'], stats['seen']), (1, 1), stats)
        self.assertEqual((self.root / 'boxart' / 'Replace Batch 39.png').read_bytes(), PNG)

    def test_offline_thumbnail_batch_survives_a_slow_consumer(self):
        names = [f'Offline Batch {i:02}.gba' for i in range(40)]
        for name in names:
            stem = Path(name).stem
            (self.root / 'boxart' / (stem + '.png')).write_bytes(PNG)
            (self.root / 'boxart' / (stem + '.t565')).write_bytes(b'old cache')
        stats, requests = self.run_worker(names, {}, titles=False, mode='hold',
                                          offline=True, timeout_ms=1000)
        self.assertEqual(requests, [])
        self.assertEqual(len(list((self.root / 'boxart').glob('*.t2.565'))), 40)
        self.assertEqual((stats['resync'], stats['resync_cleared']), (1, 1), stats)
        for name in names:
            stem = Path(name).stem
            self.assertEqual((self.root / 'boxart' / (stem + '.png')).read_bytes(), PNG)
            self.assertEqual((self.root / 'boxart' / (stem + '.t565')).read_bytes(), b'old cache')

    def test_paused_batch_quiesces_card_io_and_resumes(self):
        names = [f'Pause Batch {i:02}.gba' for i in range(40)]
        routes = {GBA + 'Named_Boxarts/' + Path(name).stem + '.png': (200, PNG)
                  for name in names}
        stats, _ = self.run_worker(names, routes, titles=False, mode='hold_pause',
                                   delay=2000, timeout_ms=5000)
        self.assertEqual(stats['command_sent'], 1, stats)
        self.assertEqual(stats['pause_quiet'], 1, stats)
        self.assertEqual(stats['seen'], 40, stats)


if __name__ == '__main__':
    unittest.main(verbosity=2)
