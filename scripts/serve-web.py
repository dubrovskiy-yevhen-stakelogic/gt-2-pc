"""Local-only browser preview. Only the built site is exposed, never the repo/discs."""
import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

class Handler(SimpleHTTPRequestHandler):
    extensions_map = {**SimpleHTTPRequestHandler.extensions_map, '.wasm':'application/wasm', '.js':'text/javascript'}
    def end_headers(self):
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        super().end_headers()
    def list_directory(self, path):
        self.send_error(403, 'Directory listing disabled')
        return None

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--directory', type=Path, default=Path('build_web/site'))
    parser.add_argument('--port', type=int, default=8080)
    args = parser.parse_args()
    folder=args.directory.resolve()
    for name in ['index.html','gt2.js','gt2.wasm']:
        if not (folder/name).is_file(): parser.error(f'Missing build output: {folder/name}')
    print(f'GT2 browser preview: http://127.0.0.1:{args.port}/', flush=True)
    ThreadingHTTPServer(('127.0.0.1',args.port),partial(Handler,directory=str(folder))).serve_forever()
