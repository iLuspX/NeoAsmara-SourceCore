"""Strict readers for the supplied client's MO object and ME event resources.

Positions and per-spawn object references are native data, not first sightings
in a network capture. Unknown object layouts fail rather than guessing a stride.
"""
import math
import struct


class Reader:
    def __init__(self, data):
        self.data, self.at = data, 0

    def take(self, size):
        if size < 0 or self.at + size > len(self.data):
            raise ValueError("Truncated native dungeon resource")
        result = self.data[self.at:self.at + size]
        self.at += size
        return result

    def u32(self):
        return struct.unpack('<I', self.take(4))[0]

    def count(self):
        value = self.u32()
        if value > 100000:
            raise ValueError("Unbounded native dungeon count")
        return value

    def f32(self):
        value = struct.unpack('<f', self.take(4))[0]
        if not math.isfinite(value):
            raise ValueError("Non-finite native event value")
        return value

    def string(self):
        size = self.u32()
        if size > 4096:
            raise ValueError("Invalid native object string length")
        return self.take(size).rstrip(b'\0').decode('utf-8', errors='strict')

    def finish(self):
        if self.at != len(self.data):
            raise ValueError("Unparsed native dungeon tail")


def objects(data):
    reader = Reader(data)
    models = [reader.string() for _ in range(reader.count())]
    result = []
    extra_sizes = {0: 0, 1: 8, 3: 12, 7: 12, 8: 0, 10: 0, 12: 4}
    for _ in range(reader.count()):
        raw = reader.take(116)
        kind = raw[0]
        if kind not in extra_sizes:
            raise ValueError(f"Unsupported native dungeon object type {kind}")
        model = reader.string()
        extra = reader.take(extra_sizes[kind])
        position = struct.unpack_from('<3f', raw, 84)
        if not all(math.isfinite(v) and abs(v) < 100000 for v in position):
            raise ValueError("Invalid native object position")
        result.append({'kind': kind, 'reference': struct.unpack_from('<I', raw, 4)[0],
                       'flags': list(raw[1:4]), 'position': list(position), 'model': model,
                       'extra': list(struct.unpack('<' + 'I' * (len(extra) // 4), extra))})
    reader.finish()
    return {'models': models, 'objects': result}


def events(data):
    reader = Reader(data)
    types = [struct.unpack('<HHI', reader.take(8)) for _ in range(reader.count())]

    def actions():
        result = []
        for _ in range(reader.count()):
            kind, target = struct.unpack('<HH', reader.take(4))
            result.append({'kind': kind, 'target': target, 'parameter': reader.u32()})
        return result

    timelines = []
    for _ in range(reader.count()):
        row = {'id': reader.u32(), 'start': reader.f32(), 'end': reader.f32(), 'frames': []}
        for _ in range(reader.count()):
            row['frames'].append({'time': reader.f32(), 'actions': actions()})
        timelines.append(row)
    triggers = []
    for _ in range(reader.count()):
        identity, mode = struct.unpack('<HH', reader.take(4))
        triggers.append({'id': identity, 'mode': mode, 'actions': actions()})
    reader.finish()
    return {'types': types, 'timelines': timelines, 'triggers': triggers}
