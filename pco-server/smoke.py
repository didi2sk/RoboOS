"""Integračná skúška s reálnym TCP prijímačom a webom."""

import asyncio
import base64
from contextlib import closing
from http.server import ThreadingHTTPServer
from pathlib import Path
import re
from tempfile import TemporaryDirectory
from threading import Thread
from urllib.parse import urlencode
from urllib.request import Request, urlopen

from dc09 import encode, parse
from pco import Station, load_objects, tcp_handler, web_handler


async def check():
    published_example = b'\n87CD0037"ADM-CID"9876R579BDFL789ABC#12345A[#12345A|1110 00 129]\r'
    assert parse(published_example).contact_id() == ('1', '110', '00', '129')

    with TemporaryDirectory() as tmp:
        station = Station(f'{tmp}/test.db', load_objects(Path(__file__).with_name('objects.json')))
        server = await asyncio.start_server(tcp_handler(station), '127.0.0.1', 0)
        async with server:
            port = server.sockets[0].getsockname()[1]
            reader, writer = await asyncio.open_connection('127.0.0.1', port)

            async def response():
                return await asyncio.wait_for(reader.readuntil(b'\r'), 3)

            async def send(*frames):
                for frame in frames:
                    writer.write(frame)
                    await writer.drain()
                return await response()

            alarm = b'\n04700027"ADM-CID"0001L0#1001[#1001|1130 01 003]\r'
            expected = b'\n9EA80012"ACK"0001L0#1001[]\r'
            bad_crc = alarm[:1] + b'0000' + alarm[5:]
            unknown = encode(b'"NULL"0000L0#9999[]')
            writer.write(bad_crc + unknown + alarm[:7])
            await writer.drain()
            await asyncio.sleep(0.02)
            assert await send(alarm[7:]) == expected
            with closing(station.db()) as db:
                assert db.execute('SELECT count(*) FROM incidents').fetchone()[0] == 1

            # Druhé spojenie simuluje opakovanie po strate ACK a zmene trasy.
            writer.close()
            await writer.wait_closed()
            reader, writer = await asyncio.open_connection('127.0.0.1', port)
            assert await send(alarm) == expected

            for seq, cid in [(2, '3401 01 001'), (3, '1401 01 001'), (4, '1602 01 000'),
                             (5, '3130 01 003')]:
                assert parse(await send(encode(f'"ADM-CID"{seq:04}L0#1001[#1001|{cid}]'.encode()))).token == b'ACK'
            assert parse(await send(encode(b'"SIA-DCS"0006L0#1001[#1001|NBA003]'))).token == b'DUH'

            with closing(station.db()) as db, db:
                before = [row[0] for row in db.execute('SELECT label FROM events ORDER BY id')]
                assert before == ['Poplach · Zadné dvere', 'Zastrážené · užívateľ 001', 'Odstrážené · užívateľ 001',
                                  'Test prenosu', 'Návrat po poplachu · Zadné dvere'], before
                db.execute('UPDATE objects SET last_seen=1 WHERE account=?', ('1001',))
            station.supervise()
            station.supervise()
            assert parse(await send(b'\n9B9B0013"NULL"0000L0#1001[]\r')).token == b'ACK'
            writer.close()
            await writer.wait_closed()

        with ThreadingHTTPServer(('127.0.0.1', 0), web_handler(station, 'tester', 'test-password')) as web:
            worker = Thread(target=web.serve_forever)
            worker.start()
            try:
                url = f'http://127.0.0.1:{web.server_port}'
                headers = {'Authorization': 'Basic ' + base64.b64encode(b'tester:test-password').decode()}
                with urlopen(Request(url, headers=headers), timeout=3) as response:
                    page = response.read().decode()
                assert all(label in page for label in ['Zadné dvere', 'Výpadok spojenia', 'Spojenie obnovené'])
                csrf = re.search(r'name="csrf" value="([^"]+)"', page)[1]
                for action in ('take', 'close'):
                    form = urlencode({'id': 1, 'action': action, 'csrf': csrf}).encode()
                    with urlopen(Request(url + '/incident', form, headers), timeout=3) as response:
                        assert response.status == 200  # po presmerovaní na dispečing
            finally:
                web.shutdown()
                worker.join()

        restarted = Station(station.path, station.objects)
        with closing(restarted.db()) as db:
            assert db.execute('SELECT count(*) FROM incidents').fetchone()[0] == 2
            audit = db.execute('SELECT operator, action FROM incident_audit ORDER BY id').fetchall()
            assert [tuple(row) for row in audit] == [('tester', 'take'), ('tester', 'close')]
            assert db.execute('SELECT offline FROM objects').fetchone()[0] == 0


if __name__ == '__main__':
    asyncio.run(check())
    print('OK: DC-09/TCP, opakovanie, dohľad, dispečing, audit a opätovné otvorenie databázy')
