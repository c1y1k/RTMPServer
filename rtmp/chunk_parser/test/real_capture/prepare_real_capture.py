#!/usr/bin/env python3

import argparse
import json
import struct
from pathlib import Path


class Pairs(list):
    """JSON object that preserves field order and duplicate keys."""


def object_pairs_hook(pairs):
    return Pairs(pairs)


def first(obj, key, default=None):
    if not isinstance(obj, Pairs):
        return default
    for current_key, value in obj:
        if current_key == key:
            return value
    return default


def all_values(obj, key):
    if not isinstance(obj, Pairs):
        return []
    return [value for current_key, value in obj if current_key == key]


def parse_integer(value):
    if value is None:
        return "-"
    text = str(value)
    return str(int(text, 0))


def extract_wireshark_headers(json_path, output_path):
    with json_path.open("r", encoding="utf-8") as source:
        packets = json.load(source, object_pairs_hook=object_pairs_hook)

    rows = []
    for packet in packets:
        source = first(packet, "_source")
        layers = first(source, "layers")
        frame = first(first(layers, "frame"), "frame.number", "-")
        for rtmpt in all_values(layers, "rtmpt"):
            for header in all_values(rtmpt, "RTMP Header"):
                rows.append(
                    [
                        str(frame),
                        parse_integer(first(header, "rtmpt.header.format")),
                        parse_integer(first(header, "rtmpt.header.csid")),
                        parse_integer(first(header, "rtmpt.header.timestamp")),
                        parse_integer(first(header, "rtmpt.header.timestampdelta")),
                        parse_integer(first(header, "rtmpt.header.bodysize")),
                        parse_integer(first(header, "rtmpt.header.typeid")),
                        parse_integer(first(header, "rtmpt.header.streamid")),
                    ]
                )

    if not rows:
        raise RuntimeError("Wireshark JSON contains no RTMP Header fields")

    with output_path.open("w", encoding="utf-8") as output:
        output.write("frame\tfmt\tcsid\ttimestamp\ttimestamp_delta\t"
                     "message_length\ttype_id\tstream_id\n")
        for row in rows:
            output.write("\t".join(row) + "\n")
    return len(rows)


def read_pcapng(path):
    data = path.read_bytes()
    offset = 0
    endian = None
    interfaces = []
    frame_number = 0

    while offset < len(data):
        if len(data) - offset < 12:
            raise RuntimeError("truncated pcapng block header")
        block_type_bytes = data[offset:offset + 4]
        if block_type_bytes == b"\x0a\x0d\x0d\x0a":
            byte_order_magic = data[offset + 8:offset + 12]
            if byte_order_magic == b"\x4d\x3c\x2b\x1a":
                endian = "<"
            elif byte_order_magic == b"\x1a\x2b\x3c\x4d":
                endian = ">"
            else:
                raise RuntimeError("invalid pcapng byte-order magic")
            interfaces = []
        elif endian is None:
            raise RuntimeError("pcapng does not begin with a section header")

        block_type, block_length = struct.unpack(
            endian + "II", data[offset:offset + 8]
        )
        if block_length < 12 or offset + block_length > len(data):
            raise RuntimeError("invalid pcapng block length")
        trailing_length = struct.unpack(
            endian + "I", data[offset + block_length - 4:offset + block_length]
        )[0]
        if trailing_length != block_length:
            raise RuntimeError("pcapng block length fields disagree")
        body = data[offset + 8:offset + block_length - 4]

        if block_type == 1:  # Interface Description Block
            if len(body) < 8:
                raise RuntimeError("truncated pcapng interface block")
            interfaces.append(struct.unpack(endian + "H", body[:2])[0])
        elif block_type == 6:  # Enhanced Packet Block
            if len(body) < 20:
                raise RuntimeError("truncated pcapng enhanced packet block")
            interface_id, _, _, captured_length, _ = struct.unpack(
                endian + "IIIII", body[:20]
            )
            if interface_id >= len(interfaces):
                raise RuntimeError("pcapng packet refers to an unknown interface")
            packet = body[20:20 + captured_length]
            if len(packet) != captured_length:
                raise RuntimeError("truncated packet data in pcapng block")
            frame_number += 1
            yield frame_number, interfaces[interface_id], packet
        elif block_type == 3:  # Simple Packet Block
            if not interfaces or len(body) < 4:
                raise RuntimeError("invalid pcapng simple packet block")
            original_length = struct.unpack(endian + "I", body[:4])[0]
            packet = body[4:4 + min(original_length, len(body) - 4)]
            frame_number += 1
            yield frame_number, interfaces[0], packet

        offset += block_length


def tcp_payload(packet, client_ip, server_ip, client_port, server_port):
    if len(packet) < 14:
        return None
    ether_type = struct.unpack("!H", packet[12:14])[0]
    offset = 14
    if ether_type == 0x8100:
        if len(packet) < 18:
            return None
        ether_type = struct.unpack("!H", packet[16:18])[0]
        offset = 18
    if ether_type != 0x0800 or len(packet) < offset + 20:
        return None

    ip_header_length = (packet[offset] & 0x0F) * 4
    if ip_header_length < 20 or len(packet) < offset + ip_header_length:
        return None
    if packet[offset + 9] != 6:
        return None

    source_ip = ".".join(str(byte) for byte in packet[offset + 12:offset + 16])
    destination_ip = ".".join(
        str(byte) for byte in packet[offset + 16:offset + 20]
    )
    tcp_offset = offset + ip_header_length
    if len(packet) < tcp_offset + 20:
        return None
    source_port, destination_port = struct.unpack("!HH", packet[tcp_offset:tcp_offset + 4])
    if (source_ip, destination_ip, source_port, destination_port) != (
        client_ip,
        server_ip,
        client_port,
        server_port,
    ):
        return None

    sequence = struct.unpack("!I", packet[tcp_offset + 4:tcp_offset + 8])[0]
    tcp_header_length = ((packet[tcp_offset + 12] >> 4) & 0x0F) * 4
    payload_offset = tcp_offset + tcp_header_length
    total_length = struct.unpack("!H", packet[offset + 2:offset + 4])[0]
    ip_end = min(len(packet), offset + total_length)
    if tcp_header_length < 20 or payload_offset > ip_end:
        return None
    payload = packet[payload_offset:ip_end]
    return (sequence, payload) if payload else None


def reassemble_client_stream(capture_path, client_ip, server_ip,
                             client_port, server_port):
    segments = []
    for frame_number, link_type, packet in read_pcapng(capture_path):
        if link_type != 1:
            raise RuntimeError(f"unsupported pcapng link type {link_type}")
        segment = tcp_payload(
            packet, client_ip, server_ip, client_port, server_port
        )
        if segment is not None:
            sequence, payload = segment
            segments.append((sequence, payload, frame_number))
    if not segments:
        raise RuntimeError("no client-to-server TCP payload was found")

    segments.sort(key=lambda segment: segment[0])
    first_sequence = segments[0][0]
    stream = bytearray()
    spans = []
    for sequence, payload, frame_number in segments:
        offset = sequence - first_sequence
        if offset > len(stream):
            raise RuntimeError(
                f"TCP stream gap at relative sequence {len(stream)}..{offset}"
            )
        overlap = len(stream) - offset
        if overlap > 0:
            compared = min(overlap, len(payload))
            if stream[offset:offset + compared] != payload[:compared]:
                raise RuntimeError(
                    f"conflicting TCP retransmission at relative sequence {offset}"
                )
        if overlap < len(payload):
            new_data = payload[max(overlap, 0):]
            span_start = len(stream)
            stream.extend(new_data)
            spans.append((span_start, len(stream), frame_number))
    return bytes(stream), spans


def write_frame_spans(spans, handshake_length, output_path):
    with output_path.open("w", encoding="utf-8") as output:
        output.write("start\tend\tframe\n")
        for start, end, frame in spans:
            if end <= handshake_length:
                continue
            rtmp_start = max(start, handshake_length) - handshake_length
            rtmp_end = end - handshake_length
            output.write(f"{rtmp_start}\t{rtmp_end}\t{frame}\n")


def main():
    parser = argparse.ArgumentParser(
        description="Prepare an RTMP TCP byte stream and Wireshark reference table"
    )
    parser.add_argument("--capture", required=True, type=Path)
    parser.add_argument("--wireshark-json", required=True, type=Path)
    parser.add_argument("--stream-output", required=True, type=Path)
    parser.add_argument("--reference-output", required=True, type=Path)
    parser.add_argument("--frame-map-output", required=True, type=Path)
    parser.add_argument("--client-ip", default="172.30.80.1")
    parser.add_argument("--server-ip", default="172.30.95.121")
    parser.add_argument("--client-port", default=51946, type=int)
    parser.add_argument("--server-port", default=1935, type=int)
    arguments = parser.parse_args()

    tcp_stream, spans = reassemble_client_stream(
        arguments.capture,
        arguments.client_ip,
        arguments.server_ip,
        arguments.client_port,
        arguments.server_port,
    )

    handshake_length = 1537 + 1536  # C0+C1 followed later by C2
    if len(tcp_stream) <= handshake_length or tcp_stream[0] != 3:
        raise RuntimeError("client stream does not contain a complete RTMP handshake")
    rtmp_stream = tcp_stream[handshake_length:]
    arguments.stream_output.write_bytes(rtmp_stream)
    write_frame_spans(spans, handshake_length, arguments.frame_map_output)
    header_count = extract_wireshark_headers(
        arguments.wireshark_json, arguments.reference_output
    )
    print(
        f"prepared {len(rtmp_stream)} RTMP bytes and "
        f"{header_count} Wireshark headers"
    )


if __name__ == "__main__":
    main()
