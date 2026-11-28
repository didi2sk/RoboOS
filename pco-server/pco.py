"""Prijímač alarmov a webový dispečing."""

import argparse
import asyncio
import base64
from contextlib import closing
from datetime import datetime
import hmac
from html import escape
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import logging
import os
from pathlib import Path
import re
import secrets
import sqlite3
from threading import Thread
import time
from urllib.parse import parse_qs
from zoneinfo import ZoneInfo

from dc09 import InvalidFrame, MAX_BODY, NULL, decode_header, parse

LOG = logging.getLogger('pco')
ZONE = ZoneInfo('Europe/Bratislava')
ALARM_CODES = {'110', '120', '130', '131', '132', '133', '134', '137', '140'}
RETRY_SECONDS = 180

SCHEMA = """
CREATE TABLE IF NOT EXISTS objects (
    account TEXT PRIMARY KEY, last_seen REAL, monitoring_since REAL NOT NULL,
    offline INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS events (
    id INTEGER PRIMARY KEY, received REAL NOT NULL, account TEXT NOT NULL,
    sequence TEXT, payload BLOB, qualifier TEXT, code TEXT,
    partition TEXT, number TEXT, label TEXT NOT NULL,
    FOREIGN KEY (account) REFERENCES objects(account)
);
CREATE INDEX IF NOT EXISTS event_retry ON events(account, sequence, received);
CREATE TABLE IF NOT EXISTS incidents (
    event_id INTEGER PRIMARY KEY, taken_at REAL, closed_at REAL,
    FOREIGN KEY (event_id) REFERENCES events(id)
);
CREATE TABLE IF NOT EXISTS incident_audit (
    id INTEGER PRIMARY KEY, event_id INTEGER NOT NULL,
    at REAL NOT NULL, operator TEXT NOT NULL, action TEXT NOT NULL,
    FOREIGN KEY (event_id) REFERENCES incidents(event_id)
);
"""


class Station:
    def __init__(self, path, objects):
        self.path = path
        self.objects = objects
        with closing(self.db()) as db, db:
            db.execute('PRAGMA journal_mode=WAL')
            db.executescript(SCHEMA)
            db.executemany('INSERT OR IGNORE INTO objects(account, monitoring_since) VALUES (?, ?)',
                           [(account, time.time()) for account in objects])

    def db(self):
        db = sqlite3.connect(self.path, timeout=3)
        db.row_factory = sqlite3.Row
        db.execute('PRAGMA foreign_keys=ON')
        db.execute('PRAGMA synchronous=FULL')
        return db

    def log_link(self, db, now, account, label, incident=False):
        row = db.execute('INSERT INTO events(received, account, label) VALUES (?, ?, ?)',
                         (now, account, label))
        if incident:
            db.execute('INSERT INTO incidents(event_id) VALUES (?)', (row.lastrowid,))

    def supervise(self):
        now = time.time()
        with closing(self.db()) as db, db:
            db.execute('BEGIN IMMEDIATE')
            for account, obj in self.objects.items():
                limit = obj.get('offline_after', 0)
                if not limit:
                    continue
                row = db.execute('SELECT * FROM objects WHERE account=?', (account,)).fetchone()
                since = row['last_seen'] or row['monitoring_since']
                if not row['offline'] and now - since >= limit:
                    db.execute('UPDATE objects SET offline=1 WHERE account=?', (account,))
                    self.log_link(db, now, account, 'Výpadok spojenia', incident=True)

    def receive(self, frame):
        msg = parse(frame)
        account = msg.account.decode('ascii').upper()
        if account not in self.objects:
            return None
        if msg.token == b'NULL':
            if msg.sequence != b'0000' or not NULL.fullmatch(msg.payload):
                return msg.reply(b'DUH')
            event = None
        elif msg.token == b'ADM-CID':
            try:
                event = msg.contact_id()
            except InvalidFrame:
                return msg.reply(b'DUH')
        else:
            return msg.reply(b'DUH')

        now = time.time()
        with closing(self.db()) as db, db:
            db.execute('BEGIN IMMEDIATE')
            row = db.execute('SELECT offline FROM objects WHERE account=?', (account,)).fetchone()
            db.execute('UPDATE objects SET last_seen=?, offline=0 WHERE account=?', (now, account))
            if row['offline']:
                self.log_link(db, now, account, 'Spojenie obnovené')
            if event:
                self.record(db, now, account, msg.sequence.decode('ascii'), msg.payload, event)
        return msg.reply(b'ACK')

    def record(self, db, now, account, sequence, payload, event):
        duplicate = db.execute(
            'SELECT 1 FROM events WHERE account=? AND sequence=? AND payload=? AND received>=?',
            (account, sequence, payload, now - RETRY_SECONDS)).fetchone()
        if duplicate:
            return
        qualifier, code, partition, number = event
        incident = qualifier == '1' and (code in ALARM_CODES or code == '461')
        zones = self.objects[account].get('zones', {})
        zone = zones.get(f'{partition}/{number}', zones.get(number, f'Zóna {number}'))
        if code == '401' and qualifier in ('1', '3'):
            label = 'Odstrážené' if qualifier == '1' else 'Zastrážené'
        elif code == '441' and qualifier in ('1', '3'):
            label = 'Odstrážené (doma)' if qualifier == '1' else 'Zastrážené doma'
        elif code in ('601', '602'):
            label = 'Test prenosu'
        elif code == '120' and qualifier in ('1', '3'):
            label = 'Panika' if incident else 'Návrat po panike'
        elif code in ALARM_CODES and qualifier in ('1', '3'):
            label = ('Poplach 24h' if code == '133' else 'Poplach') if incident else 'Návrat po poplachu'
            label += f' · {zone}'
        elif code == '406':
            label = 'Poplach zrušený'
        elif code == '461':
            label = 'Zablokovaný kód (veľa zlých pokusov)'
        elif code == '305':
            label = 'Zapnutie / reštart ústredne'
        elif code == '301':
            label = 'Výpadok napájania' if qualifier == '1' else 'Napájanie obnovené'
        else:
            label = 'Udalosť Contact ID'
        if code in ('401', '441', '406') and number != '000':
            label += f' · užívateľ {number}'
        row = db.execute(
            'INSERT INTO events(received, account, sequence, payload, qualifier, code, partition, number, label) '
            'VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)',
            (now, account, sequence, payload, *event, label))
        if incident:
            db.execute('INSERT INTO incidents(event_id) VALUES (?)', (row.lastrowid,))

    def operate(self, event_id, action, operator):
        now = time.time()
        with closing(self.db()) as db, db:
            if action == 'take':
                sql = 'UPDATE incidents SET taken_at=? WHERE event_id=? AND taken_at IS NULL'
            else:
                sql = 'UPDATE incidents SET closed_at=? WHERE event_id=? AND taken_at IS NOT NULL AND closed_at IS NULL'
            if not db.execute(sql, (now, event_id)).rowcount:
                return False
            db.execute('INSERT INTO incident_audit(event_id, at, operator, action) VALUES (?, ?, ?, ?)',
                       (event_id, now, operator, action))
        return True


def tcp_handler(station):
    slots = asyncio.Semaphore(32)

    async def handle(reader, writer):
        try:
            if slots.locked():
                return
            async with slots:
                while True:
                    header = await asyncio.wait_for(reader.readexactly(9), timeout=30)
                    _, size = decode_header(header)
                    tail = await asyncio.wait_for(reader.readexactly(size + 1), timeout=5)
                    try:
                        reply = await asyncio.to_thread(station.receive, header + tail)
                    except InvalidFrame:
                        continue
                    if reply:
                        writer.write(reply)
                        await asyncio.wait_for(writer.drain(), timeout=5)
        except (TimeoutError, asyncio.IncompleteReadError, ConnectionError, InvalidFrame):
            pass
        except sqlite3.Error:
            LOG.exception('Zápis do databázy zlyhal')
        finally:
            writer.close()

    return handle


async def run_receiver(station, host, port):
    server = await asyncio.start_server(tcp_handler(station), host, port, limit=MAX_BODY + 10)
    LOG.info('DC-09 TCP počúva na %s:%d', host, port)
    async with server:
        while True:
            try:
                await asyncio.to_thread(station.supervise)
            except sqlite3.Error:
                LOG.exception('Dohľad zlyhal')
            await asyncio.sleep(2)


def local_time(value):
    return datetime.fromtimestamp(value, ZONE).strftime('%d.%m.%Y %H:%M:%S') if value else '—'


def dashboard(station, csrf):
    with closing(station.db()) as db, db:
        db.execute('BEGIN')
        states = db.execute('SELECT * FROM objects ORDER BY account').fetchall()
        incidents = db.execute(
            'SELECT e.*, i.taken_at FROM incidents i JOIN events e ON e.id=i.event_id '
            'WHERE i.closed_at IS NULL ORDER BY e.id DESC').fetchall()
        history = db.execute(
            'SELECT e.*, i.taken_at, i.closed_at FROM events e '
            'LEFT JOIN incidents i ON i.event_id=e.id ORDER BY e.id DESC LIMIT 100').fetchall()

    def object_name(account):
        name = station.objects.get(account, {}).get('name', account)
        return escape(f'{account} · {name}')

    def link_status(row):
        if not station.objects[row['account']].get('offline_after'):
            return 'Bez dohľadu'
        if row['offline']:
            return 'Výpadok'
        return 'Dostupný' if row['last_seen'] else 'Čaká na správu'

    objects = ''.join(
        f'<li><strong>{object_name(row["account"])}</strong> '
        f'<span>{link_status(row)}</span>'
        f' <time>{local_time(row["last_seen"])}</time></li>'
        for row in states if row['account'] in station.objects)
    active = []
    for row in incidents:
        action, label = ('close', 'Uzavrieť') if row['taken_at'] else ('take', 'Prevziať')
        active.append(f'<li><div><strong>{escape(row["label"])}</strong><br>'
                      f'{object_name(row["account"])} · {local_time(row["received"])}</div>'
                      f'<form method="post" action="/incident"><input type="hidden" name="csrf" value="{csrf}">'
                      f'<input type="hidden" name="id" value="{row["id"]}">'
                      f'<button name="action" value="{action}">{label}</button></form></li>')
    records = []
    for row in history:
        cid = ' '.join(row[k] or '—' for k in ('qualifier', 'code', 'partition', 'number'))
        status = ('Uzavretý' if row['closed_at'] else 'Prevzatý' if row['taken_at'] else '')
        records.append(f'<tr><td>{local_time(row["received"])}</td><td>{object_name(row["account"])}</td>'
                       f'<td>{escape(row["label"])}</td><td><code>{cid}</code></td><td>{status}</td></tr>')

    return f'''<!doctype html><html lang="sk"><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>Robo PCO</title>
<style>
:root {{ color-scheme:dark; font:16px system-ui; background:#10151d; color:#eaf0f6 }}
body {{ margin:0 auto; max-width:1120px; padding:1.5rem }}
header, li {{ display:flex; align-items:center; justify-content:space-between; gap:1rem }}
section {{ background:#18212c; border:1px solid #2b3a49; border-radius:12px; margin:1rem 0; padding:.8rem 1rem; overflow:auto }}
h1,h2 {{ margin:.5rem 0 }} ul {{ padding:0; margin:0; list-style:none }}
li, td, th {{ padding:.75rem .4rem; border-bottom:1px solid #2b3a49; text-align:left }}
table {{ width:100%; border-collapse:collapse; min-width:640px }} time {{ color:#b1c2d4 }}
a,button {{ background:#344f71; color:#fff; border:0; border-radius:7px; padding:.55rem .8rem; text-decoration:none; font:inherit }}
button {{ cursor:pointer }} .active strong {{ color:#ffb5a5 }}
</style>
<header><h1>Robo PCO</h1><a href="/">Obnoviť</a></header>
<section class="active"><h2>Otvorené incidenty</h2><ul>{''.join(active) or '<li>Žiadne otvorené incidenty</li>'}</ul></section>
<section><h2>Spojenie s objektmi</h2><ul>{objects}</ul></section>
<section><h2>História udalostí</h2><table><thead><tr><th>Prijaté</th><th>Objekt</th>
<th>Udalosť</th><th>Contact ID</th><th>Incident</th></tr></thead><tbody>{''.join(records)}</tbody></table></section>
<script>
let refresh = setInterval(() => {{
  if (!document.hidden && document.activeElement.tagName !== 'BUTTON') location.reload();
}}, 8000);
document.querySelectorAll('form').forEach(form =>
  form.addEventListener('submit', () => clearInterval(refresh)));
</script></html>'''


def web_handler(station, operator, password):
    auth = 'Basic ' + base64.b64encode(f'{operator}:{password}'.encode()).decode()
    csrf = secrets.token_urlsafe(32)

    class Handler(BaseHTTPRequestHandler):
        def setup(self):
            self.request.settimeout(10)
            super().setup()

        def authenticated(self):
            supplied = self.headers.get('Authorization', '').encode('utf-8')
            if hmac.compare_digest(supplied, auth.encode('ascii')):
                return True
            self.send_response(401)
            self.send_header('WWW-Authenticate', 'Basic realm="Robo PCO", charset="UTF-8"')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return False

        def do_GET(self):
            if not self.authenticated():
                return
            if self.path != '/':
                self.send_error(404)
                return
            try:
                body = dashboard(station, csrf).encode()
            except sqlite3.Error:
                LOG.exception('Dispečing nevie načítať údaje')
                self.send_error(503)
                return
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Frame-Options', 'DENY')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):
            if not self.authenticated():
                return
            if self.path != '/incident':
                self.send_error(404)
                return
            try:
                size = int(self.headers.get('Content-Length', 0))
                if not 0 < size <= 1024:
                    raise ValueError
                values = parse_qs(self.rfile.read(size).decode('ascii'), max_num_fields=3)
                event_id = int(values['id'][0])
                action = values['action'][0]
                if action not in ('take', 'close') or not 0 < event_id < 2**63:
                    raise ValueError
                if not hmac.compare_digest(values['csrf'][0].encode(), csrf.encode()):
                    self.send_error(403)
                    return
            except (ValueError, KeyError):
                self.send_error(400)
                return
            try:
                changed = station.operate(event_id, action, operator)
            except sqlite3.Error:
                LOG.exception('Zmena incidentu zlyhala')
                self.send_error(503)
                return
            if not changed:
                self.send_error(409)
                return
            self.send_response(303)
            self.send_header('Location', '/')
            self.end_headers()

    return Handler


def load_objects(path):
    data = json.loads(Path(path).read_text(encoding='utf-8'))
    objects = {}
    for raw_account, obj in data['objects'].items():
        account = raw_account.upper()
        if not re.fullmatch('[0-9A-F]{3,16}', account) or account in objects:
            raise ValueError(f'Neplatné alebo duplicitné číslo objektu {raw_account}')
        if not isinstance(obj.get('name'), str) or not obj['name'].strip():
            raise ValueError(f'Objekt {account} musí mať názov')
        limit = obj.get('offline_after', 0)
        if type(limit) is not int or not (limit == 0 or 10 <= limit <= 64800):
            raise ValueError(f'Objekt {account} má neplatný čas dohľadu')
        zones = obj.get('zones', {})
        if not isinstance(zones, dict) or any(
            not re.fullmatch(r'(?:[0-9]{2}/)?[0-9]{3}', key) or not isinstance(value, str)
            for key, value in zones.items()
        ):
            raise ValueError(f'Objekt {account} má neplatné mapovanie zón')
        objects[account] = obj
    if not objects:
        raise ValueError('Pridaj aspoň jeden objekt')
    return objects


def main():
    parser = argparse.ArgumentParser(description='Robo PCO')
    parser.add_argument('--config', default='objects.json')
    parser.add_argument('--db', default='pco.sqlite3')
    parser.add_argument('--listen', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=5000)
    parser.add_argument('--web-port', type=int, default=8080)
    args = parser.parse_args()
    operator = os.environ.get('PCO_OPERATOR', 'operator')
    password = os.environ.get('PCO_PASSWORD', '')
    if len(password) < 12 or not operator or ':' in operator:
        parser.error('Nastav PCO_PASSWORD (aspoň 12 znakov) a platný PCO_OPERATOR')
    logging.basicConfig(level=logging.INFO, format='%(asctime)s %(levelname)s %(message)s')
    os.umask(0o077)
    try:
        station = Station(args.db, load_objects(args.config))
    except (OSError, ValueError, AttributeError, TypeError, KeyError, sqlite3.Error) as exc:
        parser.error(str(exc))
    with ThreadingHTTPServer(('127.0.0.1', args.web_port), web_handler(station, operator, password)) as web:
        thread = Thread(target=web.serve_forever, daemon=True)
        thread.start()
        LOG.info('Dispečing na http://127.0.0.1:%d', args.web_port)
        try:
            asyncio.run(run_receiver(station, args.listen, args.port))
        except KeyboardInterrupt:
            pass
        finally:
            web.shutdown()
            thread.join()


if __name__ == '__main__':
    main()
