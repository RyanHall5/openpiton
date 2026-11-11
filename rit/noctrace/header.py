"""OpenPiton NoC header-flit decoding.

Layout from piton/design/include/define.h.pyv (header flit 1, 64 bits):
    [63:50] dst chip id   [49:42] dst x   [41:34] dst y   [33:30] fbits
    [29:22] payload length (body flits after the header)
    [21:14] message type  [13:6] MSHR id  [5:0] options
The routers themselves only look at chip id, x, y, fbits and length
(dynamic_input_top_4.v / dynamic_input_route_request_calc.v).
"""
import re

# fbits -> final direction once the packet reaches dst (x, y)
# (dynamic_input_route_request_calc.v: FINAL_NONE/WEST/SOUTH/EAST/NORTH).
# If fbits[3] is set the router treats it as FINAL_NONE (deliver to tile).
FINAL_DIR = {0b000: "P", 0b010: "W", 0b011: "S", 0b100: "E", 0b101: "N"}

# Generated from define.h.pyv (`define MSG_TYPE_* 8'dN). Override with
# load_msg_types(path) if your OpenPiton version differs.
MSG_TYPES = {
    0: "RESERVED",
    1: "PREFETCH_REQ",
    2: "STORE_REQ",
    3: "BLK_STORE_REQ",
    4: "BLKINIT_STORE_REQ",
    5: "CAS_REQ",
    6: "CAS_P1_REQ",
    7: "CAS_P2Y_REQ",
    8: "CAS_P2N_REQ",
    9: "SWAP_REQ",
    10: "SWAP_P1_REQ",
    11: "SWAP_P2_REQ",
    12: "WB_REQ",
    13: "WBGUARD_REQ",
    14: "NC_LOAD_REQ",
    15: "NC_STORE_REQ",
    16: "LOAD_FWD",
    17: "STORE_FWD",
    18: "INV_FWD",
    19: "LOAD_MEM",
    20: "STORE_MEM",
    21: "LOAD_FWDACK",
    22: "STORE_FWDACK",
    23: "INV_FWDACK",
    24: "LOAD_MEM_ACK",
    25: "STORE_MEM_ACK",
    26: "NC_LOAD_MEM_ACK",
    27: "NC_STORE_MEM_ACK",
    28: "NODATA_ACK",
    29: "DATA_ACK",
    30: "ERROR",
    31: "LOAD_REQ",
    32: "INTERRUPT_FWD",
    33: "INTERRUPT",
    34: "L2_LINE_FLUSH_REQ",
    35: "L2_DIS_FLUSH_REQ",
    36: "AMO_ADD_REQ",
    37: "AMO_AND_REQ",
    38: "AMO_OR_REQ",
    39: "AMO_XOR_REQ",
    40: "AMO_MAX_REQ",
    41: "AMO_MAXU_REQ",
    42: "AMO_MIN_REQ",
    43: "AMO_MINU_REQ",
    44: "AMO_ADD_P1_REQ",
    45: "AMO_AND_P1_REQ",
    46: "AMO_OR_P1_REQ",
    47: "AMO_XOR_P1_REQ",
    48: "AMO_MAX_P1_REQ",
    49: "AMO_MAXU_P1_REQ",
    50: "AMO_MIN_P1_REQ",
    51: "AMO_MINU_P1_REQ",
    52: "AMO_ADD_P2_REQ",
    53: "AMO_AND_P2_REQ",
    54: "AMO_OR_P2_REQ",
    55: "AMO_XOR_P2_REQ",
    56: "AMO_MAX_P2_REQ",
    57: "AMO_MAXU_P2_REQ",
    58: "AMO_MIN_P2_REQ",
    59: "AMO_MINU_P2_REQ",
    60: "LR_REQ",
    61: "LOAD_NOSHARE_REQ",
    62: "SWAPWB_REQ",
    63: "SWAPWB_P1_REQ",
    64: "SWAPWB_P2_REQ",
}

# Message types that originate off chip (memory controller / chipset). Used to
# guess the source when the trace has no injection events (router_mon logs).
OFFCHIP_ORIGIN_TYPES = {24, 25, 26, 27}


def load_msg_types(define_path):
    """Read MSG_TYPE_* values from an OpenPiton define.h.pyv / define.tmp.h."""
    pat = re.compile(r"`define\s+MSG_TYPE_([A-Z0-9_]+)\s+8'd(\d+)")
    out = {}
    with open(define_path) as f:
        for line in f:
            m = pat.search(line)
            if m and int(m.group(2)) not in out:
                out[int(m.group(2))] = m.group(1)
    return out


def decode_header(flit):
    """Decode a 64-bit header flit (Python int) into a dict of fields."""
    flit = int(flit)
    return {
        "dst_chip": (flit >> 50) & 0x3FFF,
        "dst_x": (flit >> 42) & 0xFF,
        "dst_y": (flit >> 34) & 0xFF,
        "fbits": (flit >> 30) & 0xF,
        "length": (flit >> 22) & 0xFF,
        "msg_type": (flit >> 14) & 0xFF,
        "mshr": (flit >> 6) & 0xFF,
        "options": flit & 0x3F,
    }


def final_dir(fbits):
    if fbits & 0b1000:
        return "P"
    return FINAL_DIR.get(fbits & 0b111, "P")
