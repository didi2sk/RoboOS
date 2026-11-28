"""Pilotný profil DC-09 pre TCP v privátnej sieti."""

from dataclasses import dataclass
import re

MAX_BODY = 0xFFF
HEADER = re.compile(
    rb'"(?P<token>\*?[A-Z0-9-]+)"(?P<sequence>[0-9]{4})'
    rb'(?P<route>(?:R[0-9A-Fa-f]{1,6})?L[0-9A-Fa-f]{1,6})'
    rb'#(?P<account>[0-9A-Fa-f]{3,16})\['
)
TIMESTAMP = rb'_[0-9]{2}:[0-9]{2}:[0-9]{2},[0-9]{2}-[0-9]{2}-[0-9]{4}'
EVENT = re.compile(
    rb'#(?P<account>[0-9A-Fa-f]{3,16})\|(?P<qualifier>[136])'
    rb'(?P<code>[0-9]{3}) (?P<partition>[0-9]{2}) (?P<number>[0-9]{3})\]'
    rb'(?:\[[^\[\]\r\n]*\])*(?:' + TIMESTAMP + rb')?'
)
NULL = re.compile(rb'\](?:' + TIMESTAMP + rb')?')


class InvalidFrame(ValueError):
    pass


def crc16(data):
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


def encode(body):
    return b'\n%04X%04X' % (crc16(body), len(body)) + body + b'\r'


def decode_header(prefix):
    if not re.fullmatch(rb'\n[0-9A-Fa-f]{4}0[0-9A-Fa-f]{3}', prefix):
        raise InvalidFrame('Neplatná hlavička rámca')
    return int(prefix[1:5], 16), int(prefix[5:9], 16)


@dataclass(frozen=True)
class Message:
    token: bytes
    sequence: bytes
    route: bytes
    account: bytes
    payload: bytes

    def reply(self, token):
        return encode(b'"%s"%s%s#%s[]' % (token, self.sequence, self.route, self.account))

    def contact_id(self):
        event = EVENT.fullmatch(self.payload)
        if self.sequence == b'0000' or not event:
            raise InvalidFrame('Neplatná udalosť Contact ID')
        if event['account'].upper() != self.account.upper():
            raise InvalidFrame('Rozdielne čísla objektu')
        return tuple(event[key].decode('ascii') for key in ('qualifier', 'code', 'partition', 'number'))


def parse(frame):
    crc, size = decode_header(frame[:9])
    body = frame[9:-1]
    if frame[-1:] != b'\r' or len(body) != size or crc16(body) != crc:
        raise InvalidFrame('Neplatná dĺžka, koniec rámca alebo CRC')
    match = HEADER.match(body)
    if not match:
        raise InvalidFrame('Neplatný identifikátor správy')
    return Message(*match.groups(), body[match.end():])
